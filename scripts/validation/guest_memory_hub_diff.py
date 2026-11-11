#!/usr/bin/env python3
"""Independent full-state byte oracle for checked loads behind multiple PHI hubs."""
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

import guest_memory_checked_edges_diff as original

NAMES = ('plain', 'deep', 'two_guards', 'dynamic', 'liveout', 'loop', 'alias',
         'partial', 'unknown_call', 'fence', 'volatile', 'atomic', 'overwrite', 'history')
GAINS = ('plain', 'deep', 'two_guards', 'dynamic', 'liveout', 'loop')
REJECTS = ('alias', 'partial', 'unknown_call', 'fence', 'volatile', 'atomic', 'history')
VALUES = (0, 1, 50, 51, 0x8000, 0x80000000, 1 << 63, (1 << 64) - 1)
INPUTS = tuple((name, path, bound, x) for name in NAMES for path in range(4)
               for bound in range(4 if name in ('loop', 'history') else 3) for x in VALUES)
CONTROLS = ('path', 'drop_store', 'unknown', 'bounds', 'reentry', 'history', 'liveout')
SNAPSHOT_BYTES = original.SNAPSHOT_BYTES
MASK = original.MASK
GROUPS = original.GROUPS
digest = original.digest
differences = original.differences


def mode(name):
    return ('two_guards' if name in ('deep', 'two_guards') else 'loop' if name == 'history'
            else 'volatile' if name == 'atomic' else name)


def parameters(key):
    name, path, bound, x = key
    sp = 256 + (x & 1) * 256
    actual = 1 if name == 'history' and path == 0 else path
    offset = sp - 128 if actual == 0 else sp - 96 + (actual - 1) * 32
    size = (original.MEM_BYTES, offset + 7, offset - 1, sp - 96 + 7)[bound]
    return sp, offset, size, x & 0xffffffff, sp - 128 if x & 1 else 0x600


def model(key, fault=None):
    name, path, _, x = key
    base = mode(name)
    mode_id = original.NAMES.index(base)
    sp, offset, size, rounds, alias = parameters(key)
    regs, memory = original.initial(key)
    initial16 = original.struct.unpack_from('<Q', regs, 16)[0]
    original.put(regs, 0, sp)
    original.put(regs, 8, x ^ (0x11110000 + mode_id))
    calls = 0
    extra = name in ('plain', 'two_guards', 'dynamic', 'liveout', 'loop')
    if path == 0 and name != 'history':
        if fault != 'drop_store':
            original.put(memory, sp - 128, x ^ 0x1357 if name == 'dynamic' else 50)
        if name == 'alias':
            original.put(memory, alias, x ^ 0x4567)
        elif name == 'partial':
            memory[sp - 127] = 0xab
        elif name == 'unknown_call':
            calls = 1
            regs[48] ^= x & 255
            memory[0x300] ^= (x >> 8) & 255
            original.put(memory, sp - 128, x ^ 0xfaceb00c12345678)
    if extra and path in (1, 2):
        original.put(memory, offset, 23 if path == 1 else 35)
    if fault == 'path' and path == 2:
        offset = sp - 128
    historical = 42 if path in (0, 1) else 43 if path == 2 else 44
    if name == 'liveout' and path == 0 and x & 1:
        tag, result, count = 3, x ^ 0xe411000000000000, 0
        memory[0x510] = 0xe7
    else:
        iteration = 0
        limit = (rounds & 3) + 1
        while True:
            if name == 'overwrite':
                original.put(memory, offset, x ^ 0x987654321)
            first = offset <= size
            second = first and size - offset >= 8
            if fault != 'bounds' and (not first or not second):
                tag = 1 if base in ('plain', 'dynamic', 'liveout') or not first else 2
                memory[0x510] = (path + tag) & 255
                result = offset ^ (0xbad0000000000000 + tag)
                count = iteration
                break
            if not 0 <= offset <= original.MEM_BYTES - 8:
                raise RuntimeError('hub fixture/control attempted an unallocated host access')
            loaded = 50 if fault == 'unknown' else int.from_bytes(memory[offset:offset + 8], 'little')
            original.put(memory, 0x500, loaded + x)
            result, count, tag = loaded ^ x, iteration + 1, 0
            if name not in ('loop', 'history') or count >= limit:
                break
            iteration += 1
            if name == 'history':
                historical = loaded
                original.put(memory, sp - 128, 50)
                offset = sp - 128
            else:
                offset = sp - 128 if fault == 'reentry' else sp - 96
    original.put(regs, 0, result)
    original.put(regs, 8, offset)
    original.put(regs, 16, initial16 if fault == 'liveout' else initial16 ^ result)
    original.put(regs, 24, count)
    original.put(regs, 32, path)
    successful, early = tag == 0, tag == 3
    fields = (1 if successful else 2 if early else 5, int(early),
              0x4000 + mode_id * 0x100 + tag * 8,
              (result + 0x5000) & MASK if successful else offset,
              int(successful or early), x ^ 0x9000,
              0 if successful else 5 if early else 3)
    returned = result ^ (50 if fault == 'history' else historical) if name == 'history' else result
    return struct.pack('<QQ7Q', returned & MASK, calls, *fields) + regs + memory


