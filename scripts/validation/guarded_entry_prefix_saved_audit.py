#!/usr/bin/env python3
"""Recheck every saved frame; audit counts never count as native executions."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path


def load(path, name, expected):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, str(path)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stream(path, expected):
    count = 0
    digest = hashlib.sha256()
    with gzip.open(path, 'rb') as source:
        for frame in expected:
            actual = source.read(len(frame))
            assert actual == frame, (str(path), count)
            digest.update(actual)
            count += 1
        assert source.read(1) == b'', ('extra frame data', str(path))
    return {'path': str(path), 'frames': count, 'plain_sha256': digest.hexdigest(),
            'gzip_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--canonical-model', type=Path, required=True)
    args = parser.parse_args()
    root = args.root
    derived = root / 'derived-no-dead-latch'
    old = load(root / 'guarded_entry_prefix_oracle.py', 'original_prefix_model',
               'fe30bdbf6d612e923c2cd1329be549a48dd3ba67bed02089d7ba8ee92109ef17')
    model = load(derived / 'guarded_entry_prefix_oracle_v2.py', 'derived_prefix_model',
                 '7ae9bb618359e0fa3e5ec2aaffb83ed2fd86f02d5478a9b0366a45db7f862050')
    canonical = load(args.canonical_model, 'canonical_model',
                     '0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c')
    report = {'streams': [], 'model_equivalent_inputs': 0, 'native_execution_added': 0}
    for key in model.INPUTS:
        previous = ('diamond' if key[0] == 'diamond_no_dead_latch' else key[0],) + key[1:]
        assert model.model(key) == old.model(previous)
        report['model_equivalent_inputs'] += 1
    for name in ('entry_phi', 'loop', 'fresh') + tuple('control_' + f for f in model.FAULTS):
        assert (derived / 'before' / (name + '.ll')).read_bytes() == (root / 'before' / (name + '.ll')).read_bytes()
    for directory in (root, derived):
        assert (directory / 'source-tool-before.sha256').read_bytes() == (directory / 'source-tool-after.sha256').read_bytes()
    for opt in (0, 2):
        failed = json.loads((root / f'results-O{opt}/report.json').read_text())
        assert failed['normal'] == 0 and failed['fault_observations'] == 0 and failed['failures']
        result = derived / f'results-O{opt}'
        numbers = json.loads((result / 'report.json').read_text())
        assert numbers['normal'] == 512 and numbers['fault_observations'] == 256
        assert numbers['required_faults'] == numbers['detected_faults'] == 82
        assert numbers['comparator_controls'] == 2560 and not numbers['failures']
        expected_files = set()
        for stage in ('before', 'after'):
            for mode in model.MODES:
                path = result / f'{stage}-{mode}.snapshots.gz'
                expected_files.add(path.name)
                report['streams'].append(stream(path, (model.model(key) for key in model.INPUTS if key[0] == mode)))
        for fault in model.FAULTS:
            path = result / f'control-{fault}.snapshots.gz'
            expected_files.add(path.name)
            report['streams'].append(stream(path, (model.model(key, fault) for key in model.INPUTS if key[0] == 'loop')))
        assert {p.name for p in result.glob('*.snapshots.gz')} == expected_files
    compatibility = root / 'loop112-compat'
    numbers = json.loads((compatibility / 'report.json').read_text())
    assert numbers['normal'] == 672 and numbers['comparator_controls'] == 2688 and not numbers['failures']
    keys = tuple(key for key in canonical.INPUTS if key[0] == 'loop')
    assert len(keys) == 112 and [list(key) for key in keys] == numbers['inputs']
    assert len(numbers['stages']) == 6 and all(s['matches'] == 112 for s in numbers['stages'])
    for item in numbers['stages']:
        report['streams'].append(stream(compatibility / (item['label'] + '.snapshots.gz'),
                                       (canonical.model(key, numbers['arena_address']) for key in keys)))
    assert len(list(compatibility.glob('*.snapshots.gz'))) == 6
    admission = json.loads((root / 'compat-admission/report.json').read_text())
    assert len(admission) == 20
    assert all(x['source_unchanged'] and x['exit'] == (0 if x['mode'] == 'loop' else 3) for x in admission)
    for name in ('historical', 'liveout'):
        assert (root / f'logs/reject_{name}.exit').read_text().strip() == '3'
        assert not (root / f'reject_{name}-output.ll').exists()
    report.update({'ok': True, 'original_whole4_gain': False, 'derived_whole4_gain': True,
                   'canonical_whole10_transform': False, 'audited_frames': sum(x['frames'] for x in report['streams'])})
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
