#!/usr/bin/env python3
"""Independent, full-state native oracle for edge-local state-guard copies.

This validates an optional LLVM transform, not machine-lifter correctness.
The byte model has no dependency on production proof or unit-test builders.
All host accesses have allocated storage; logical memory failures are explicit
IR paths. No undef/poison value is assigned a chosen native meaning.
"""
import argparse
import ctypes
import functools
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

NAMES = ('basic', 'size_load', 'outer_guard', 'loop', 'narrow8', 'narrow32',
         'signed64', 'ordinary_load', 'unknown_call', 'store', 'atomic',
         'volatile', 'fence')
GAINS, REJECTS = NAMES[:8], NAMES[8:]
VALUES = (0, 1, 49, 50, 51, 0x7f, 0x80, 0xff, 0x80000000, 1 << 63, (1 << 64) - 1)
SIZES = (0x507, 0x508)
INPUTS = tuple((name, route, size, rounds, x) for name in NAMES for route in range(4)
               for size in SIZES for rounds in ((0, 2) if name == 'loop' else (0,)) for x in VALUES)
MASK = (1 << 64) - 1
REG_BYTES, MEM_BYTES, SNAPSHOT_BYTES = 512, 65536, 66120
EXIT_FIELDS = ('reason', 'transfer_kind', 'source_pc', 'next_pc',
               'next_pc_valid', 'return_pc', 'detail')
GROUPS = (('result', 0, 8), ('calls', 8, 16)) + tuple(
    (name, 16 + index * 8, 24 + index * 8) for index, name in enumerate(EXIT_FIELDS)) + (
    ('registers', 72, 584), ('memory', 584, SNAPSHOT_BYTES))
HELPER = r'''
#include <stdint.h>
uint64_t state_guard_calls;
void state_guard_unknown_writer(unsigned char *regs, unsigned char *data, uint64_t x) {
  ++state_guard_calls;
  regs[40] ^= (unsigned char)x;
  data[0x700] ^= (unsigned char)(x >> 8);
}
'''

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()

@functools.lru_cache(maxsize=len(VALUES))
def initial(x):
    return (bytes((i * 13 + (x & 255)) & 255 for i in range(REG_BYTES)),
            bytes((i * 37 + ((x >> 8) ^ x)) & 255 for i in range(MEM_BYTES)))

def put(target, offset, value):
    target[offset:offset + 8] = (value & MASK).to_bytes(8, 'little')

def get(target, offset): return int.from_bytes(target[offset:offset + 8], 'little')

def state_value(name, route, x):
    width = 8 if name == 'narrow8' else 32 if name == 'narrow32' else 64
    mask = (1 << width) - 1
    state = 50 if route == 0 else x if route == 1 else x ^ 0x55 if route == 2 else x + 1
    return state & mask

def rejected(name, state):
    signed = state - (1 << 64) if name == 'signed64' and state >> 63 else state
    return signed > 50

def model(key):
    name, route, size, rounds, x = key
    regs, memory = map(bytearray, initial(x))
    initial_reg, iteration, calls = get(regs, 32), 0, 0
    if name == 'outer_guard' and x & 1:
        state, flags, status, value = 0xabc, 0xdef, 4, 29
    else:
        state = state_value(name, route, x)
        while True:
            if name == 'unknown_call':
                calls += 1; regs[40] ^= x & 255; memory[0x700] ^= (x >> 8) & 255
            elif name == 'store': put(memory, 0x520, x)
            extra = get(memory, 0x500) if name in ('ordinary_load', 'atomic', 'volatile') else 0
            flags = state ^ 0xf00dfacecafebeef ^ size ^ extra
            if rejected(name, state): status, value = 2, 17; break
            if size < 0x508: status, value = 3, 23; break
            value = get(memory, 0x500)
            if name == 'loop' and iteration < rounds:
                state, iteration = x, iteration + 1
                continue
            status = 1; break
    result = (state ^ flags ^ status ^ value ^ iteration ^ initial_reg) & MASK
    put(regs, 0, result); put(regs, 8, state); put(regs, 16, flags)
    put(regs, 24, iteration); put(regs, 32, initial_reg ^ result)
    put(memory, 0x600, result); put(memory, 0x608, status)
    fields = (status, 0, 0x9000 + NAMES.index(name) * 16,
              (result + 0x1000) & MASK, 1, x ^ 0xdeadbeef, iteration)
    return struct.pack('<QQ7Q', result, calls, *fields) + regs + memory

