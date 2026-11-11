#!/usr/bin/env python3
"""Independent byte model for historical inputs to checked Guest-memory corridors."""
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
from functools import lru_cache
import guest_memory_checked_edges_diff as original

KINDS = ('phi', 'load', 'freeze')
DEPTHS = ('one', 'deep', 'branch')
NAMES = tuple(kind + '_' + depth for kind in KINDS for depth in DEPTHS)
ORIGINAL = tuple(n for n in NAMES if not n.endswith('_branch'))
ADDITIONAL = tuple(n for n in NAMES if n.endswith('_branch'))
VALUES = (0, 1, 0x80000000, 0x8000000000000000, 0x7fffffffffffffff, 0xffffffffffffffff)
INPUTS = tuple((name, path, bound, rounds, x) for name in NAMES for path in range(4 if name.endswith('_branch') else 3)
               for bound in range(3) for rounds in range(3) for x in VALUES)
CONTROLS = ('fresh_history', 'stale_fresh', 'drop_store', 'bounds', 'reentry', 'liveout') + tuple(
    kind + '_branch_' + fault for kind in KINDS for fault in ('fresh_history', 'drop_store'))
SNAPSHOT_BYTES = original.SNAPSHOT_BYTES
MASK = original.MASK
GROUPS = original.GROUPS
digest = original.digest
differences = original.differences

@lru_cache(None)
def initial_bytes(x):
    regs = bytes((i * 13 + (x & 255)) & 255 for i in range(512))
    pattern = bytes((i * 37 + ((x >> 8) ^ x)) & 255 for i in range(256))
    memory = bytearray(pattern * 256)
    original.put(memory, 0x400, x ^ 0x1122334455667788)
    return regs, bytes(memory)

def parameters(key):
    _, path, bound, _, x = key
    sp = 256 + (x & 1) * 256
    index = sp + (32 if path == 1 else 64 if path == 3 else 0) - 168
    size = (65536, index + 7, index + 8)[bound]
    return sp, size

def model(key, fault=None):
    name, path, _, rounds, x = key
    kind, depth = name.split('_')
    if fault and '_branch_' in fault:
        fault = fault.split('_branch_', 1)[1]
    regs, memory = map(bytearray, initial_bytes(x))
    sp, size = parameters(key)
    frame = sp + (32 if path == 1 else 64 if path == 3 else 0)
    history, count = x ^ (0x33 if path == 3 else 0), 0
    while True:
        index = (frame - 168) & MASK
        fresh = (frame ^ count) if kind == 'phi' else int.from_bytes(memory[0x400:0x408], 'little')
        if kind == 'freeze':
            fresh ^= count
        valid = size >= 8 and index <= size - 8
        if not valid and fault != 'bounds':
            success = False
            result = index ^ 0xbad0000000000000
            break
        if not 0 <= index <= 65528:
            raise RuntimeError('fixture attempted an unallocated host read')
        state = int.from_bytes(memory[index:index + 8], 'little')
        observed_history = fresh if fault == 'fresh_history' else history
        observed_fresh = history if fault == 'stale_fresh' else fresh
        result = state ^ ((observed_history + observed_fresh) & MASK)
        if count >= rounds:
            success = True
            break
        original.put(memory, 0x400, x ^ (count + 0x55))
        if fault != 'drop_store':
            original.put(memory, index, 14)
        if path == 2 and count == 0 and fault != 'reentry':
            frame += 8
        history = fresh
        count += 1
    live = x if fault == 'liveout' else frame ^ fresh ^ history
    for offset, value in ((0, result), (8, count), (16, live), (24, fresh), (32, history), (40, index)):
        original.put(regs, offset, value)
    original.put(memory, 0x500, result)
    original.put(memory, 0x508, live)
    fields = (1 if success else 5, 0, 0x6000 + KINDS.index(kind) * 0x100 + DEPTHS.index(depth) * 0x40,
              index, int(success), x, 0 if success else 3)
    return struct.pack('<QQ7Q', result, 0, *fields) + regs + memory

