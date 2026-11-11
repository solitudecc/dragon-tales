#!/usr/bin/env python3
"""Exact saved-stream audit for standalone prefix v3; no native executions."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path


def load(path, name, expected):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
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
        assert source.read(1) == b'', str(path)
    return {'path': str(path), 'frames': count, 'plain_sha256': digest.hexdigest(),
            'gzip_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--v2-root', type=Path, required=True)
    parser.add_argument('--canonical-model', type=Path, required=True)
    args = parser.parse_args()
    root = args.root
    model = load(root / 'guarded_entry_prefix_oracle_v2.py', 'frozen_prefix_model',
                 '7ae9bb618359e0fa3e5ec2aaffb83ed2fd86f02d5478a9b0366a45db7f862050')
    terminal = load(root / 'terminal/guarded_entry_terminal_oracle.py', 'terminal_model',
                    'eb13c260ca10e2f32ceb49a3c07b747f810903caec4b17153adf051371b1dbde')
    canonical = load(args.canonical_model, 'canonical_model',
                     '0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c')
    report = {'streams': [], 'native_execution_added': 0}
    assert (root / 'source-tool-before.sha256').read_bytes() == (root / 'source-tool-after.sha256').read_bytes()
    for path in (root / 'before').iterdir():
        if path.is_file():
            assert path.read_bytes() == (args.v2_root / 'derived-no-dead-latch/before' / path.name).read_bytes()
    for opt in (0, 2):
        result = root / f'results-O{opt}'
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
    term = root / 'terminal'
    for prefix in ('source', 'tool'):
        def pins(path):
            return [(row.split()[0], Path(row.split()[1]).name) for row in path.read_text().splitlines()]
        # Source-before ran one directory above source-after. Bind identical
        # digest + basename, preserving the original path spelling in evidence.
        assert pins(term / (prefix + '-before.sha256')) == pins(term / (prefix + '-after.sha256'))
    for mode in terminal.MODES:
        assert (term / f'logs/{mode}-old-v2.exit').read_text().strip() == '3'
        assert not (term / 'old-v2' / (mode + '.ll')).exists()
        for budget in ('6-256', '32-2048'):
            assert (term / f'logs/{mode}-{budget}.exit').read_text().strip() == '0'
    for control in ('shared', 'budget'):
        assert (term / f'logs/reject-{control}.exit').read_text().strip() == '3'
        assert not (term / f'reject-{control}-output.ll').exists()
    for opt in (0, 2):
        result = term / f'results-O{opt}'
        numbers = json.loads((result / 'report.json').read_text())
        assert numbers['normal'] == 64 and numbers['fault_observations'] == 48
        assert numbers['comparator_controls'] == 640 and not numbers['failures']
        assert numbers['faults'] == {'wrong_default': {'required': 7, 'caught': 7},
                                    'drop_store': {'required': 8, 'caught': 8},
                                    'wrong_phi': {'required': 2, 'caught': 2}}
        expected_files = set()
        for stage in ('before', 'after'):
            for mode in terminal.MODES:
                path = result / f'{stage}-{mode}.snapshots.gz'
                expected_files.add(path.name)
                report['streams'].append(stream(path, (terminal.model(key) for key in terminal.INPUTS if key[0] == mode)))
        for fault, mode in terminal.FAULTS.items():
            path = result / f'control-{fault}.snapshots.gz'
            expected_files.add(path.name)
            report['streams'].append(stream(path, (terminal.model(key, fault) for key in terminal.INPUTS if key[0] == mode)))
        assert {p.name for p in result.glob('*.snapshots.gz')} == expected_files
    report.update({'ok': True, 'audited_frames': sum(x['frames'] for x in report['streams']),
                   'scope': 'standalone v3 synthetic; no production or real-case native claim'})
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
