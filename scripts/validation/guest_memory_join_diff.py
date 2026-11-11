#!/usr/bin/env python3
"""Independent byte model for allocation-independent Guest join forwarding."""
import argparse
import ctypes
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from functools import lru_cache
import guest_memory_checked_edges_diff as common

MASK = (1 << 64) - 1
VALUES = (0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
          0x7fffffffffffffff, 0x8000000000000000, MASK)
INDICES = (0, 1, 7, 31, 256, 257, 760, 767, 768, 769, 65528, 65529,
           65535, 1 << 63, MASK)
INPUTS = tuple((index, condition, seed) for index in INDICES
               for condition in (0, 1, 2, MASK) for seed in VALUES)

@lru_cache(None)
def initial(seed):
    regs = bytes((i * 13 + (seed & 255)) & 255 for i in range(512))
    pattern = bytes((i * 37 + (seed ^ (seed >> 8))) & 255 for i in range(256))
    return regs, pattern * 256

def model(key, fault=None):
    index, condition, seed = key
    regs, memory = map(bytearray, initial(seed))
    if index > 65528:
        result = 0
        fields = (5, 0, 0x6000, 0x6000, 1, index, 3)
    else:
        desired = 10 if condition & 1 else 20
        if fault != 'drop-store':
            common.put(memory, index, desired)
        seen = int.from_bytes(memory[index:index + 8], 'little')
        if fault == 'wrong-value':
            seen = 20 if condition & 1 else 10
        result = seen ^ seed
        for offset, value in ((0, result), (8, seen), (16, condition), (24, index)):
            common.put(regs, offset, value)
        common.put(memory, 0x300, result)
        fields = (1, 0, 0x6000, seen, 1, index, condition)
    return struct.pack('<QQ7Q', result, 0, *fields) + regs + memory

def invoke(library, key):
    index, condition, seed = key
    regs, memory = initial(seed)
    ra = (ctypes.c_ubyte * 512).from_buffer_copy(regs)
    ma = (ctypes.c_uint64 * 8192).from_buffer_copy(memory)
    ex = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + i for i in range(7)])
    address = ctypes.addressof(ma)
    desc = common.Memory(address, 65536)
    fn = library.join_memory
    fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(common.Memory), ctypes.c_void_p,
                   ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
    fn.restype = ctypes.c_uint64
    result = fn(ra, ctypes.byref(desc), ex, index, condition, seed)
    if desc.data != address or desc.size != 65536:
        raise RuntimeError('Guest descriptor changed')
    return struct.pack('<QQ7Q', result, 0, *ex) + bytes(ra) + bytes(ma)

