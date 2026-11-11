#!/usr/bin/env python3
"""Full-state byte oracle for optional STORE-preserving context copies.

The earlier producer/model files stay unchanged. This distinct fixture adds
observable ordered stores, descriptor writes and actual regs/Guest overlap.
"""
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
import sys

import loop_carried_context_diff as original

shared = original.shared
NAMES = ('guest', 'regs', 'overlap', 'descriptor', 'narrow16', 'wide128',
         'history', 'outer', 'call', 'volatile', 'atomic', 'fence', 'budget')
GAINS = NAMES[:8]
REJECTS = ('call', 'volatile', 'atomic', 'fence')
VALUES = (0, 1, 50, 51, 0x8000, 0x80000000, 1 << 63, (1 << 64) - 1)
SIZES = (0x487, 0x598)
INPUTS = tuple((name, route, size, rounds, x) for name in NAMES
               for route in ((0, 5, 13, 19, 20) if name == 'budget' else range(4))
               for size in SIZES
               for rounds in ((0, 2) if name in ('guest', 'overlap', 'history') else (0,))
               for x in VALUES)
CONTROLS = ('drop_store', 'reverse_stores', 'stale_read', 'duplicate_store',
            'unknown', 'bounds', 'default', 'history', 'stale_size')
MASK = (1 << 64) - 1
SNAPSHOT_BYTES = shared.SNAPSHOT_BYTES + 8
GROUPS = shared.GROUPS + (('descriptor_size', shared.SNAPSHOT_BYTES, SNAPSHOT_BYTES),)


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def initial(key):
    name, _, _, _, x = key
    regs, memory = map(bytearray, original.initial(x))
    if name == 'overlap':
        memory[0x20:0x220] = regs
        regs = memoryview(memory)[0x20:0x220]
    return regs, memory


