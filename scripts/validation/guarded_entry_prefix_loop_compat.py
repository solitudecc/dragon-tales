#!/usr/bin/env python3
"""Exact loop-only subset of the unchanged canonical guarded-entry oracle."""
import argparse
import ctypes
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def same(actual, expected, label):
    if actual != expected:
        raise AssertionError((label, 'full-state mismatch'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--guarded', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    digest = hashlib.sha256(args.model.read_bytes()).hexdigest()
    assert digest == '0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c'
    spec = importlib.util.spec_from_file_location('unchanged_guarded_oracle', args.model)
    model = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    keys = tuple(key for key in model.INPUTS if key[0] == 'loop')
    assert len(model.INPUTS) == 1120 and len(keys) == 112
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'helper.c').write_text(model.HELPER)
    arena = (ctypes.c_ubyte * model.ARENA_BYTES)()
    address = ctypes.addressof(arena)
    report = {'scope': 'loop112 subset; other nine modes rejected', 'model_sha256': digest,
              'inputs': keys, 'arena_address': address, 'normal': 0,
              'comparator_controls': 0, 'stages': [], 'failures': []}
    try:
        for opt in (0, 2):
            for stage in ('original', 'guarded', 'prefix'):
                path = getattr(args, stage)
                label = f'{stage}-O{opt}'
                so = args.output / (label + '.so')
                with (args.output / (label + '.compile.log')).open('wb') as log:
                    subprocess.run(['clang-21', '-shared', '-fPIC', f'-O{opt}',
                                    '-Wno-override-module', str(path),
                                    str(args.output / 'helper.c'), '-o', str(so)],
                                   stdout=log, stderr=log, check=True)
                library = ctypes.CDLL(str(so.resolve()))
                symbol = 'dragon_static_entry_x86_64_1400' + ('' if stage == 'original' else '.guarded.oracle')
                count = 0
                with gzip.open(args.output / (label + '.snapshots.gz'), 'wb') as stream:
                    for key in keys:
                        actual = model.invoke(library, key, arena, symbol)
                        expected = model.model(key, address)
                        stream.write(actual)
                        same(actual, expected, (label, key))
                        count += 1
                        report['normal'] += 1
                        for offset in (0, 8 + 0x100, 8 + model.GUEST + 256, 8 + 0x300 + 48):
                            damaged = bytearray(actual)
                            damaged[offset] ^= 1
                            try:
                                same(damaged, expected, 'comparator control')
                            except AssertionError:
                                report['comparator_controls'] += 1
                            else:
                                raise AssertionError('comparator control escaped')
                report['stages'].append({'label': label, 'matches': count,
                                        'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    except Exception as error:
        report['failures'].append(repr(error))
        raise
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
