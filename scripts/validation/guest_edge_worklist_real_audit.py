#!/usr/bin/env python3
"""Check explicit activation and unchanged default output around existing audits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import bounded_memory_pipeline_real as pipeline
import guest_edge_worklist_real as activation


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def exact_ir(expected, actual):
    if len(expected) != 4 or set(expected) != set(actual):
        raise RuntimeError('default compatibility lost a module')
    names = {'before-pipeline.ll', 'after-pipeline.ll', 'round-1.ll', 'round-2.ll', 'round-3.ll'}
    for entry, files in expected.items():
        if set(files) != names or actual[entry] != files:
            raise RuntimeError('default compatibility changed saved IR')


def self_test():
    import copy
    names = ('before-pipeline.ll', 'after-pipeline.ll', 'round-1.ll', 'round-2.ll', 'round-3.ll')
    good = {str(i): {name: 'abc' for name in names} for i in range(4)}
    exact_ir(good, copy.deepcopy(good))
    bad = []
    x = copy.deepcopy(good); x.pop('0'); bad.append(x)
    x = copy.deepcopy(good); x['0'].pop('round-2.ll'); bad.append(x)
    x = copy.deepcopy(good); x['0']['round-2.ll'] = 'wrong'; bad.append(x)
    for row in bad:
        try: exact_ir(good, row)
        except RuntimeError: continue
        raise RuntimeError('default compatibility fault control was accepted')
    return dict(ok=True, positive=1, rejected_controls=len(bad),
                new_analysis_runs=0, new_native_executions=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--mode', choices=('default', 'enabled'))
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--activation-manifest', type=Path)
    parser.add_argument('--group', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(), indent=2)); return 0
    if any(v is None for v in (args.mode, args.manifest, args.activation_manifest, args.group, args.out)):
        parser.error('--mode --manifest --activation-manifest --group --out required')
    manifest = json.loads(args.manifest.read_text())
    contract = json.loads(args.activation_manifest.read_text())
    if sha(args.manifest) != contract['predeclared_sha256']:
        raise RuntimeError('predeclared gate changed')
    receipt = json.loads((args.group / 'edge-worklist-activation.json').read_text())
    enabled = args.mode == 'enabled'
    if (receipt['enabled'] is not enabled or receipt['backend_status'] != 0 or
            receipt['driver_sha256'] != contract['driver_sha256'] or
            receipt['original_wrapper_sha256'] != manifest['wrapper_sha256']):
        raise RuntimeError('wrong activation or wrapper')
    activation.validate_activation(receipt['records'], enabled)
    summary = json.loads((args.group / 'bounded-memory-summary.json').read_text())
    semantic = json.loads((args.group / 'summary.json').read_text())
    preflight = json.loads((args.group / 'preflight.json').read_text())
    if (not summary['ok'] or not semantic['ok'] or
            semantic['C_statuses'] != {'match': 16} or semantic['LLVM_statuses'] != {'match': 16}):
        raise RuntimeError('full C/LLVM oracle coverage changed')
    controls = semantic['controls']
    if len(controls) != 16 or not all(c['register_detected'] and c['memory_detected'] and
            len(c['exit_fields_detected']) == 7 and all(c['exit_fields_detected']) for c in controls):
        raise RuntimeError('full-state corruption control failed')
    if (preflight['inputs'] != manifest['fixture_sha256'] or summary['harness_sha256'] != manifest['wrapper_sha256'] or
            summary['requested_rounds'] != 3 or summary['max_pass_runs'] != 32 or
            summary['actual_recipe'] != list(pipeline.ACTUAL_STEPS)):
        raise RuntimeError('frozen real recipe changed')
    actual = {}
    for row in summary['rows']:
        key = hex(row['entry'])
        if key in actual: raise RuntimeError('duplicate module')
        pipeline.validate_report(row['report'], 3)
        if row['report']['iterations'] != 3 or row['original_request'] != list(pipeline.LEGACY_REQUEST):
            raise RuntimeError('rounds or original oracle request changed')
        work = args.group / row['work'].split('/' + args.group.name + '/', 1)[1]
        actual[key] = {name: sha(work / name) for name in contract['default_ir_sha256'][key]}
        if actual[key]['before-pipeline.ll'] != manifest['baseline_indices'][key]['before_sha256']:
            raise RuntimeError('lifted input changed')
    if set(actual) != set(manifest['baseline_indices']): raise RuntimeError('missing/extra module')
    if not enabled:
        exact_ir(contract['default_ir_sha256'], actual)
    scripts = Path(__file__).parent
    checks = [('original-audit', scripts / 'bounded_memory_pipeline_audit.py', [])]
    if enabled:
        checks.append(('precision', scripts / 'guest_pointer_phi_real_review.py', ['--manifest', str(args.manifest)]))
    reports = {}
    for label, script, extra in checks:
        expected = (manifest['original_switch_audit_sha256'] if label == 'original-audit'
                    else manifest['precision_review_sha256'])
        if sha(script) != expected: raise RuntimeError('original comparator changed')
        out = args.out.with_name(args.out.stem + '-' + label + '.json')
        command = [sys.executable, str(script), '--group', str(args.group), '--out', str(out), *extra]
        subprocess.run(command, check=True)
        reports[label] = dict(path=str(out), sha256=sha(out))
    result = dict(ok=True, mode=args.mode, predeclared_sha256=sha(args.manifest),
                  activation_manifest_sha256=sha(args.activation_manifest),
                  C_matches=16, LLVM_matches=16, register_controls=16, memory_controls=16,
                  Exit_field_controls=112, default_ir_byte_equal_count=0 if enabled else 20,
                  checks=reports, exclusions=[], skips=[], normalizations=[],
                  new_analysis_runs=0, new_native_executions=0)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(result, indent=2)); return 0


if __name__ == '__main__':
    raise SystemExit(main())
