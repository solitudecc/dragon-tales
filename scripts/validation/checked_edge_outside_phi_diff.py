#!/usr/bin/env python3
"""Small full-state extension of the frozen checked-memory byte oracle.

An external argument or entry LOAD passes through a success PHI and affects the
actual result. The existing comparator and byte model stay unchanged; this file
models only that extra source-level XOR. No old failed comparison is removed.
"""
import argparse, ctypes, gzip, hashlib, json, os, shutil, struct, subprocess, sys
from pathlib import Path
import guest_memory_checked_edges_diff as base

NAMES = ('outside_arg', 'outside_load')
INPUTS = tuple((name, path, bound, x) for name in NAMES for path in (0, 1)
               for bound in (0, 1) for x in base.VALUES)

def model(key):
    name, path, bound, x = key
    original_key = ('plain', path, bound, x)
    result = bytearray(base.model(original_key))
    if struct.unpack_from('<Q', result, 16)[0] == 1:
        initial_regs, _ = base.initial(original_key)
        initial16 = struct.unpack_from('<Q', initial_regs, 16)[0]
        outside = x if name == 'outside_arg' else initial16
        value = struct.unpack_from('<Q', result)[0] ^ outside
        struct.pack_into('<Q', result, 0, value)
        struct.pack_into('<Q', result, 72, value)
        struct.pack_into('<Q', result, 72 + 16, initial16 ^ value)
        struct.pack_into('<Q', result, 16 + 3 * 8, (value + 0x5000) & base.MASK)
    return bytes(result)

def invoke(library, key):
    name, path, bound, x = key
    regs, memory = base.initial(('plain', path, bound, x))
    r = (ctypes.c_ubyte * base.REG_BYTES).from_buffer_copy(regs)
    m = (ctypes.c_ubyte * base.MEM_BYTES).from_buffer_copy(memory)
    e = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + i for i in range(7)])
    address = ctypes.addressof(m)
    sp, offset, size, rounds, alias = base.parameters(('plain', path, bound, x))
    descriptor = base.Memory(address, size)
    f = getattr(library, name)
    f.argtypes = [ctypes.c_void_p, ctypes.POINTER(base.Memory), ctypes.c_void_p,
                  ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_void_p]
    f.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'edge_calls'); calls.value = 0
    value = f(r, ctypes.byref(descriptor), e, path, sp, x, rounds, address + alias)
    if descriptor.data != address or descriptor.size != size: raise RuntimeError('descriptor changed')
    return struct.pack('<QQ7Q', value, calls.value, *e) + bytes(r) + bytes(m)

def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True); p.add_argument('--work', type=Path, required=True)
    a = p.parse_args(); repo, library, work = (v.expanduser().resolve() for v in (a.repo, a.library, a.work))
    if work.exists(): p.error('--work must be new')
    work.mkdir(parents=True); env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    report = dict(inputs=len(INPUTS), commands=[], rows=[], errors=[], normal_comparisons=0,
        normal_mismatches=0, control_comparisons=0, controls_required=0, controls_detected=0,
        excluded=0, skipped=0, normalized=0)
    def run(name, argv):
        r = subprocess.run(argv, capture_output=True, timeout=180, env=env)
        (work/(name+'.stdout')).write_bytes(r.stdout); (work/(name+'.stderr')).write_bytes(r.stderr)
        report['commands'].append(dict(name=name, argv=argv, returncode=r.returncode))
        if r.returncode: raise RuntimeError(name+' failed: '+str(r.returncode))
        return r.stdout
    try:
        report['library_sha256'] = pin = base.digest(library)
        sources = work/'sources'; sources.mkdir()
        for name in ('checked_edge_outside_phi_native.cpp','checked_edge_outside_phi_diff.py',
                     'guest_memory_checked_edges_native.cpp','guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent/name,sources/name)
        report['source_sha256'] = {p.name:base.digest(p) for p in sources.iterdir()}
        include = run('llvm-include',['llvm-config-21','--includedir']).decode().strip()
        run('compile-generator',['c++','-std=c++20','-O0','-I'+str(repo/'include'),'-I'+include,
            str(sources/'checked_edge_outside_phi_native.cpp'),str(library),'-Wl,-rpath,'+str(library.parent),'-o',str(work/'generate')])
        stats = {}
        for line in run('generate',[str(work/'generate'),str(work)]).decode().splitlines():
            row = dict(f.split('=') for f in line.split()); name = row.pop('case')
            if name in stats or name not in NAMES: raise RuntimeError('duplicate/unexpected fixture')
            stats[name] = {k:int(v) for k,v in row.items()}
        report['pass_stats'] = stats
        if set(stats)!=set(NAMES) or any(s['invalid'] or not s['redirected'] or not s['forwarded'] or s['unknown_edges']!=3 for s in stats.values()):
            raise RuntimeError('edge copy, forwarding or retained-unknown invariant failed')
        (work/'helper.c').write_text(base.HELPER)
        with gzip.open(work/'expected.bin.gz','wb',compresslevel=1) as stream:
            for key in INPUTS: stream.write(model(key))
        for level in ('O0','O2'):
            for stage in ('before','after','control'):
                label=stage+'-'+level; output=work/(label+'.so')
                run('compile-'+label,['clang-21','-'+level,'-shared','-fPIC','-Wno-override-module']+
                    [str(work/(n+'-'+stage+'.ll')) for n in NAMES]+[str(work/'helper.c'),'-o',str(output)])
                native=ctypes.CDLL(str(output))
                with gzip.open(work/(label+'.bin.gz'),'wb',compresslevel=1) as stream:
                    for key in INPUTS:
                        actual=invoke(native,key);stream.write(actual);difference=base.differences(actual,model(key))
                        report['rows'].append(dict(stage=stage,level=level,key=key,fields=difference,
                            snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                        if stage=='control':
                            expected=key[2]==0
                            report['control_comparisons']+=1;report['controls_required']+=expected
                            report['controls_detected']+=bool(difference) and expected
                            if bool(difference)!=expected:raise RuntimeError('outside-value fault escaped or changed failure path')
                        else:
                            report['normal_comparisons']+=1;report['normal_mismatches']+=bool(difference)
        if base.digest(library)!=pin:raise RuntimeError('runtime changed')
        report['ok']=(report['normal_comparisons']==128 and not report['normal_mismatches']
            and report['control_comparisons']==64 and report['controls_required']==report['controls_detected']==32)
    except Exception as error:
        report['errors'].append(type(error).__name__+': '+str(error));report['ok']=False
    report['artifact_sha256']={str(p.relative_to(work)):base.digest(p) for p in sorted(work.rglob('*')) if p.is_file()}
    (work/'report.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('rows','commands','artifact_sha256','source_sha256','pass_stats')}))
    return 0 if report['ok'] else 1
if __name__=='__main__':sys.exit(main())
