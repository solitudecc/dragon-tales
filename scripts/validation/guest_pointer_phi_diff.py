#!/usr/bin/env python3
"""Full-state independent native oracle for paired STORE pointer/frame PHIs."""
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
import guest_memory_history_diff as history_source

ORIGINAL_NAMES = ('different', 'same', 'alias_before', 'alias_after', 'call_before', 'call_after',
         'base_reload', 'different_base', 'uncorrelated', 'history', 'history_mixed', 'history_noanchor')
NAMES = ORIGINAL_NAMES + tuple(n + '_suffix' for n in ORIGINAL_NAMES[:9])
GAINS = ('different', 'same', 'alias_before', 'call_before')
REJECTS = ('alias_after', 'call_after', 'base_reload', 'different_base', 'uncorrelated', 'history_mixed', 'history_noanchor')
SUFFIX_GAINS = tuple(n + '_suffix' for n in GAINS)
SUFFIX_REJECTS = tuple(n + '_suffix' for n in REJECTS if not n.startswith('history'))
VALUES = (0, 1, 2, 3, 0x80000000, 1 << 63, 0x7fffffffffffffff, 0xffffffffffffffff)
INPUTS = tuple((name, path, bound, x) for name in NAMES for path in range(3)
               for bound in range(3) for x in VALUES)
CONTROLS = ('wrong_phi', 'drop_store', 'drop_alias', 'drop_call', 'stale_pointer', 'bounds',
            'suffix_wrong_phi', 'suffix_drop_store', 'suffix_drop_alias', 'suffix_drop_call', 'suffix_bounds')
CONTROL_TARGETS = dict(wrong_phi='different', drop_store='different', drop_alias='alias_after',
                       drop_call='call_after', stale_pointer='history', bounds='different')
CONTROL_TARGETS.update({c: CONTROL_TARGETS[c.removeprefix('suffix_')] + '_suffix'
                        for c in CONTROLS if c.startswith('suffix_')})
SNAPSHOT_BYTES, MASK, GROUPS = original.SNAPSHOT_BYTES, original.MASK, original.GROUPS
digest, differences = original.digest, original.differences
initial_bytes = history_source.initial_bytes

def parameters(key):
    name, path, bound, x = key
    name = name.removesuffix('_suffix')
    sp = 256 + (x & 1) * 256
    if name.startswith('history'):
        index = sp + (32 if path == 1 else 0) - 168
    else:
        frame = sp + (64 if path == 2 else 0 if name == 'same' else
                      32 if (path == 0 if name == 'uncorrelated' else path == 1) else 0)
        index = frame - 128
    size = (65536, index + 7, index + 8)[bound]
    if name == 'different_base':
        alias = 64 if x & 1 else 0
    else:
        source_index = sp - 128 + (32 if path == 1 else 0)
        alias = source_index if x & 1 else 0x300
    return sp, index, size, alias

def historical_model(key, fault):
    name, path, _, x = key
    regs, memory = map(bytearray, initial_bytes(x))
    sp, _, size, _ = parameters(key)
    frame = sp + (32 if path == 1 else 0)
    history, count, rounds = x, 0, x & 3
    while True:
        index = (frame - 168) & MASK
        fresh = int.from_bytes(memory[0x400:0x408], 'little') ^ count
        if not (size >= 8 and index <= size - 8):
            success, result = False, index ^ 0xbad0000000000000
            break
        state = int.from_bytes(memory[index:index + 8], 'little')
        result = state ^ ((history + fresh) & MASK)
        if count >= rounds:
            success = True
            break
        original.put(memory, 0x400, x ^ (count + 0x55))
        store_index = sp - 168 if fault == 'stale_pointer' else index + (64 if name == 'history_mixed' and path == 1 else 0)
        original.put(memory, store_index, 14)
        if path == 2 and count == 0:
            frame += 8
        history = fresh
        count += 1
    live = frame ^ fresh ^ history
    for offset, value in ((0, result), (8, count), (16, live), (24, fresh), (32, history), (40, index)):
        original.put(regs, offset, value)
    original.put(memory, 0x500, result); original.put(memory, 0x508, live)
    fields = (1 if success else 5, 0, 0x6200, index, int(success), x, 0 if success else 3)
    return struct.pack('<QQ7Q', result, 0, *fields) + regs + memory

