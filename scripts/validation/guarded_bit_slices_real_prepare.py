#!/usr/bin/env python3
"""Prepare sealed59 + one bit-slice diagnostic. Never compile or run a pass."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

ARCHIVE59 = 'dfd1e910bc28c282b786a2a06e8a5194157b52cf28948dcc906b4d9fda5e9c43'
AUDIT_V2 = '2b21f62fcacc6d0a0cfbdf1f454fb75314beb78319015c6be7e4b8061699bb0c'
CASES = ('f-cff-O0', 'f-cff-O2', 'f-combo-O0', 'f-combo-O2')
EXPECTED_SOURCES = {
    'guarded_checked_import_reference.cpp': '339e87e4434661dd183d8bbecb691138f6abb1045eac433720e60b9cd4292081',
    'guarded_business_inventory.cpp': 'd1ae25da9272b39d776515019d32e9fcc6591890806d67320d9968ff2de7d9fe',
}
SOURCES = tuple(EXPECTED_SOURCES) + (
    'guarded_bit_slices_real_probe.cpp',
    'guarded_bit_slices_real_prepare.py',
    'guarded_bit_slices_real_probe_run.py',
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pin(path):
    return dict(path=str(path), size=path.stat().st_size, sha256=sha(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--sealed59', type=Path, required=True)
    parser.add_argument('--audit-v2', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--diagnostic-out', type=Path, required=True)
    args = parser.parse_args()
    if args.prepared.exists() or args.diagnostic_out.exists():
        raise RuntimeError('refuse to overwrite preparation or diagnostic')
    archive59 = args.sealed59.with_suffix('.tar.gz')
    audit_archive = args.audit_v2.with_suffix('.tar.gz')
    if sha(archive59) != ARCHIVE59 or sha(audit_archive) != AUDIT_V2:
        raise RuntimeError('frozen complete59 or repaired v2 observer archive differs')
    manifest59 = json.loads((args.sealed59 / 'ARCHIVE-MANIFEST-v1.json').read_text())
    audit_manifest = json.loads((args.audit_v2 / 'MANIFEST.json').read_text())
    if audit_manifest['passes_executed'] != 0 or audit_manifest['native_executions'] != 0:
        raise RuntimeError('observer archive has unexpected execution scope')
    summary = json.loads((args.audit_v2 / 'FINAL-SUMMARY-v2.json').read_text())
    if not summary['old_type_collision_complete_CLI_false_acceptance_preserved'] or not summary['old_straight_complete_CLI_refusal_preserved']:
        raise RuntimeError('the repaired v2 observer/failure bindings are missing')

    def checked(root, manifest, relative):
        path = root / relative
        actual = pin(path)
        row = manifest['files'][relative]
        if (actual['size'], actual['sha256']) != (row['size'], row['sha256']):
            raise RuntimeError('frozen payload differs: ' + str(path))
        return actual

    sources = []
    for name in SOURCES:
        item = pin(args.source_dir / name)
        if name in EXPECTED_SOURCES and item['sha256'] != EXPECTED_SOURCES[name]:
            raise RuntimeError('unchanged independent source differs: ' + name)
        sources.append(item)
    cases = []
    for case in CASES:
        raw = checked(args.sealed59, manifest59, f'probes-v1/{case}/checked59.after.ll')
        inventory = checked(args.sealed59, manifest59, f'probes-v1/{case}/checked59.after.json')
        manifest = checked(args.sealed59, manifest59, f'probes-v1/{case}/checked59.manifest.json')
        binding = checked(args.sealed59, manifest59, f'probes-v1/{case}/checked59.bindings.json')
        # The repaired observer independently archived raw final59, inventory,
        # original D37 anchors and inherited V predictions. Compare complete
        # raw payloads with the producer, without text or metadata normalization.
        audit_raw = checked(args.audit_v2, audit_manifest, f'real-inputs-v2/{case}.after-ll.ll')
        audit_inventory = checked(args.audit_v2, audit_manifest, f'real-inputs-v2/{case}.after.json')
        if (raw['size'], raw['sha256']) != (audit_raw['size'], audit_raw['sha256']):
            raise RuntimeError('actual59 raw differs from repaired observer input')
        if (inventory['size'], inventory['sha256']) != (audit_inventory['size'], audit_inventory['sha256']):
            raise RuntimeError('actual59 inventory differs from repaired observer input')
        anchor = checked(args.audit_v2, audit_manifest, f'real-inputs-v2/{case}.source-anchor.json')
        hints = checked(args.audit_v2, audit_manifest, f'real-inputs-v2/{case}.hints.json')
        directory = args.diagnostic_out / case
        expected = directory / 'bits-import'
        prefix = directory / 'bits60'
        cases.append(dict(case=case, source59_raw=raw, source59_inventory=inventory,
            source59_manifest=manifest, source59_binding=binding,
            audit_v2_raw=audit_raw, audit_v2_inventory=audit_inventory,
            original_D37_anchor=anchor, inherited_V_predictions=hints,
            independent_reference_command=[str(args.prepared / 'parse-import'), raw['path'],
                str(Path(raw['path']).with_suffix('')), str(expected)],
            candidate_command=[str(args.prepared / 'probe-bits'), raw['path'],
                str(expected) + '.expected.ll', str(expected) + '.expected.json',
                inventory['path'], str(prefix)],
            declared_inherited_runs=59, new_candidate_runs=1,
            executed_candidate_runs=0, executed_native_runs=0))
    args.prepared.mkdir(parents=True)
    destination = args.prepared / 'source'
    destination.mkdir()
    for item in sources:
        shutil.copyfile(item['path'], destination / Path(item['path']).name)
    document = dict(schema='guarded-bit-slices-real60-preparation-v1', status='prepared-not-executed',
        diagnostic_scope='sealed actual59 final then guarded-bit-slices once per case',
        full_recipe=['original D37', 'original base19', 'unchanged M1', 'unchanged V1',
            'already executed checked1', 'bit-slices1'], full_profile_pass_count=60,
        original_C59_reruns=0, additional_cleanup_runs=0,
        new_pass_options=dict(max_candidates=64, max_rewrites=32, max_blocks=128, max_instructions=16384),
        per_command_timeout_seconds=180, running_disk_floor_bytes=67108864,
        diagnostic_minimum_start_free_bytes=536870912,
        full_minimum_start_free_bytes=1073741824,
        runtime=dict(status='awaiting-root-formal-runtime-SHA', candidate_execution_authorized=False),
        binaries=dict(status='not-compiled', required=['parse-import', 'probe-bits']),
        source59_archive=pin(archive59), observer_v2_archive=pin(audit_archive),
        source59_old58_precision_gate=[True, True, True, False],
        corrected_checked59_structure_gate='retain repaired observer result; never replace with v1 default',
        old_metrics_and_original_failures_retained=True,
        sources=sources, copied_source_files=[pin(destination / Path(p['path']).name) for p in sources],
        cases=cases, import_contract=dict(
            reference_source_sha256=EXPECTED_SOURCES['guarded_checked_import_reference.cpp'],
            module_identifier='partial-masks-values-real', reference_pass_or_native_runs=0,
            before_candidate='all actual full-render LL/JSON equal independent expectation and sealed59 JSON',
            normalization_or_hidden_retry=False),
        observer_contract=dict(archive_sha256=AUDIT_V2, v1_default_forbidden=True,
            frozen_v2_manifest=pin(args.audit_v2 / 'MANIFEST.json'),
            full_v2_summary=pin(args.audit_v2 / 'FINAL-SUMMARY-v2.json'),
            required_actual_frames=['original D37 anchor', 'actual59 before', 'actual60 after'],
            automatic_observer_execution=False),
        future_full_scope='unchanged C16/LLVM16 + full1MiB Guest + seven Exit +144 controls; not executed or authorized',
        future_full_required_gates=['new both-runtime native validation',
            'independent repaired strict structure all four', 'formal runtime/source SHA', 'original1GiB storage gate'],
        observed_free_bytes=shutil.disk_usage(args.prepared).free,
        executed_candidate_runs=0, executed_native_runs=0, executed_full_replays=0)
    (args.prepared / 'PREPARED-NOT-EXECUTED.json').write_text(json.dumps(document, indent=2) + '\n')
    print(json.dumps(dict(prepared=str(args.prepared), cases=len(cases), candidate_runs=0, native_runs=0)))


if __name__ == '__main__':
    main()
