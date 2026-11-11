#!/usr/bin/env python3
"""Read-only audit of saved native streams against their frozen byte models.

No optimizer, target program or native code is executed. Frozen model modules
are the source snapshots already SHA-bound by each saved runner report.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def typed_key(key):
    if not isinstance(key, (list, tuple)) or not key or type(key[0]) is not str:
        raise RuntimeError('malformed native input key')
    if any(type(v) is not int for v in key[1:]):
        raise RuntimeError('bool/float/non-integer native input key')
    return tuple(key)


def load_model(path, label):
    old_path = list(sys.path)
    old_modules = dict(sys.modules)
    try:
        sys.path.insert(0, str(path.parent))
        # Each group imports its own frozen source snapshots, not whatever a
        # prior group left cached under the same producer module name.
        for name in ('loop_carried_context_diff', 'state_guard_diff',
                     'guest_memory_checked_edges_diff'):
            sys.modules.pop(name, None)
        spec = importlib.util.spec_from_file_location('frozen_model_' + label, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path[:] = old_path
        for name in list(sys.modules):
            if name not in old_modules:
                del sys.modules[name]
        for name, module in old_modules.items():
            sys.modules[name] = module


def audit_group(work):
    report = json.loads((work / 'report.json').read_text())
    for name, pin in report['artifact_sha256'].items():
        if digest(work / name) != pin:
            raise RuntimeError('artifact changed: ' + str(work / name))
    for name, pin in report['source_sha256'].items():
        if digest(work / 'sources' / name) != pin:
            raise RuntimeError('source changed: ' + name)
    if report.get('unavailable'):
        if not report['ok'] or report['normal_comparisons'] or report['control_comparisons']:
            raise RuntimeError('unavailable is not zero-execution success')
        return dict(group=work.name, kind='option unavailable', observations=0, fault_observations=0,
                    native_executions=0, analysis_runs=0, artifact_files=len(report['artifact_sha256']))
    if not report['ok']:
        raise RuntimeError('not a completed normal group: ' + work.name)
    kind = 'store' if (work / 'sources/store_context_diff.py').exists() else 'hub'
    module = load_model(work / 'sources' / ('store_context_diff.py' if kind == 'store' else 'guest_memory_hub_diff.py'), work.name.replace('-', '_'))
    inputs = tuple(typed_key(key) for key in module.INPUTS)
    if len(inputs) != len(set(inputs)) or report['inputs'] != len(inputs):
        raise RuntimeError('missing/extra/duplicate requested input coverage')
    expected = [module.model(key) for key in inputs]
    size = module.SNAPSHOT_BYTES
    if any(len(value) != size for value in expected):
        raise RuntimeError('bad model snapshot length')
    with gzip.open(work / 'expected.bin.gz', 'rb') as f:
        for value in expected:
            if f.read(size) != value:
                raise RuntimeError('saved expected bytes differ from frozen model')
        if f.read(1):
            raise RuntimeError('extra expected snapshot bytes')
    rows = {}
    for row in report['rows']:
        key = typed_key(row['key'])
        stage = ('normal', row['stage']) if 'stage' in row else ('control', row['control'])
        lookup = (row['level'], *stage, key)
        if lookup in rows:
            raise RuntimeError('duplicate result row')
        rows[lookup] = row
    visited = set()
    normal_count = control_count = fault_count = 0
    for level in ('O0', 'O2'):
        for stage in ('before', 'after'):
            with gzip.open(work / (stage + '-' + level + '.bin.gz'), 'rb') as f:
                for key, wanted in zip(inputs, expected):
                    actual = f.read(size)
                    lookup = (level, 'normal', stage, key)
                    row = rows[lookup]
                    visited.add(lookup)
                    if actual != wanted or row['fields'] or hashlib.sha256(actual).hexdigest() != row['snapshot_sha256']:
                        raise RuntimeError('normal full snapshot mismatch: ' + str(lookup))
                    normal_count += 1
                if f.read(1):
                    raise RuntimeError('extra normal snapshot bytes')
        for control in module.CONTROLS:
            target = ('descriptor' if control == 'stale_size' else 'history') if kind == 'store' else (
                'loop' if control == 'reentry' else 'history' if control == 'history' else 'plain')
            keys = [key for key in inputs if key[0] == target]
            with gzip.open(work / ('control-' + control + '-' + level + '.bin.gz'), 'rb') as f:
                for key in keys:
                    actual = f.read(size)
                    correct, faulty = module.model(key), module.model(key, control)
                    lookup = (level, 'control', control, key)
                    row = rows[lookup]
                    visited.add(lookup)
                    fields = module.differences(faulty, correct)
                    if actual != faulty or fields != row['fields'] or hashlib.sha256(actual).hexdigest() != row['snapshot_sha256']:
                        raise RuntimeError('specified fault full snapshot mismatch: ' + str(lookup))
                    control_count += 1
                    fault_count += bool(fields)
                if f.read(1):
                    raise RuntimeError('extra faulty snapshot bytes')
    if visited != set(rows):
        raise RuntimeError('missing/extra requested result rows')
    if (normal_count != report['normal_comparisons'] or control_count != report['control_comparisons']
            or fault_count != report['controls_required'] or fault_count != report['controls_detected']
            or report['normal_mismatches'] or any(report[name] for name in ('excluded', 'skipped', 'normalized'))):
        raise RuntimeError('summary coverage differs from full raw streams')
    return dict(group=work.name, kind=kind, ok=True, observations=normal_count,
                fault_observations=control_count, specified_faults=fault_count,
                normal_streams=4, artifact_files=len(report['artifact_sha256']),
                library=report['library_sha256'], report_sha256=digest(work / 'report.json'),
                native_executions=0, analysis_runs=0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('groups', type=Path, nargs='+')
    a = p.parse_args()
    results = [audit_group(path.resolve()) for path in a.groups]
    report = dict(ok=True, kind='saved stream/source/model audit', rows=results,
                  new_native_executions=0, new_analysis_runs=0)
    a.out.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