def model(key, fault=None):
    name, path, _, x = key
    name = name.removesuffix('_suffix')
    if fault:
        fault = fault.removeprefix('suffix_')
    if name.startswith('history'):
        return historical_model(key, fault)
    regs, memory = map(bytearray, initial_bytes(x))
    sp, index, size, alias = parameters(key)
    calls = 0
    if path != 2:
        store_index = sp - 128 + (32 if path == 1 and name != 'same' else 0)
        if name == 'different_base':
            store_index += alias
        if fault == 'wrong_phi' and path == 1:
            store_index = sp - 128
        def unknown_call():
            nonlocal calls
            calls += 1
            regs[48] ^= x & 255
            memory[0x300] ^= (x >> 8) & 255
            original.put(memory, store_index, x ^ 0xfaceb00c12345678)
        if name == 'alias_before':
            original.put(memory, alias, x ^ 0x5555)
        if name == 'call_before':
            unknown_call()
        if fault != 'drop_store':
            original.put(memory, store_index, 50)
        if name == 'alias_after' and fault != 'drop_alias':
            original.put(memory, alias, x ^ 0x5555)
        if name == 'call_after' and fault != 'drop_call':
            unknown_call()
    success = fault == 'bounds' or (size >= 8 and index <= size - 8)
    result = (int.from_bytes(memory[index:index + 8], 'little') ^ x if success
              else index ^ 0xbad0000000000000)
    for offset, value in ((0, result), (8, index + 128), (16, index), (24, path)):
        original.put(regs, offset, value)
    original.put(memory, 0x500, result)
    fields = (1 if success else 5, 0, 0x7000 + ORIGINAL_NAMES.index(name) * 0x100,
              index, int(success), x, 0 if success else 3)
    return struct.pack('<QQ7Q', result, calls, *fields) + regs + memory

def invoke(library, key, override=None):
    name, path, _, x = key
    regs, memory = initial_bytes(x)
    ra = (ctypes.c_ubyte * 512).from_buffer_copy(regs)
    ma = (ctypes.c_uint64 * (65536 // 8)).from_buffer_copy(memory)
    ex = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + n for n in range(7)])
    address = ctypes.addressof(ma)
    sp, _, size, alias = parameters(key)
    descriptor = original.Memory(address, size)
    fn = getattr(library, override or 'pointer_' + name)
    fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(original.Memory), ctypes.c_void_p,
                   ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
                   ctypes.c_uint64 if name.startswith('history') else ctypes.c_void_p]
    fn.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'edge_calls'); calls.value = 0
    result = fn(ra, ctypes.byref(descriptor), ex, path, sp, x, x & 3 if name.startswith('history') else address + alias)
    if descriptor.data != address or descriptor.size != size:
        raise RuntimeError('descriptor mutated')
    return struct.pack('<QQ7Q', result, calls.value, *ex) + bytes(ra) + bytes(ma)

def self_test():
    count = 0
    for key in INPUTS:
        expected = model(key)
        if differences(expected, expected):
            raise RuntimeError('identity comparator failed')
        for field, begin, _ in GROUPS:
            bad = bytearray(expected); bad[begin] ^= 1
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
    p.add_argument('--require-suffix-gains', action='store_true')
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
                  require_gains=a.require_gains, require_suffix_gains=a.require_suffix_gains,
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
        for name in ('guest_pointer_phi_native.cpp', 'guest_pointer_phi_diff.py',
                     'guest_memory_history_native.cpp', 'guest_memory_history_diff.py',
                     'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
                                 str(sources / 'guest_pointer_phi_native.cpp'), str(library),
                                 '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats, structural = {}, {}
        for line in run('generate', [str(generator), str(work)]).decode().splitlines():
            row = dict(field.split('=') for field in line.split())
            if 'structural' in row:
                name = row.pop('structural')
                if name in structural: raise RuntimeError('duplicate structural control')
                structural[name] = {k: int(v) for k, v in row.items()}
                continue
            name = row.pop('case')
            if name not in NAMES or name in stats:
                raise RuntimeError('unexpected or duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        report['structural_controls'] = structural
        if set(structural) != {'undef', 'closed'} or any(r['invalid'] or r['forwarded'] for r in structural.values()):
            raise RuntimeError('structural refusal control failed')
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['available'] or not r['unknown_retained'] for r in stats.values()):
            raise RuntimeError('missing or invalid fixture')
        report['precision_ok'] = all(stats[n]['redirected'] and stats[n]['forwarded'] and stats[n]['target_loads'] == 1 for n in GAINS)
        report['suffix_precision_ok'] = all(stats[n]['redirected'] and stats[n]['forwarded'] and stats[n]['target_loads'] == 1 for n in SUFFIX_GAINS)
        report['rejection_ok'] = all(not stats[n]['redirected'] for n in REJECTS + SUFFIX_REJECTS)
        report['historical_gain'] = bool(stats['history']['redirected'] and stats['history']['forwarded'] and stats['history']['target_loads'] == 1)
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
                target = CONTROL_TARGETS[control]
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
        planned_controls=2 * sum(sum(key[0] == CONTROL_TARGETS[c] for key in INPUTS) for c in CONTROLS)
        report['coverage_ok'] = (report['normal_comparisons'] == len(INPUTS) * 4
                                 and report['control_comparisons'] == planned_controls
                                 and report['controls_required'] == report['controls_detected'])
        report['ok'] = (report['semantic_ok'] and report['coverage_ok'] and report['rejection_ok']
                        and (not a.require_gains or report['precision_ok'])
                        and (not a.require_suffix_gains or report['suffix_precision_ok']))
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
