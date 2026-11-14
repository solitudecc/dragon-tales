#!/usr/bin/env python3
"""Saved-only complete-byte audit of portable Tigress before calibration.

No compiler, SDK, Unicorn or target is invoked. Original pure comparator
method bodies are reused only to independently re-read saved output frames.
"""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runner', type=Path, required=True)
    ap.add_argument('--runner-sha256', required=True)
    ap.add_argument('--plan', type=Path, required=True)
    ap.add_argument('--plan-sha256', required=True)
    ap.add_argument('--run', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    require(not args.out.exists(), 'audit receipt already exists')
    require(digest(args.runner) == args.runner_sha256, 'runner hash differs before import')
    spec = importlib.util.spec_from_file_location('portable_native_before_reader', args.runner)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(module.sha(args.runner) == args.runner_sha256, 'runner hash differs')
    require(module.sha(args.plan) == args.plan_sha256, 'plan hash differs')
    plan = json.loads(args.plan.read_text())
    result = json.loads((args.run / 'RESULTS.json').read_text())
    guard = json.loads((args.run / 'GUARD.json').read_text())
    require(result['correct'] and result['complete'] and result['pins_unchanged'], 'calibration not complete')
    require(result['plan_sha256'] == args.plan_sha256 and guard['plan_sha256'] == args.plan_sha256,
            'calibration/guard plan binding differs')
    require(guard['returncode'] == 0 and guard['failure'] is None, 'outer guard failed')
    require(result['native_comparisons'] == 32 and result['comparator_controls'] == 96,
            'native/control counts differ')
    require(result['skipped'] == result['undefined_exclusions'] == result['normalizations'] == 0,
            'comparison domain was reduced')
    require(module.recheck(result['source_pins']), 'an input/source/tool/runtime changed')
    source = Path(plan['source']['root'])
    adapter, base = module.local_adapter(source)
    expected, mutate = module.comparison_helpers(source, base)
    controls = {(row['host_llvm_level'], row['trial'], row['field']): row for row in result['controls']}
    rows = {(row['host_llvm_level'], row['trial']): row for row in result['results']}
    require(len(controls) == 96 and len(rows) == 32, 'duplicate/missing result/control key')
    outputs = []
    for level in ('O0', 'O2'):
        for item in plan['inputs']:
            trial = item['trial']
            input_path = args.run / level / f'input-{trial}.bin'
            output_path = args.run / level / f'output-{trial}.bin'
            original_input = Path(item['input']['path'])
            original_expected = Path(item['expected']['path'])
            require(input_path.read_bytes() == original_input.read_bytes(), 'saved input differs')
            eb, ab = original_expected.read_bytes(), output_path.read_bytes()
            require(ab == eb, 'saved complete66136byte output differs')
            want, actual = expected(eb), module.decode_actual(ab)
            differences, record, root = module.complete_compare(adapter, want, actual, ab, eb)
            require(not differences and root and record == module.ROOT_EXIT, 'saved full-state/root comparison differs')
            row = rows[(level, trial)]
            require(row['status'] == 'match' and row['raw_snapshot_equal'] and row['root_return']
                    and row['input_sha256'] == module.sha(input_path)
                    and row['output_sha256'] == module.sha(output_path), 'saved row/hash differs')
            for field in ('RAX', 'memory', 'exit'):
                fresh = mutate(adapter, want, actual, field)
                stored = controls[(level, trial, field)]
                require(fresh['field'] == stored['field'] and
                        [d['field'] for d in stored['differences']] == [field], 'exact-field control differs')
            outputs.append(dict(level=level, trial=trial, sha256=module.sha(output_path), bytes=len(ab)))
    module.save(args.out, dict(schema='tigress-dispatch-native-saved-audit-v1', passed=True,
        result_sha256=module.sha(args.run / 'RESULTS.json'), plan_sha256=args.plan_sha256,
        saved_outputs_checked=32, saved_inputs_checked=32, exact_field_controls_rechecked=96,
        raw_bytes_per_output=66136, register_fields=76, Guest_bytes=65536,
        exclusions=0, skips=0, normalization=0, new_native_runs=0, new_Unicorn_runs=0,
        old_C_UC_counts_recounted=0, outputs=outputs))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