def differences(actual, expected):
    if len(actual) != SNAPSHOT_BYTES or len(expected) != SNAPSHOT_BYTES:
        raise RuntimeError('truncated or extended full-state snapshot')
    return [name for name, begin, end in GROUPS if actual[begin:end] != expected[begin:end]]

def self_test():
    controls = 0
    for key in INPUTS:
        wanted = model(key)
        if differences(wanted, wanted): raise RuntimeError('identity comparison failed')
        for field, begin, end in GROUPS:
            corrupt = bytearray(wanted); corrupt[begin] ^= 1
            if differences(corrupt, wanted) != [field]: raise RuntimeError('single-field control escaped')
            controls += 1
    for bad in (b'', bytes(SNAPSHOT_BYTES - 1), bytes(SNAPSHOT_BYTES + 1)):
        try: differences(bad, model(INPUTS[0]))
        except RuntimeError: controls += 1
        else: raise RuntimeError('malformed snapshot accepted')
    return controls

class Memory(ctypes.Structure):
    _fields_ = [('data', ctypes.c_void_p), ('size', ctypes.c_uint64)]

def invoke(library, key, override=None):
    name, route, size, rounds, x = key
    regs, memory = initial(x)
    reg_array = (ctypes.c_ubyte * REG_BYTES).from_buffer_copy(regs)
    mem_array = (ctypes.c_uint64 * (MEM_BYTES // 8)).from_buffer_copy(memory)
    exit_array = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + i for i in range(7)])
    base = ctypes.addressof(mem_array)
    if base % 8: raise RuntimeError('atomic input buffer lacks native alignment')
    descriptor = Memory(base, size)
    function = getattr(library, override or 'guard_' + name)
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(Memory), ctypes.c_void_p,
                        ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'state_guard_calls'); calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exit_array, x, route, rounds)
    if descriptor.data != base or descriptor.size != size: raise RuntimeError('descriptor unexpectedly mutated')
    return struct.pack('<QQ7Q', result, calls.value, *exit_array) + bytes(reg_array) + bytes(mem_array)

