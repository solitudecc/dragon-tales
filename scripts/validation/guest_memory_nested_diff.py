#!/usr/bin/env python3
"""Full-state independent nested-loop oracle; allocation layouts are not semantic inputs."""
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
SHAPES=('nested','multi','self')
NAMES=tuple(f'{shape}_{layout}' for shape in SHAPES for layout in range(8))
VALUES=(0,1,0x7fffffff,0x80000000,0x8000000000000000,0x7fffffffffffffff,0xffffffff,0xffffffffffffffff)
INPUTS=tuple((name,outer,inner,x) for name in NAMES for outer in (0,1,2,3) for inner in (0,1,2) for x in VALUES)
CONTROLS=tuple(f'{shape}_{fault}' for shape in SHAPES for fault in ('drop_store','stale_load'))
SNAPSHOT_BYTES=original.SNAPSHOT_BYTES
MASK=original.MASK
GROUPS=original.GROUPS
digest=original.digest
differences=original.differences
@lru_cache(None)
def initial_bytes(x):
    regs=bytes((i*13+(x&255))&255 for i in range(512))
    pattern=bytes((i*37+((x>>8)^x))&255 for i in range(256))
    return regs,pattern*256

def model(key,fault=None):
    name,outer,inner,x=key
    shape=name.rsplit('_',1)[0]
    regs,memory=map(bytearray,initial_bytes(x))
    state=x; original.put(memory,0x100,state)
    total=acc=0
    for n in range(outer&3):
        if shape=='multi':
            state=(x+n)&MASK;original.put(memory,0x100,state)
        observed=x if fault and fault.endswith('stale_load') else state
        acc=(acc+observed)&MASK
        for j in range(inner%3):
            if not (fault and fault.endswith('drop_store')):
                state=x^((n*0x101+j*0x10001+0x55)&MASK)
                original.put(memory,0x100,state)
            total+=1
        if shape=='multi' and n&1:original.put(memory,0x110,acc)
    result=acc^state
    for offset,v in ((0,result),(8,outer&3),(16,total),(24,state)):original.put(regs,offset,v)
    original.put(memory,0x200,result);original.put(memory,0x208,total)
    fields=(1,0,0x5000+SHAPES.index(shape)*0x100,state,1,x,total)
    return struct.pack('<QQ7Q',result,0,*fields)+regs+memory

def invoke(library,key,override=None):
    name,outer,inner,x=key
    regs,memory=initial_bytes(x)
    ra=(ctypes.c_ubyte*512).from_buffer_copy(regs)
    ma=(ctypes.c_uint64*(65536//8)).from_buffer_copy(memory)
    ex=(ctypes.c_uint64*7)(*[0xcafe000000000000+n for n in range(7)])
    address=ctypes.addressof(ma);desc=original.Memory(address,65536)
    f=getattr(library,override or 'nested_'+name)
    f.argtypes=[ctypes.c_void_p,ctypes.POINTER(original.Memory),ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64]
    f.restype=ctypes.c_uint64
    result=f(ra,ctypes.byref(desc),ex,x,outer,inner)
    if desc.data!=address or desc.size!=65536:raise RuntimeError('descriptor mutated')
    return struct.pack('<QQ7Q',result,0,*ex)+bytes(ra)+bytes(ma)

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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--repo', type=Path)
    p.add_argument('--library', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--baseline', action='store_true')
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
        for name in ('guest_memory_nested_native.cpp', 'guest_memory_nested_diff.py',
                     'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {f.name: digest(f) for f in sources.iterdir()}
        include = run('llvm-include', [a.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [a.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'), '-I' + include,
                                 str(sources / 'guest_memory_nested_native.cpp'), str(library),
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
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['available'] for r in stats.values()):
            raise RuntimeError('missing or invalid fixture')
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
                target = control.split('_',1)[0]+'_0'
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
        planned_controls=2*sum(sum(key[0]==c.split('_',1)[0]+'_0' for key in INPUTS) for c in CONTROLS)
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
