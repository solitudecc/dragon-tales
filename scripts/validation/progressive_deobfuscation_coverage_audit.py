"""Saved-only machine-table coverage gate; never imports Dragon or runs a pass.

An independent, explicit machine manifest is a coverage oracle, not a trusted
Guest-memory fact.  Matching this gate would still not prove equivalence or
removal of the dispatcher.  Existing collector files are never modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
FILES = {'REPORT.json', 'GUARD.json', 'pass-report.json', 'before.ll', 'after.ll',
         'before.cfg.json', 'after.cfg.json', 'graph.before.json', 'graph.after.json',
         'worker.stdout', 'loader.stderr', 'before.verify.stdout',
         'before.verify.stderr', 'after.verify.stdout', 'after.verify.stderr'}


def require(value, message):
    if not value:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def inventory(root):
    paths = [p for p in root.rglob('*') if p.is_file()]
    require({str(p.relative_to(root)) for p in paths} == FILES,
            'unexpected saved file set')
    require(not any(p.is_symlink() for p in root.rglob('*')), 'symlink in saved results')
    return {p.name: dict(bytes=p.stat().st_size, sha256=digest(p)) for p in sorted(paths)}


def audit(root, machine):
    initial = inventory(root)
    report, guard = read(root / 'REPORT.json'), read(root / 'GUARD.json')
    require(report['ok'] and guard['returncode'] == 0 and guard['failure'] is None,
            'saved ordinary baseline did not complete')
    require(report['semantic_validation'] is False and report['native_runs'] == 0
            and report['c_runs'] == 0 and report['unicorn_runs'] == 0
            and report['checked_component_calls'] == 0, 'scope changed')
    elf = str(Path(machine['elf']['vm_path']))
    require(report['inputs'].get(elf) == machine['elf']['sha256'], 'ELF binding differs')
    graph, after = read(root / 'graph.before.json'), read(root / 'graph.after.json')
    require(graph == after, 'source Graph changed')
    require(initial['before.ll']['sha256'] == machine['observed_before_ir_sha256'],
            'observed raw IR changed from the frozen first measurement')
    instructions = {row['address']: row for row in graph['instructions']}
    require(len(instructions) == len(graph['instructions']), 'duplicate machine instruction')
    jump = machine['jump']
    require(instructions[jump['address']]['bytes'] == jump['bytes'], 'jump bytes differ')
    expected = set(machine['table']['targets'])
    require(len(machine['table']['targets']) == machine['table']['slots'], 'slot count differs')
    require(len(expected) == machine['table']['unique_targets'], 'unique count differs')
    require(expected.issubset(instructions), 'some expected target is not decoded')
    actual = set(instructions[jump['address']]['branches'])
    functions = [f for f in graph['functions'] if f['address'] == machine['kernel']['entry']]
    require(len(functions) == 1 and functions[0]['valid'], 'kernel Function missing')
    members = set(functions[0]['blocks'])
    missing_edges, missing_members = sorted(expected - actual), sorted(expected - members)
    coverage = not missing_edges and not missing_members
    ir_function = 'dragon_static_entry_x86_64_' + format(machine['kernel']['entry'], 'x')
    cfg = {}
    for stage in ('before', 'after'):
        row = read(root / (stage + '.cfg.json'))['functions'][ir_function]
        cfg[stage] = dict(blocks=row['block_count'], successor_slots=row['successor_slots'],
                          cyclic_component_sizes=[len(g) for g in row['cyclic_components']],
                          cyclic_switches=row['cyclic_switches'],
                          operation_counts=row['operation_counts'])
    require(inventory(root) == initial, 'saved baseline changed during audit')
    return dict(root=str(root), saved_file_set=initial, raw_guard_and_verify_passed=True,
                source_graph_unchanged=True, decoded_instructions=len(instructions),
                expected_table_slots=machine['table']['slots'],
                expected_unique_targets=len(expected), actual_jump_branches=sorted(actual),
                covered_jump_targets=len(expected & actual), kernel_machine_blocks=sorted(members),
                expected_targets_in_kernel=len(expected & members),
                missing_jump_targets=missing_edges, missing_kernel_targets=missing_members,
                entry_case_cfg_coverage_gate=coverage,
                precision_complete=False if not coverage else None,
                semantic_validation=False, cfg=cfg, pass_report=report['pass_report'],
                calls=report['calls'], unlifted=report['unlifted'],
                ordinary_baseline_report_sha256=initial['REPORT.json']['sha256'],
                conclusion=('indirect-edge/function coverage incomplete; zero cyclic-switch '
                            'does not measure dispatcher removal' if not coverage else
                            'entry coverage passed; deobfuscation and equivalence remain pending'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--machine-manifest', type=Path, required=True)
    p.add_argument('--machine-manifest-sha256', required=True)
    p.add_argument('--saved', type=Path, action='append', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    require(not a.out.exists(), 'audit output must be new')
    require(digest(a.machine_manifest) == a.machine_manifest_sha256, 'machine manifest hash differs')
    machine = read(a.machine_manifest)
    reports = [audit(root, machine) for root in a.saved]
    require(digest(a.machine_manifest) == a.machine_manifest_sha256, 'machine manifest changed')
    result = dict(schema='dragon-progressive-saved-machine-coverage-audit-v1',
                  checker=dict(path=str(Path(__file__).resolve()), sha256=digest(__file__)),
                  machine_manifest=dict(path=str(a.machine_manifest),
                                        sha256=a.machine_manifest_sha256),
                  new_dragon_calls=0, new_native_runs=0, new_reference_runs=0, runs=reports)
    a.out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