def invoke(library, key, override=None):
    name, path, _, rounds, x = key
    regs, memory = initial_bytes(x)
    ra = (ctypes.c_ubyte * 512).from_buffer_copy(regs)
    ma = (ctypes.c_uint64 * (65536 // 8)).from_buffer_copy(memory)
    ex = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + n for n in range(7)])
    address = ctypes.addressof(ma)
    sp, size = parameters(key)
    descriptor = original.Memory(address, size)
    fn = getattr(library, override or 'history_' + name)
    fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(original.Memory), ctypes.c_void_p,
                   ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
    fn.restype = ctypes.c_uint64
    result = fn(ra, ctypes.byref(descriptor), ex, path, sp, x, rounds)
    if descriptor.data != address or descriptor.size != size:
        raise RuntimeError('descriptor mutated')
    return struct.pack('<QQ7Q', result, 0, *ex) + bytes(ra) + bytes(ma)

def self_test():
    count = 0
    for key in INPUTS:
        expected = model(key)
        if differences(expected, expected):
            raise RuntimeError('identity comparator failed')
        for field, begin, _ in GROUPS:
            bad = bytearray(expected)
            bad[begin] ^= 1
            if differences(bad, expected) != [field]:
                raise RuntimeError('field comparator control escaped')
            count += 1
    for size in (0, SNAPSHOT_BYTES - 1, SNAPSHOT_BYTES + 1):
        try:
            differences(bytes(size), model(INPUTS[0]))
        except RuntimeError:
            count += 1
        else:
            raise RuntimeError('wrong length accepted')
    return count

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--repo', type=Path)
    p.add_argument('--library', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--baseline', action='store_true')
    p.add_argument('--require-gains', action='store_true')
    p.add_argument('--require-additional-gains', action='store_true')
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
                  require_gains=a.require_gains, require_additional_gains=a.require_additional_gains,
                  commands=[], rows=[], errors=[],
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
        for name in ('guest_memory_history_native.cpp', 'guest_memory_history_diff.py',
                     'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
                                 str(sources / 'guest_memory_history_native.cpp'), str(library),
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
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['available'] or not r['unknown_retained'] for r in stats.values()):
            raise RuntimeError('missing or invalid fixture')
        gained = lambda n: bool(stats[n]['redirected'] and stats[n]['forwarded'] and stats[n]['target_loads'] == 1)
        # Keep the original six-case gate intact. The old single-predecessor
        # deep shapes remain in that gate, even though their admission is not
        # supported by this fix. Report new legal multi-hub cases separately.
        report['precision_ok'] = all(gained(n) for n in ORIGINAL)
        report['single_precision_ok'] = all(gained(n) for n in ORIGINAL if n.endswith('_one'))
        report['additional_precision_ok'] = all(gained(n) for n in ADDITIONAL)
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
                target = control.split('_branch_', 1)[0] + '_branch' if '_branch_' in control else 'freeze_one'
                keys = [key for key in INPUTS if key[0] == target]
                wanted, found = set(), set()
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as f:
                    for key in keys:
                        if differences(model(key, control), model(key)):
                            wanted.add(key)
                        actual = invoke(native, key, 'control_' + control)
                        if actual!=model(key,control):raise RuntimeError('fault full snapshot differs from independent faulty model')
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
        planned_controls = 2 * sum(sum(key[0] == (c.split('_branch_', 1)[0] + '_branch' if '_branch_' in c else 'freeze_one') for key in INPUTS) for c in CONTROLS)
        report['coverage_ok'] = (report['normal_comparisons'] == len(INPUTS) * 4
                                 and report['control_comparisons'] == planned_controls
                                 and report['controls_required'] == report['controls_detected'])
        report['ok'] = (report['semantic_ok'] and report['coverage_ok']
                        and (not a.require_gains or report['precision_ok'])
                        and (not a.require_additional_gains or report['additional_precision_ok']))
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