def integer(data, offset, width=64):
    return int.from_bytes(data[offset:offset + width // 8], 'little')


def put(data, offset, value, width=64):
    data[offset:offset + width // 8] = (value & ((1 << width) - 1)).to_bytes(width // 8, 'little')


def model(key, fault=None):
    name, route, size, rounds, x = key
    regs, memory = initial(key)
    desc_size = size
    if name == 'outer' and route == 0 and x & 1:
        result = x ^ 0xabcd
        put(regs, 0, result)
        put(memory, 0x200, 0xe7)
        fields = tuple(0xf000 + n for n in range(7))
        return struct.pack('<QQ7Q', result, 0, *fields) + bytes(regs) + bytes(memory) + struct.pack('<Q', desc_size)
    if name == 'budget':
        state = route if route < 20 else x
    elif name != 'history' and route == 0:
        state = 50
    else:
        state = (x ^ 0x55 if route == 1 else x + 1 if route == 2 else x) & MASK
    if fault == 'unknown' and route in (1, 2, 3):
        state = 50
    iteration = calls = 0
    cs, cl, cf = (integer(regs, n) for n in (8, 24, 32))
    entering_b = True
    for _ in range(8):
        if entering_b:
            head_state = state
            put(memory, 0x180, integer(memory, 0x180) + (2 if fault == 'duplicate_store' else 1))
            width = 16 if name == 'narrow16' else 128 if name == 'wide128' else 64
            mask = (1 << width) - 1
            effect = regs if name in ('regs', 'overlap') else memory
            offset = 96 if name in ('regs', 'overlap') else 0x100
            old_effect = desc_size if name == 'descriptor' else integer(effect, offset, width)
            cached_size = desc_size
            payload = x ^ state
            if name == 'descriptor':
                payload = (payload & 0x1ff) | 0x400
            if width == 128:
                payload |= (x ^ 0xabcdef) << 64
            payload &= mask

            def write_effect(v):
                nonlocal desc_size
                if name == 'descriptor':
                    desc_size = v & MASK
                else:
                    put(effect, offset, v, width)

            if fault == 'reverse_stores':
                write_effect(old_effect + 1)
                write_effect(payload)
            else:
                write_effect(payload)
                fresh = old_effect if fault == 'stale_read' else payload
                if fault != 'drop_store':
                    write_effect(fresh + 1)
            observed = integer(memory, 0x100)
            if name == 'call':
                calls += 1
                regs[80] ^= x & 255
                memory[0x300] ^= (x >> 8) & 255
            flags = ((state * 17 + cs * 49) & MASK) ^ cl ^ ((cf << 5) & MASK) ^ observed
            if state > 50:
                status, value, index = 2, 17, 0
                break
        index = ((state << 3) + 0x400) & MASK
        bound_size = cached_size if fault == 'stale_size' else desc_size
        if not (bound_size > 7 and index <= bound_size - 8) and fault != 'bounds':
            status, value = 3, 23
            break
        if index + 8 > shared.MEM_BYTES:
            raise RuntimeError('fixture/control attempted an unallocated host access')
        target = integer(memory, index)
        effective = target if iteration < rounds else 0x1111
        if effective == 0x1111:
            status, value = 1, target
            break
        if effective == 0x2222:
            status, value = 5, target
            break
        if effective == 0x3333:
            changed = (observed + 0x101 + x + iteration) & MASK
            put(memory, 0x100, changed)
            cs = 16 if fault == 'history' else head_state if name == 'history' else state
            cl = cf = observed
            state, entering_b = 16, True
        elif effective == 0x4444:
            state ^= 1
            entering_b = False
        elif fault == 'default':
            status, value = 1, target
            break
        else:
            status, value = 4, target
            break
        iteration += 1
    else:
        raise RuntimeError('finite store fixture did not terminate')
    result = ((flags + state + cs + cl + cf + observed) & MASK) ^ status ^ value ^ index ^ iteration
    for n, v in enumerate((result, state, cs, cl, cf, observed, flags, iteration, index)):
        put(regs, n * 8, v)
    put(memory, 0x200, result)
    put(memory, 0x208, status)
    put(memory, 0x210, cf)
    mode = 1 if name == 'history' else 4 if name == 'call' else 8 if name == 'fence' else 5
    fields = (status, 0, 0xc000 + mode * 16, (result + 0x1000) & MASK, 1, x ^ 0xdeadbeef, iteration)
    return struct.pack('<QQ7Q', result, calls, *fields) + bytes(regs) + bytes(memory) + struct.pack('<Q', desc_size)


def differences(actual, expected):
    if len(actual) != SNAPSHOT_BYTES or len(expected) != SNAPSHOT_BYTES:
        raise RuntimeError('truncated or extended store full-state snapshot')
    return [name for name, begin, end in GROUPS if actual[begin:end] != expected[begin:end]]


def self_test():
    count = 0
    for key in INPUTS:
        wanted = model(key)
        if differences(wanted, wanted):
            raise RuntimeError('identity comparison failed')
        for field, begin, _ in GROUPS:
            bad = bytearray(wanted)
            bad[begin] ^= 1
            if differences(bad, wanted) != [field]:
                raise RuntimeError('single-field comparator control escaped')
            count += 1
    for size in (0, SNAPSHOT_BYTES - 1, SNAPSHOT_BYTES + 1):
        try:
            differences(bytes(size), model(INPUTS[0]))
        except RuntimeError:
            count += 1
        else:
            raise RuntimeError('bad snapshot length accepted')
    return count


def invoke(native, key, override=None):
    name, route, size, rounds, x = key
    regs, memory = initial(key)
    mem_array = (ctypes.c_uint64 * (shared.MEM_BYTES // 8)).from_buffer_copy(memory)
    base = ctypes.addressof(mem_array)
    if name == 'overlap':
        reg_array = (ctypes.c_ubyte * shared.REG_BYTES).from_address(base + 0x20)
    else:
        reg_array = (ctypes.c_ubyte * shared.REG_BYTES).from_buffer_copy(regs)
    descriptor = shared.Memory(base, size)
    exit_array = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + n for n in range(7)])
    function = getattr(native, override or 'store_' + name)
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(shared.Memory), ctypes.c_void_p,
                         ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(native, 'carried_calls')
    calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exit_array, x, route, rounds)
    if descriptor.data != base:
        raise RuntimeError('descriptor.data changed from its exact allocated base')
    return (struct.pack('<QQ7Q', result, calls.value, *exit_array) + bytes(reg_array)
            + bytes(mem_array) + struct.pack('<Q', descriptor.size))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--repo', type=Path)
    p.add_argument('--library', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--feature', choices=('default', 'stores'), default='stores')
    p.add_argument('--expect-unavailable', action='store_true')
    p.add_argument('--require-gains', action='store_true')
    p.add_argument('--cxx', default='c++')
    p.add_argument('--clang', default='clang-21')
    p.add_argument('--llvm-config', default='llvm-config-21')
    a = p.parse_args()
    if a.self_test:
        print(json.dumps(dict(ok=True, inputs=len(INPUTS), controls=self_test())))
        return 0
    if any(v is None for v in (a.repo, a.library, a.work)):
        p.error('--repo, --library and --work required')
    repo, library, work = (v.expanduser().resolve() for v in (a.repo, a.library, a.work))
    if work.exists():
        p.error('--work must be new')
    work.mkdir(parents=True)
    report = dict(inputs=len(INPUTS), library=str(library), feature=a.feature,
                  expect_unavailable=a.expect_unavailable, require_gains=a.require_gains,
                  commands=[], rows=[], errors=[], normal_comparisons=0, normal_mismatches=0,
                  control_comparisons=0, controls_required=0, controls_detected=0,
                  control_mismatches=0, excluded=0, skipped=0, normalized=0)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))

    def run(name, argv):
        try:
            r = subprocess.run(argv, capture_output=True, env=env, timeout=240)
        except subprocess.TimeoutExpired as e:
            (work / (name + '.stdout')).write_bytes(e.stdout or b'')
            (work / (name + '.stderr')).write_bytes(e.stderr or b'')
            report['commands'].append(dict(name=name, argv=argv, timeout=True))
            raise
        (work / (name + '.stdout')).write_bytes(r.stdout)
        (work / (name + '.stderr')).write_bytes(r.stderr)
        report['commands'].append(dict(name=name, argv=argv, returncode=r.returncode))
        if r.returncode:
            raise RuntimeError(name + ' failed: ' + str(r.returncode))
        return r.stdout

    try:
        pin = report['library_sha256'] = digest(library)
        report['repo_head'] = run('repo-head', ['git', '-C', str(repo), 'rev-parse', 'HEAD']).decode().strip()
        sources = work / 'sources'
        sources.mkdir()
        for name in ('store_context_native.cpp', 'store_context_diff.py',
                     'loop_carried_context_native.cpp', 'loop_carried_context_diff.py', 'state_guard_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
                                 str(sources / 'store_context_native.cpp'), str(library),
                                 '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats = {}
        for line in run('generate', [str(generator), str(work), a.feature]).decode().splitlines():
            row = dict(field.split('=') for field in line.split())
            name = row.pop('case')
            if name not in NAMES or name in stats:
                raise RuntimeError('unexpected or duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        if set(stats) != set(NAMES) or any(r['invalid'] or not r['unknown_retained'] for r in stats.values()):
            raise RuntimeError('fixture missing, invalid or unknown edge lost')
        if a.expect_unavailable:
            if any(r['available'] or r['changed'] for r in stats.values()):
                raise RuntimeError('expected an unavailable option without mutation')
            if any(digest(work / (n + '-before.ll')) != digest(work / (n + '-after.ll')) for n in NAMES):
                raise RuntimeError('unavailable option changed the module')
            report['ok'] = report['unavailable'] = True
        else:
            if not all(r['available'] for r in stats.values()):
                raise RuntimeError('STORE option unavailable')
            if a.feature == 'default' and any(r['changed'] or r['redirected'] for r in stats.values()):
                raise RuntimeError('default STORE barrier changed')
            if a.require_gains:
                for name in GAINS:
                    if not stats[name]['redirected'] or not stats[name]['index16' if name == 'history' else 'index50']:
                        raise RuntimeError('required actual STORE context/table LOAD missing: ' + name)
                if not stats['budget']['budget'] or not stats['budget']['redirected']:
                    raise RuntimeError('module-copy bound did not retain a partial gain')
            if any(stats[n]['changed'] or stats[n]['redirected'] for n in REJECTS):
                raise RuntimeError('effect barrier was bypassed')
            report['self_controls'] = self_test()
            (work / 'helper.c').write_text(original.HELPER)
            with gzip.open(work / 'expected.bin.gz', 'wb', compresslevel=1) as f:
                for key in INPUTS:
                    f.write(model(key))
            for level in ('O0', 'O2'):
                for stage in ('before', 'after'):
                    label = stage + '-' + level
                    out = work / (label + '.so')
                    run('compile-' + label, [a.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module']
                        + [str(work / (n + '-' + stage + '.ll')) for n in NAMES]
                        + [str(work / 'helper.c'), '-latomic', '-o', str(out)])
                    native = ctypes.CDLL(str(out))
                    with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as f:
                        for key in INPUTS:
                            actual = invoke(native, key)
                            f.write(actual)
                            bad = differences(actual, model(key))
                            report['normal_comparisons'] += 1
                            report['normal_mismatches'] += bool(bad)
                            report['rows'].append(dict(level=level, stage=stage, key=key, fields=bad,
                                                       snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                for control in CONTROLS:
                    label = 'control-' + control + '-' + level
                    out = work / (label + '.so')
                    run('compile-' + label, [a.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module',
                        str(work / ('control_' + control + '.ll')), str(work / 'helper.c'), '-latomic', '-o', str(out)])
                    native = ctypes.CDLL(str(out))
                    keys = [key for key in INPUTS if key[0] == ('descriptor' if control == 'stale_size' else 'history')]
                    wanted, found = set(), set()
                    with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as f:
                        for key in keys:
                            if differences(model(key, control), model(key)):
                                wanted.add(key)
                            actual = invoke(native, key, 'control_' + control)
                            f.write(actual)
                            bad = differences(actual, model(key))
                            report['control_comparisons'] += 1
                            report['control_mismatches'] += bool(bad)
                            if bad:
                                found.add(key)
                            report['rows'].append(dict(level=level, control=control, key=key, fields=bad,
                                                       snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                    report['controls_required'] += len(wanted)
                    report['controls_detected'] += len(wanted & found)
                    if found != wanted:
                        raise RuntimeError('fault control differed from predeclared set: ' + control)
            report['semantic_ok'] = report['normal_mismatches'] == 0
            planned_controls = 2 * sum(sum(key[0] == ('descriptor' if c == 'stale_size' else 'history')
                                          for key in INPUTS) for c in CONTROLS)
            report['coverage_ok'] = (report['normal_comparisons'] == len(INPUTS) * 4
                                     and report['control_comparisons'] == planned_controls
                                     and report['controls_required'] == report['controls_detected'])
            report['ok'] = report['semantic_ok'] and report['coverage_ok']
        if digest(library) != pin:
            raise RuntimeError('runtime changed')
    except Exception as e:
        report['errors'].append(type(e).__name__ + ': ' + str(e))
        report['ok'] = False
    report['artifact_sha256'] = {str(f.relative_to(work)): digest(f) for f in sorted(work.rglob('*')) if f.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({k: report.get(k) for k in ('inputs', 'normal_comparisons', 'normal_mismatches',
        'control_comparisons', 'controls_required', 'controls_detected', 'errors', 'ok')}))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