def control_required(control, key):
    name, route, size, rounds, x = key
    state = state_value(name, route, x)
    if control == 'unknown': return rejected(name, state)
    if control == 'flags': return True
    return state <= 50 and size < 0x508

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true'); p.add_argument('--repo', type=Path)
    p.add_argument('--library', type=Path); p.add_argument('--work', type=Path)
    g = p.add_mutually_exclusive_group(); g.add_argument('--baseline', action='store_true'); g.add_argument('--require-gains', action='store_true')
    p.add_argument('--cxx', default='c++'); p.add_argument('--clang', default='clang-21')
    p.add_argument('--llvm-config', default='llvm-config-21'); a = p.parse_args()
    if a.self_test:
        print(json.dumps(dict(ok=True, inputs=len(INPUTS), controls=self_test()))); return 0
    if a.repo is None or a.library is None or a.work is None: p.error('--repo, --library and --work required')
    repo, library, work = (v.expanduser().resolve() for v in (a.repo, a.library, a.work))
    if work.exists(): p.error('--work must be new; evidence is never overwritten')
    work.mkdir(parents=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    report = dict(library=str(library), inputs=len(INPUTS), commands=[], rows=[], errors=[],
                  normal_comparisons=0, normal_mismatches=0, control_comparisons=0,
                  control_mismatches=0, controls_required=0, controls_detected=0,
                  baseline=a.baseline, require_gains=a.require_gains, excluded=0, skipped=0, normalized=0)
    def run(name, argv):
        try: r = subprocess.run(argv, capture_output=True, env=env, timeout=180)
        except subprocess.TimeoutExpired as e:
            (work / (name + '.stdout')).write_bytes(e.stdout or b''); (work / (name + '.stderr')).write_bytes(e.stderr or b'')
            report['commands'].append(dict(name=name, argv=argv, timeout=True)); raise RuntimeError(name + ' timed out') from e
        (work / (name + '.stdout')).write_bytes(r.stdout); (work / (name + '.stderr')).write_bytes(r.stderr)
        report['commands'].append(dict(name=name, argv=argv, returncode=r.returncode))
        if r.returncode: raise RuntimeError(name + ' failed: ' + str(r.returncode))
        return r.stdout
    try:
        pin = report['library_sha256'] = digest(library)
        report['repo_head'] = run('repo-head', ['git', '-C', str(repo), 'rev-parse', 'HEAD']).decode().strip()
        run('repo-status', ['git', '-C', str(repo), 'status', '--short'])
        sources = work / 'sources'; sources.mkdir()
        for name in ('state_guard_native.cpp', 'state_guard_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {p.name: digest(p) for p in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
            str(sources / 'state_guard_native.cpp'), str(library), '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats = {}
        for line in run('generate', [str(generator), str(work)]).decode().splitlines():
            row = dict(field.split('=') for field in line.split()); name = row.pop('case')
            if name not in NAMES or name in stats: raise RuntimeError('unexpected or duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        if set(stats) != set(NAMES) or any(r['invalid'] or not r['available'] or not r['unknown_retained'] for r in stats.values()):
            raise RuntimeError('fixture missing, invalid or lost unknown path')
        if a.baseline and any(r['gain'] or r['added_blocks'] for r in stats.values()): raise RuntimeError('baseline gained edge copies')
        if a.require_gains and any(not stats[n]['gain'] or stats[n]['added_blocks'] != 1 for n in GAINS):
            raise RuntimeError('required known incoming edge was not cloned and folded')
        if any(stats[n]['gain'] or stats[n]['added_blocks'] or stats[n]['changed'] for n in REJECTS):
            raise RuntimeError('a forbidden-effect block was copied')
        report['self_controls'] = self_test()
        (work / 'helper.c').write_text(HELPER)
        with gzip.open(work / 'expected.bin.gz', 'wb', compresslevel=1) as stream:
            for key in INPUTS: stream.write(model(key))
        for level in ('O0', 'O2'):
            for stage in ('before', 'after'):
                label = stage + '-' + level; output = work / (label + '.so')
                run('compile-' + label, [a.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module']
                    + [str(work / (n + '-' + stage + '.ll')) for n in NAMES]
                    + [str(work / 'helper.c'), '-latomic', '-o', str(output)])
                native = ctypes.CDLL(str(output))
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as stream:
                    for key in INPUTS:
                        actual = invoke(native, key); stream.write(actual); mismatch = differences(actual, model(key))
                        report['normal_comparisons'] += 1; report['normal_mismatches'] += bool(mismatch)
                        report['rows'].append(dict(level=level, stage=stage, key=key, fields=mismatch,
                                                  snapshot_sha256=hashlib.sha256(actual).hexdigest()))
            for control in ('unknown', 'flags', 'failure'):
                label = 'control-' + control + '-' + level; output = work / (label + '.so')
                run('compile-' + label, [a.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module',
                    str(work / ('control_' + control + '.ll')), str(work / 'helper.c'), '-latomic', '-o', str(output)])
                native = ctypes.CDLL(str(output)); wanted, found = set(), set()
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as stream:
                    for key in (k for k in INPUTS if k[0] == 'basic'):
                        if control_required(control, key): wanted.add(key)
                        actual = invoke(native, key, 'control_' + control); stream.write(actual)
                        mismatch = differences(actual, model(key)); report['control_comparisons'] += 1
                        report['control_mismatches'] += bool(mismatch)
                        if mismatch: found.add(key)
                        report['rows'].append(dict(level=level, control=control, key=key, fields=mismatch,
                                                  snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                report['controls_required'] += len(wanted); report['controls_detected'] += len(wanted & found)
                if found != wanted: raise RuntimeError('faulty IR did not fail exactly the predeclared input set: ' + control)
        if digest(library) != pin: raise RuntimeError('library changed during validation')
        report['semantic_ok'] = report['normal_mismatches'] == 0
        report['coverage_ok'] = (report['normal_comparisons'] == len(INPUTS) * 4
            and report['control_comparisons'] == len([k for k in INPUTS if k[0] == 'basic']) * 6
            and report['controls_detected'] == report['controls_required'])
        report['ok'] = report['semantic_ok'] and report['coverage_ok']
    except Exception as e:
        report['errors'].append(type(e).__name__ + ': ' + str(e)); report['ok'] = False
    report['artifact_sha256'] = {str(p.relative_to(work)): digest(p) for p in sorted(work.rglob('*')) if p.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({k: report.get(k) for k in ('inputs', 'normal_comparisons', 'normal_mismatches',
        'control_comparisons', 'control_mismatches', 'controls_required', 'controls_detected',
        'semantic_ok', 'baseline', 'require_gains', 'errors', 'ok')})); return 0 if report['ok'] else 1

if __name__ == '__main__': sys.exit(main())
