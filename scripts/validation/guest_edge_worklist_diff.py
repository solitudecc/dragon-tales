#!/usr/bin/env python3
"""Full-state independent native oracle for multiple original checked-memory incoming edges."""
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

NAMES = ('different', 'same', 'hub', 'history')
VALUES = (0, 1, 2, 3, 0x80, 0x8000, 0x80000000, 1 << 63, 0x7fffffffffffffff, 0xffffffffffffffff)
INPUTS = tuple((name, path, bound, x) for name in NAMES
               for path in range(3 if name == 'history' else 6 if name == 'hub' else 5)
               for bound in range(3) for x in VALUES)
CONTROLS = ('wrong_store', 'drop_store', 'drop_alias', 'drop_call', 'bounds',
            'fresh_history', 'stale_fresh', 'reentry', 'liveout')
CONTROL_TARGETS = {c: 'history' if c in CONTROLS[5:] else 'different' for c in CONTROLS}
SNAPSHOT_BYTES, MASK, GROUPS = original.SNAPSHOT_BYTES, original.MASK, original.GROUPS
digest, differences = original.digest, original.differences
initial_bytes = history_source.initial_bytes

def parameters(key):
    name, path, bound, x = key
    sp = 256 + (x & 1) * 256
    index = sp + (32 if path == 1 else 0) - 168 if name == 'history' else sp + (0 if name == 'same' else path * 32) - 128
    size = (65536, index + 7, index + 8)[bound]
    alias = index if x & 1 else 0x300
    return sp, index, size, alias

def model(key, fault=None):
    name, path, _, x = key
    regs, memory = map(bytearray, initial_bytes(x))
    sp, index, size, alias = parameters(key)
    calls = 0
    if name == 'history':
        frame, history, count = sp + (32 if path == 1 else 0), x, 0
        while True:
            index = frame - 168
            fresh = int.from_bytes(memory[0x400:0x408], 'little') ^ count
            success = size >= 8 and index <= size - 8
            if not success:
                result = index ^ 0xbad0000000000000
                break
            state = int.from_bytes(memory[index:index+8], 'little')
            result = state ^ (((fresh if fault == 'fresh_history' else history)
                              + (history if fault == 'stale_fresh' else fresh)) & MASK)
            if count >= (x & 3):
                break
            original.put(memory, 0x400, x ^ (count + 0x55))
            original.put(memory, index, 34 if count & 1 else 14)
            if path == 2 and count == 0 and fault != 'reentry':
                frame += 8
            history, count = fresh, count + 1
        live = x if fault == 'liveout' else frame ^ fresh ^ history
        for offset, value in ((0,result),(8,count),(16,live),(24,fresh),(32,history),(40,index)):
            original.put(regs,offset,value)
        original.put(memory,0x500,result); original.put(memory,0x508,live)
        pc = 0x6200
    else:
        if path < 4:
            if fault != 'drop_store':
                original.put(memory,index,34 if path == 1 and fault != 'wrong_store' else 14)
            if path == 2 and fault != 'drop_alias':
                original.put(memory,alias,x ^ 0x5555)
            if path == 3 and fault != 'drop_call':
                calls += 1; regs[48] ^= x & 255; memory[0x300] ^= (x >> 8) & 255
                original.put(memory,index,x ^ 0xfaceb00c12345678)
        success = fault == 'bounds' or size >= 8 and index <= size - 8
        result = int.from_bytes(memory[index:index+8],'little') ^ x if success else index ^ 0xbad0000000000000
        for offset,value in ((0,result),(8,index+128),(16,index),(24,path)):
            original.put(regs,offset,value)
        original.put(memory,0x500,result)
        pc = 0x8000 + NAMES.index(name)*0x100
    fields = (1 if success else 5,0,pc,index,int(success),x,0 if success else 3)
    return struct.pack('<QQ7Q',result,calls,*fields) + regs + memory

def invoke(library,key,override=None):
    name,path,_,x = key
    regs,memory=initial_bytes(x)
    ra=(ctypes.c_ubyte*512).from_buffer_copy(regs)
    ma=(ctypes.c_uint64*(65536//8)).from_buffer_copy(memory)
    ex=(ctypes.c_uint64*7)(*[0xcafe000000000000+n for n in range(7)])
    address=ctypes.addressof(ma); sp,_,size,alias=parameters(key)
    descriptor=original.Memory(address,size)
    fn=getattr(library,override or 'worklist_'+name)
    fn.argtypes=[ctypes.c_void_p,ctypes.POINTER(original.Memory),ctypes.c_void_p,
                 ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64,
                 ctypes.c_uint64 if name=='history' else ctypes.c_void_p]
    fn.restype=ctypes.c_uint64
    calls=ctypes.c_uint64.in_dll(library,'edge_calls');calls.value=0
    result=fn(ra,ctypes.byref(descriptor),ex,path,sp,x,x&3 if name=='history' else address+alias)
    if descriptor.data!=address or descriptor.size!=size:raise RuntimeError('descriptor mutated')
    return struct.pack('<QQ7Q',result,calls.value,*ex)+bytes(ra)+bytes(ma)

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
    p.add_argument('--edge-worklist', action='store_true')
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
                  require_gains=a.require_gains, edge_worklist=a.edge_worklist,
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
        for name in ('guest_edge_worklist_native.cpp', 'guest_edge_worklist_diff.py',
                     'guest_memory_history_native.cpp', 'guest_memory_history_diff.py',
                     'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include, *(['-DEDGE_WORKLIST=1'] if a.edge_worklist else []),
                                 str(sources / 'guest_edge_worklist_native.cpp'), str(library),
                                 '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        stats = {}
        for line in run('generate', [str(generator), str(work)]).decode().splitlines():
            row = dict(field.split('=') for field in line.split())
            name = row.pop('case')
            if name not in NAMES or name in stats: raise RuntimeError('unexpected or duplicate fixture')
            stats[name] = {k: int(v) for k, v in row.items()}
        report['pass_stats'] = stats
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['available'] for r in stats.values()):
            raise RuntimeError('missing or invalid fixture')
        report['precision_ok'] = all(stats[n]['known_redirected']==2 and stats[n]['forwarded']>=2 and stats[n]['target_loads']==1 for n in NAMES)
        report['rejection_ok'] = all(not stats[n]['negative_redirected'] for n in NAMES)
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
                       )
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
