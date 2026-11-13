#!/usr/bin/env python3
"""Freeze profile59 diagnostic sources and typed inputs; never execute a pass."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

ARCHIVE58 = '799f891ace9888c01309ff12a635022425524261536d6e9bfc3355921ed1aa1d'
CASES = ('f-cff-O0', 'f-cff-O2', 'f-combo-O0', 'f-combo-O2')
EXPECTED_SOURCES = {
    'guarded_partial_masks_state_values_real_probe.cpp': '937eaac02d41748fff7f3a10feba32f7aa59c1ffacf64c56a525c1670d27ed21',
    'guarded_partial_masks_state_values_real.py': 'e93aa1eba0193cf54900f776f93e1e9472fe8980e6bbf1c242b6cb8f4f2f74cb',
    'guarded_business_inventory.cpp': 'd1ae25da9272b39d776515019d32e9fcc6591890806d67320d9968ff2de7d9fe',
    'guarded_checked_import_reference.cpp': '339e87e4434661dd183d8bbecb691138f6abb1045eac433720e60b9cd4292081',
    'constant_branches_real.py': 'f4c35425046fd1e5882ef4042ef9e2853130de15af92f2bfcdfe2fdc772b5449',
}
SOURCES = tuple(EXPECTED_SOURCES) + (
    'guarded_checked_corridors_real_probe.cpp',
    'guarded_checked_corridors_real.py',
    'guarded_checked_corridors_real_prepare.py',
    'guarded_checked_corridors_real_probe_run.py',
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pin(path):
    return dict(path=str(path), size=path.stat().st_size, sha256=sha(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--sealed58', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--diagnostic-out', type=Path, required=True)
    args = parser.parse_args()
    if args.prepared.exists() or args.diagnostic_out.exists():
        raise RuntimeError('refuse to overwrite an existing preparation or diagnostic')
    archive = args.sealed58.with_suffix('.tar.gz')
    if sha(archive) != ARCHIVE58:
        raise RuntimeError('the sealed58 archive differs')
    rows = json.loads((args.sealed58 / 'ARCHIVE-MANIFEST.json').read_text())['files']

    def frozen(relative):
        path = args.sealed58 / relative
        actual = pin(path)
        if (actual['size'], actual['sha256']) != (rows[relative]['size'], rows[relative]['sha256']):
            raise RuntimeError('sealed58 payload differs: ' + relative)
        actual['archive_member'] = args.sealed58.name + '/' + relative
        return actual

    source_pins = []
    for name in SOURCES:
        item = pin(args.source_dir / name)
        if name in EXPECTED_SOURCES and item['sha256'] != EXPECTED_SOURCES[name]:
            raise RuntimeError('frozen source changed: ' + name)
        source_pins.append(item)
    cases = []
    for case in CASES:
        raw = frozen(f'probes-v1/{case}/module.value.after.ll')
        inventory = frozen(f'probes-v1/{case}/module.value.after.json')
        manifest = frozen(f'probes-v1/{case}/module.manifest.json')
        anchor = frozen(f'probes-v1/V-audit/anchors/{case}/module.before.json')
        hints = frozen(f'probes-v1/hints/{case}.json')
        directory = args.diagnostic_out / case
        reference = directory / 'import-reference'
        candidate = directory / 'checked59'
        cases.append(dict(
            case=case, source58_raw=raw, source58_inventory=inventory,
            source58_manifest=manifest, original_D37_anchor=anchor,
            inherited_V_predictions=hints,
            independent_reference_command=[str(args.prepared / 'parse-import'), raw['path'],
                str(Path(raw['path']).with_suffix('')), str(reference)],
            candidate_command=[str(args.prepared / 'probe-checked'), raw['path'],
                str(reference) + '.expected.ll', str(reference) + '.expected.json',
                inventory['path'], str(candidate)],
            expected_actual_frames={
                'checked_before_IR': str(candidate) + '.checked.before.ll',
                'checked_before_inventory': str(candidate) + '.checked.before.json',
                'checked_after_IR': str(candidate) + '.checked.after.ll',
                'checked_after_inventory': str(candidate) + '.checked.after.json',
                'final_IR': str(candidate) + '.after.ll'},
            declared_inherited_runs=58, new_candidate_runs=1,
            executed_candidate_runs=0, executed_native_runs=0))
    args.prepared.mkdir(parents=True)
    destination = args.prepared / 'source'
    destination.mkdir()
    for item in source_pins:
        shutil.copyfile(item['path'], destination / Path(item['path']).name)
    document = dict(
        schema='guarded-checked-real59-preparation-v2', status='prepared-not-executed',
        diagnostic_scope='sealed58 V.final then checked1 once per input',
        full_recipe=['original D37', 'original base19', 'unchanged mask1', 'unchanged V1', 'checked1'],
        full_profile_pass_count=59, additional_cleanup_runs=0,
        new_pass_options=dict(max_candidates=64, max_rewrites=32, max_blocks=128, max_instructions=16384),
        per_command_timeout_seconds=180, running_disk_floor_bytes=67108864,
        diagnostic_minimum_start_free_bytes=536870912,
        full_minimum_start_free_bytes=1073741824,
        runtime=dict(status='awaiting-formal-freeze', candidate_execution_authorized=False),
        binaries=dict(status='not-compiled', required=['parse-import', 'probe-checked']),
        source58_archive=pin(archive), source58_original_precision_gate=[True, True, True, False],
        old_metrics_and_original_failures_retained=True,
        sources=source_pins, copied_source_files=[pin(destination / Path(p['path']).name) for p in source_pins],
        cases=cases,
        import_contract={
            'reference_source_sha256': EXPECTED_SOURCES['guarded_checked_import_reference.cpp'],
            'module_identifier': 'partial-masks-values-real',
            'reference_pass_or_native_runs': 0,
            'reference_rc0': 'exact IR and inventory: hardlink the verified source as expectation',
            'reference_rc5': 'retain raw mismatch and emitted expectation; require complete source inventory byte equality',
            'before_candidate': 'C producer compares actual before LL and JSON with independent expectation and source58 JSON',
            'normalization_or_hidden_retry': False},
        structural_contract={
            'old_V4': 'unchanged retained; its same-target rule cannot prove a clone-root loop redirect',
            'new_proof_fields': ['checked_context_proofs', 'checked_context_state_success_metric'],
            'required': ['actual selected-seed changed successor slot', 'all instruction and SSA mappings',
                'historical seed snapshots', 'fresh root loop PHIs', 'duplicate actual edges',
                'actual same-block target LOAD', 'complete same-read full fallback',
                'ordinary side exits', 'all exit PHIs', 'original anchor central coverage']},
        future_full_scope=dict(strategy=0, product_optimization='O0', inputs_per_ELF=4,
            C_cases=16, LLVM_cases=16, specified_fault_controls=144,
            guest_comparison_bytes=1048576, exit_fields=7,
            original_harness_inputs_and_comparator_unchanged=True),
        future_full_required_gates=['new main and no-Z3 native validation',
            'independent checked-aware strict structure proof passes all four diagnostic cases',
            'formal runtime source and backend pins', 'at least 1GiB free before full launch'],
        observed_free_bytes=shutil.disk_usage(args.prepared).free,
        executed_candidate_runs=0, executed_native_runs=0, executed_full_replays=0)
    (args.prepared / 'PREPARED-NOT-EXECUTED.json').write_text(json.dumps(document, indent=2) + '\n')
    print(json.dumps(dict(prepared=str(args.prepared), cases=len(cases), candidate_runs=0, native_runs=0)))


if __name__ == '__main__':
    main()