def self_test():
    count = 0
    for key in INPUTS:
        expected = model(key)
        if common.differences(expected, expected):
            raise RuntimeError('identity comparator failure')
        for field, begin, _ in common.GROUPS:
            damaged = bytearray(expected)
            damaged[begin] ^= 1
            if common.differences(damaged, expected) != [field]:
                raise RuntimeError('comparator missed ' + field)
            count += 1
    for size in (0, common.SNAPSHOT_BYTES - 1, common.SNAPSHOT_BYTES + 1):
        try:
            common.differences(bytes(size), model(INPUTS[0]))
        except RuntimeError:
            count += 1
        else:
            raise RuntimeError('comparator accepted bad snapshot size')
    return count

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--require-stable', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(dict(inputs=len(INPUTS), comparator_controls=self_test(), ok=True)))
        return 0
    if any(v is None for v in (args.repo, args.library, args.work)):
        parser.error('--repo --library --work required')
    repo, library, work = (p.expanduser().resolve() for p in (args.repo, args.library, args.work))
    if work.exists():
        parser.error('--work must be new')
    work.mkdir(parents=True)
    report = dict(inputs=len(INPUTS), commands=[], rows=[], errors=[], normal_comparisons=0,
                  normal_mismatches=0, control_comparisons=0, controls_required=0,
                  controls_detected=0, excluded=0, skipped=0, normalized=0)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    def run(name, command):
        result = subprocess.run(command, capture_output=True, env=env, timeout=180)
        (work / (name + '.stdout')).write_bytes(result.stdout)
        (work / (name + '.stderr')).write_bytes(result.stderr)
        report['commands'].append(dict(name=name, argv=command, returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(name + ' failed')
        return result.stdout
    try:
        pin = common.digest(library)
        report['library_sha256'] = pin
        sources = work / 'sources'
        sources.mkdir()
        for name in ('guest_memory_join_native.cpp', 'guest_memory_join_diff.py',
                     'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {p.name: common.digest(p) for p in sources.iterdir()}
        include = run('llvm-include', ['llvm-config-21', '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', ['c++', '-std=c++20', '-O0', '-I' + str(repo / 'include'),
                                 '-I' + include, str(sources / 'guest_memory_join_native.cpp'),
                                 str(library), '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats = {}
        for line in run('generate', [str(generator), str(work)]).decode().splitlines():
            row = dict(item.split('=') for item in line.split())
            name = row.pop('case')
            if name in stats:
                raise RuntimeError('duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        if set(stats) != {'join_0', 'join_1'} or any(
                row != dict(available=1, complete=1, forwarded=1, phis=1, joins=0)
                for row in stats.values()):
            raise RuntimeError('fixture did not exercise exactly one forwarded PHI')
        before_equal = (work / 'join_0-before.ll').read_bytes() == (work / 'join_1-before.ll').read_bytes()
        after_equal = (work / 'join_0-after.ll').read_bytes() == (work / 'join_1-after.ll').read_bytes()
        report['same_before_ir'] = before_equal
        report['same_after_ir'] = after_equal
        if not before_equal:
            raise RuntimeError('allocation controls changed input IR')
        report['comparator_controls'] = self_test()
        with gzip.open(work / 'expected.bin.gz', 'wb', compresslevel=1) as stream:
            for key in INPUTS:
                stream.write(model(key))
        for level in ('O0', 'O2'):
            cases = [(f'join_{layout}-{stage}', None) for layout in (0, 1)
                     for stage in ('before', 'after')]
            cases += [(fault, fault) for fault in ('drop-store', 'wrong-value')]
            for name, fault in cases:
                label = name + '-' + level
                output = work / (label + '.so')
                run('compile-' + label, ['clang-21', '-' + level, '-shared', '-fPIC',
                                        '-Wno-override-module', str(work / (name + '.ll')),
                                        '-o', str(output)])
                native = ctypes.CDLL(str(output))
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as stream:
                    for key in INPUTS:
                        actual = invoke(native, key)
                        stream.write(actual)
                        expected = model(key)
                        bad = common.differences(actual, expected)
                        if fault:
                            faulty_expected = model(key, fault)
                            if actual != faulty_expected:
                                raise RuntimeError('fault IR disagrees with independent faulty model')
                            required = faulty_expected != expected
                            report['control_comparisons'] += 1
                            report['controls_required'] += required
                            report['controls_detected'] += bool(bad) and required
                            if bool(bad) != required:
                                raise RuntimeError('fault detection set differs from specified set')
                        else:
                            report['normal_comparisons'] += 1
                            report['normal_mismatches'] += bool(bad)
                        report['rows'].append(dict(case=label, key=key, fields=bad,
                                                   snapshot_sha256=hashlib.sha256(actual).hexdigest()))
        if common.digest(library) != pin:
            raise RuntimeError('runtime changed')
        report['ok'] = (report['normal_comparisons'] == len(INPUTS) * 8
                        and report['normal_mismatches'] == 0
                        and report['control_comparisons'] == len(INPUTS) * 4
                        and report['controls_detected'] == report['controls_required']
                        and (after_equal or not args.require_stable))
        if args.require_stable and not after_equal:
            report['errors'].append('output IR depends on host allocation order')
    except Exception as error:
        report['errors'].append(type(error).__name__ + ': ' + str(error))
        report['ok'] = False
    report['artifact_sha256'] = {str(p.relative_to(work)): common.digest(p)
                                for p in sorted(work.rglob('*')) if p.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({k: report.get(k) for k in ('inputs', 'normal_comparisons', 'normal_mismatches',
                     'control_comparisons', 'controls_required', 'controls_detected', 'same_before_ir',
                     'same_after_ir', 'errors', 'ok')}))
    return 0 if report['ok'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
