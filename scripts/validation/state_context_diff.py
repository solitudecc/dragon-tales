#!/usr/bin/env python3
"""Full-state independent oracle for bounded multi-block state contexts.

Native executions use defined memory and an explicit finite reentry count.
Guest table bytes are inputs, never supplied to the transformation as facts.
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
import state_guard_diff as shared

NAMES = ('basic', 'two_bridge', 'outer_guard', 'entry_value', 'unknown_call',
         'store', 'atomic', 'volatile', 'fence')
GAINS, REJECTS = NAMES[:4], NAMES[4:]
VALUES = (0, 1, 2, 49, 50, 51, 127, 128, 255, 0x80000000, 1 << 63, (1 << 64) - 1)
SIZES = (0x597, 0x598)
INPUTS = tuple((name, route, size, rounds, x) for name in NAMES for route in range(4)
               for size in SIZES for rounds in (0, 2) for x in VALUES)
MASK = (1 << 64) - 1
TARGETS = (0x1111, 0x2222, 0x3333, 0x4444, 0xdead)
CONTROLS = ('unknown', 'flags', 'failure', 'reentry', 'default')

@functools.lru_cache(maxsize=len(VALUES))
def initial(x):
    regs, raw = shared.initial(x)
    memory = bytearray(raw)
    for state in range(64):
        shared.put(memory, 0x400 + state * 8, TARGETS[(state + x) % len(TARGETS)])
    return regs, bytes(memory)

def state_value(route, x):
    return (50 if route == 0 else x if route == 1 else x ^ 0x55 if route == 2 else x + 1) & MASK

def model(key, fault=None):
    name, route, size, rounds, x = key
    regs, memory = map(bytearray, initial(x))
    old16, old32 = shared.get(regs, 16), shared.get(regs, 32)
    calls = iteration = 0
    state = state_value(route, x)
    if name == 'outer_guard' and x & 1:
        state, flags, index, status, value = 0xabc, 0xdef, 0, 6, 29
    else:
        entering_b = True
        for step in range(5):
            if entering_b:
                flags = state ^ old16 ^ 0xf00dfacecafebeef
                if state > 50 and fault != 'unknown':
                    status, value, index = 2, 17, 0
                    break
                if name == 'unknown_call':
                    calls += 1
                    regs[40] ^= x & 255
                    memory[0x700] ^= (x >> 8) & 255
                elif name == 'store': shared.put(memory, 0x708, x)
                outside = old32 if name == 'entry_value' else old16
                flags = state ^ outside ^ 0xf00dfacecafebeef ^ shared.get(memory, 0x100)
                if name == 'two_bridge': flags ^= shared.get(memory, 0x108)
                if fault == 'flags': flags = 0
            index = ((state << 3) + 0x400) & MASK
            fits = size > 7 and index <= size - 8
            if not fits and fault != 'failure':
                status, value = 3, 23
                break
            # All forced-failure controls still access allocated host storage.
            if index + 8 > shared.MEM_BYTES: raise RuntimeError('oracle attempted an invalid host address')
            target = shared.get(memory, index)
            effective = target if iteration < rounds else TARGETS[0]
            if effective == TARGETS[0]: status, value = 1, target; break
            if effective == TARGETS[1]: status, value = 5, target ^ 0x222; break
            if effective == TARGETS[2]:
                state, entering_b = (50 if fault == 'reentry' else x), True
            elif effective == TARGETS[3]:
                state, entering_b = (50 if fault == 'reentry' else state ^ 1), False
            elif fault == 'default': status, value = 1, target; break
            else: status, value = 4, target; break
            iteration += 1
        else: raise RuntimeError('bounded oracle unexpectedly failed to terminate')
    result = (state ^ flags ^ status ^ value ^ iteration ^ index ^ old32) & MASK
    for offset, value_ in ((0,result),(8,state),(16,flags),(24,iteration),(32,old32 ^ result),(48,index)):
        shared.put(regs, offset, value_)
    shared.put(memory, 0x600, result); shared.put(memory, 0x608, status)
    fields = (status, 0, 0xb000 + NAMES.index(name) * 16, (result + 0x1000) & MASK,
              1, x ^ 0xdeadbeef, iteration)
    return struct.pack('<QQ7Q', result, calls, *fields) + regs + memory

def self_test():
    count = 0
    for key in INPUTS:
        wanted = model(key)
        assert not shared.differences(wanted, wanted)
        for field, begin, end in shared.GROUPS:
            wrong = bytearray(wanted); wrong[begin] ^= 1
            assert shared.differences(wrong, wanted) == [field]
            count += 1
    for length in (0, shared.SNAPSHOT_BYTES - 1, shared.SNAPSHOT_BYTES + 1):
        try: shared.differences(bytes(length), model(INPUTS[0]))
        except RuntimeError: count += 1
        else: raise RuntimeError('malformed snapshot accepted')
    return count

def invoke(library, key, override=None):
    name, route, size, rounds, x = key
    regs, memory = initial(x)
    reg_array = (ctypes.c_ubyte * shared.REG_BYTES).from_buffer_copy(regs)
    mem_array = (ctypes.c_uint64 * (shared.MEM_BYTES // 8)).from_buffer_copy(memory)
    exit_array = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + n for n in range(7)])
    address = ctypes.addressof(mem_array)
    descriptor = shared.Memory(address, size)
    function = getattr(library, override or 'context_' + name)
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(shared.Memory), ctypes.c_void_p,
                        ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'state_guard_calls'); calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exit_array, x, route, rounds)
    if descriptor.data != address or descriptor.size != size: raise RuntimeError('descriptor changed')
    return struct.pack('<QQ7Q', result, calls.value, *exit_array) + bytes(reg_array) + bytes(mem_array)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test',action='store_true');p.add_argument('--repo',type=Path)
    p.add_argument('--library',type=Path);p.add_argument('--work',type=Path)
    group=p.add_mutually_exclusive_group();group.add_argument('--baseline',action='store_true');group.add_argument('--require-gains',action='store_true')
    p.add_argument('--cxx',default='c++');p.add_argument('--clang',default='clang-21');p.add_argument('--llvm-config',default='llvm-config-21')
    a=p.parse_args()
    if a.self_test:
        print(json.dumps(dict(ok=True,inputs=len(INPUTS),controls=self_test())));return 0
    if any(v is None for v in (a.repo,a.library,a.work)):p.error('--repo, --library and --work required')
    repo,library,work=(v.expanduser().resolve() for v in (a.repo,a.library,a.work))
    if work.exists():p.error('--work must be new')
    work.mkdir(parents=True)
    report=dict(inputs=len(INPUTS),library=str(library),baseline=a.baseline,require_gains=a.require_gains,
        commands=[],rows=[],errors=[],normal_comparisons=0,normal_mismatches=0,control_comparisons=0,
        controls_required=0,controls_detected=0,control_mismatches=0,excluded=0,skipped=0,normalized=0)
    env=dict(os.environ,LD_LIBRARY_PATH=str(library.parent))
    def run(name,argv):
        try:r=subprocess.run(argv,capture_output=True,env=env,timeout=180)
        except subprocess.TimeoutExpired as e:
            (work/(name+'.stdout')).write_bytes(e.stdout or b'');(work/(name+'.stderr')).write_bytes(e.stderr or b'');report['commands'].append(dict(name=name,argv=argv,timeout=True));raise
        (work/(name+'.stdout')).write_bytes(r.stdout);(work/(name+'.stderr')).write_bytes(r.stderr)
        report['commands'].append(dict(name=name,argv=argv,returncode=r.returncode))
        if r.returncode:raise RuntimeError(name+' failed: '+str(r.returncode))
        return r.stdout
    try:
        pin=report['library_sha256']=shared.digest(library)
        report['repo_head']=run('repo-head',['git','-C',str(repo),'rev-parse','HEAD']).decode().strip()
        sources=work/'sources';sources.mkdir()
        for name in ('state_context_native.cpp','state_context_diff.py','state_guard_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent/name,sources/name)
        report['source_sha256']={p.name:shared.digest(p) for p in sources.iterdir()}
        include=run('llvm-include',[a.llvm_config,'--includedir']).decode().strip()
        generator=work/'generate'
        run('compile-generator',[a.cxx,'-std=c++20','-O0','-I'+str(repo/'include'),'-I'+include,str(sources/'state_context_native.cpp'),str(library),'-Wl,-rpath,'+str(library.parent),'-o',str(generator)])
        run('generator-ldd',['ldd',str(generator)])
        stats={}
        for line in run('generate',[str(generator),str(work)]).decode().splitlines():
            row=dict(field.split('=') for field in line.split());name=row.pop('case')
            if name not in NAMES or name in stats:raise RuntimeError('unexpected or duplicate fixture')
            stats[name]={k:int(v) for k,v in row.items()}
        report['pass_stats']=stats
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['unknown'] for r in stats.values()):raise RuntimeError('missing fixture, invalid IR or changed unknown edge')
        if a.baseline and any(r['address'] or r['table_loads']!=1 for r in stats.values()):raise RuntimeError('baseline unexpectedly gained table context')
        if a.require_gains and any(not all(stats[n][k] for k in ('bounds','address','switch_targets')) or stats[n]['table_loads']!=2 or stats[n]['bounds_checks']!=2 for n in GAINS):raise RuntimeError('required known-state address context or original checks missing')
        if any(stats[n]['address'] or stats[n]['table_loads']!=1 or stats[n]['bounds_checks']!=1 for n in REJECTS):raise RuntimeError('effectful corridor was extended')
        report['self_controls']=self_test()
        (work/'helper.c').write_text(shared.HELPER)
        with gzip.open(work/'expected.bin.gz','wb',compresslevel=1) as f:
            for key in INPUTS:f.write(model(key))
        for level in ('O0','O2'):
            for stage in ('before','after'):
                label=stage+'-'+level;out=work/(label+'.so')
                run('compile-'+label,[a.clang,'-'+level,'-shared','-fPIC','-Wno-override-module']+[str(work/(n+'-'+stage+'.ll')) for n in NAMES]+[str(work/'helper.c'),'-latomic','-o',str(out)])
                native=ctypes.CDLL(str(out))
                with gzip.open(work/(label+'.bin.gz'),'wb',compresslevel=1) as f:
                    for key in INPUTS:
                        actual=invoke(native,key);f.write(actual);bad=shared.differences(actual,model(key))
                        report['normal_comparisons']+=1;report['normal_mismatches']+=bool(bad)
                        report['rows'].append(dict(level=level,stage=stage,key=key,fields=bad,snapshot_sha256=hashlib.sha256(actual).hexdigest()))
            for control in CONTROLS:
                label='control-'+control+'-'+level;out=work/(label+'.so')
                run('compile-'+label,[a.clang,'-'+level,'-shared','-fPIC','-Wno-override-module',str(work/('control_'+control+'.ll')),str(work/'helper.c'),'-latomic','-o',str(out)])
                native=ctypes.CDLL(str(out));wanted=set();found=set()
                with gzip.open(work/(label+'.bin.gz'),'wb',compresslevel=1) as f:
                    for key in (v for v in INPUTS if v[0]=='basic'):
                        if shared.differences(model(key,control),model(key)):wanted.add(key)
                        actual=invoke(native,key,'control_'+control);f.write(actual);bad=shared.differences(actual,model(key))
                        report['control_comparisons']+=1;report['control_mismatches']+=bool(bad)
                        if bad:found.add(key)
                        report['rows'].append(dict(level=level,control=control,key=key,fields=bad,snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                report['controls_required']+=len(wanted);report['controls_detected']+=len(wanted&found)
                if found!=wanted:raise RuntimeError('fault control differed from predeclared set: '+control)
        if shared.digest(library)!=pin:raise RuntimeError('runtime changed')
        report['semantic_ok']=report['normal_mismatches']==0
        report['coverage_ok']=report['normal_comparisons']==len(INPUTS)*4 and report['control_comparisons']==len([k for k in INPUTS if k[0]=='basic'])*len(CONTROLS)*2 and report['controls_required']==report['controls_detected']
        report['ok']=report['semantic_ok'] and report['coverage_ok']
    except Exception as e:report['errors'].append(type(e).__name__+': '+str(e));report['ok']=False
    report['artifact_sha256']={str(p.relative_to(work)):shared.digest(p) for p in sorted(work.rglob('*')) if p.is_file()}
    (work/'report.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({k:report.get(k) for k in ('inputs','normal_comparisons','normal_mismatches','control_comparisons','control_mismatches','controls_required','controls_detected','semantic_ok','errors','ok')}))
    return 0 if report['ok'] else 1
if __name__=='__main__':sys.exit(main())
