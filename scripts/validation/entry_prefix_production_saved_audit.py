#!/usr/bin/env python3
"""Audit production-API identities, status, pins, and every saved native frame."""
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
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--runtime-flavor', choices=('main', 'noz3'), default='main')
    args = parser.parse_args()
    root, reference = args.root, args.reference
    model = load(root / 'guarded_entry_prefix_oracle_v2.py', 'frozen_prefix_model',
                 '7ae9bb618359e0fa3e5ec2aaffb83ed2fd86f02d5478a9b0366a45db7f862050')
    terminal = load(root / 'terminal/guarded_entry_terminal_oracle.py', 'terminal_model',
                    'eb13c260ca10e2f32ceb49a3c07b747f810903caec4b17153adf051371b1dbde')
    canonical = load(root / 'reference-snapshot/canonical_guarded_entry_diff.py', 'canonical_model',
                     '0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c')
    report = {'streams': [], 'driver_checks': [], 'native_execution_added': 0}
    pins = (root / 'source-runtime-before.sha256').read_text()
    assert pins == (root / 'source-runtime-after.sha256').read_text()
    runtime_pins = {
        'main': ('e5d15ecdc1a8ca789169635e335ee1f3ee5c336209a364a0ebaea7526d4d08da',
                 '953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84'),
        'noz3': ('890781cb533d9019aadd153212a13493535463c92b130000cc6cf0f64e9a7627',
                 '061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0'),
    }
    assert [row.split()[0] for row in pins.splitlines()] == [
        'c3de98f01ba23cd2bd68145df2ac4a86e289ebe68f4ac41a6900b5d9551cd941',
        *runtime_pins[args.runtime_flavor]]
    assert f'runtime-{args.runtime_flavor}-v1/libdragon.so' in (root / 'driver-ldd.txt').read_text()
    if args.runtime_flavor == 'noz3':
        binaries = (root / 'unchanged-driver-binary.sha256').read_text().splitlines()
        assert len(binaries) == 2 and binaries[0].split()[0] == binaries[1].split()[0]
        assert hashlib.sha256((root / 'prefix-production').read_bytes()).hexdigest() == binaries[0].split()[0]
        assert (root / 'build-noz3-option.txt').read_text().strip() == 'DRAGON_USE_Z3:BOOL=OFF'
    for directory in ('before', 'terminal/before'):
        for path in (root / directory).iterdir():
            if path.is_file():
                assert path.read_bytes() == (reference / directory / path.name).read_bytes()

    def driver(path, changed, complete=None, skip=False):
        item = json.loads(path.read_text())
        assert item['driver_exit'] == (0 if changed else 3)
        assert item['available'] and item['changed'] == changed and item['candidateEdges'] == 1
        assert all(item[key] for key in ('moduleIdentity', 'globalIdentity', 'argumentIdentity',
                                        'ordinaryUnchanged', 'metadataUnchanged', 'verified'))
        if complete is not None:
            assert item['complete'] == complete
        if changed:
            assert item['peeledEdges'] == 1 and item['rejected'] == 0
        else:
            assert item['rejectedUnchanged']
            assert item['skippedSingleEntry'] == (1 if skip else 0)
            assert item['rejected'] == (0 if skip else 1)
        report['driver_checks'].append({'path': str(path), 'result': item})

    for mode in model.MODES:
        for budget in ('6-256', '32-2048'):
            driver(root / f'logs/{mode}-{budget}.stderr', True, True if budget == '32-2048' else None)
    driver(root / 'logs/canonical-loop.stderr', True, True)
    for name in ('historical', 'liveout'):
        driver(root / f'logs/reject_{name}.stderr', False, False)
        assert not (root / f'reject_{name}-output.ll').exists()
    driver(root / 'logs/repeat.stderr', False, True, True)
    assert not (root / 'repeat-output.ll').exists()
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
    for mode in terminal.MODES:
        for budget in ('6-256', '32-2048'):
            driver(term / f'logs/{mode}-{budget}.stderr', True, True)
    for control in ('shared', 'budget'):
        driver(term / f'logs/reject-{control}.stderr', False, False)
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
                   'scope': f'production {args.runtime_flavor}-v1 API; frozen synthetic models; no real-case native claim'})
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