def self_test():
    count = 0
    for key in INPUTS:
        expected = model(key)
        if differences(expected, expected):
            raise RuntimeError('identity comparison failed')
        for field, begin, _ in GROUPS:
            bad = bytearray(expected)
            bad[begin] ^= 1
            if differences(bad, expected) != [field]:
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


def invoke(library, key, override=None):
    name, path, _, x = key
    regs, memory = original.initial(key)
    reg_array = (ctypes.c_ubyte * original.REG_BYTES).from_buffer_copy(regs)
    mem_array = (ctypes.c_uint64 * (original.MEM_BYTES // 8)).from_buffer_copy(memory)
    exits = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + n for n in range(7)])
    address = ctypes.addressof(mem_array)
    sp, _, size, rounds, alias = parameters(key)
    descriptor = original.Memory(address, size)
    function = getattr(library, override or 'hub_' + name)
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(original.Memory), ctypes.c_void_p,
                         ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64,
                         ctypes.c_uint32, ctypes.c_void_p]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'edge_calls')
    calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exits, path, sp, x, rounds, address + alias)
    if descriptor.data != address or descriptor.size != size:
        raise RuntimeError('descriptor mutated')
    return struct.pack('<QQ7Q', result, calls.value, *exits) + bytes(reg_array) + bytes(mem_array)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--repo', type=Path)
    p.add_argument('--library', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--baseline', action='store_true')
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
    report = dict(inputs=len(INPUTS), library=str(library), baseline=a.baseline,
                  require_gains=a.require_gains, commands=[], rows=[], errors=[],
                  normal_comparisons=0, normal_mismatches=0, control_comparisons=0,
                  controls_required=0, controls_detected=0, control_mismatches=0,
                  excluded=0, skipped=0, normalized=0)
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
        for name in ('guest_memory_hub_native.cpp', 'guest_memory_hub_diff.py',
                     'guest_memory_checked_edges_native.cpp', 'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
                                 str(sources / 'guest_memory_hub_native.cpp'), str(library),
                                 '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats = {}
        for line in run('generate', [str(generator), str(work)]).decode().splitlines():
            row = dict(field.split('=') for field in line.split())
            name = row.pop('case')
            if name not in NAMES or name in stats:
                raise RuntimeError('unexpected or duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        if set(stats) != set(NAMES) or any(r['invalid'] or not r['available'] or not r['unknown_retained'] for r in stats.values()):
            raise RuntimeError('fixture missing, invalid or unknown edge lost')
        if a.baseline and any(stats[n]['redirected'] for n in GAINS):
            raise RuntimeError('baseline unexpectedly traversed the new PHI hubs')
        if a.require_gains and any(not stats[n]['redirected'] or not stats[n]['forwarded'] for n in GAINS):
            raise RuntimeError('new hub forwarding did not reach a real LOAD')
        if any(stats[n]['redirected'] for n in REJECTS):
            raise RuntimeError('an unproved/effect/history path was copied')
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
                target = 'loop' if control == 'reentry' else 'history' if control == 'history' else 'plain'
                keys = [key for key in INPUTS if key[0] == target]
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
        if digest(library) != pin:
            raise RuntimeError('runtime changed')
        report['semantic_ok'] = report['normal_mismatches'] == 0
        planned_controls = 2 * sum(sum(key[0] == ('loop' if c == 'reentry' else 'history' if c == 'history' else 'plain')
                                      for key in INPUTS) for c in CONTROLS)
        report['coverage_ok'] = (report['normal_comparisons'] == len(INPUTS) * 4
                                 and report['control_comparisons'] == planned_controls
                                 and report['controls_required'] == report['controls_detected'])
        report['ok'] = report['semantic_ok'] and report['coverage_ok']
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
