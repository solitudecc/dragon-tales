#!/usr/bin/env python3
"""Isolate old-B snapshot semantics while preserving the original producer/model.

Only a pure iteration add's placement changes. Original phi/load/freeze modes
remain intact in their own files and are also replayed in the original shape.
"""
import argparse,ctypes,gzip,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
import loop_carried_context_diff as original
shared=original.shared
NAMES=('phi','load','freeze')
INPUTS=tuple(key for key in original.INPUTS if key[0] in NAMES)
CONTROLS=original.CONTROLS
HELPER=original.HELPER
model=original.model
invoke=original.invoke

def self_test():
    count=0
    for key in INPUTS:
        wanted=model(key)
        if shared.differences(wanted,wanted):raise RuntimeError('identity comparison failed')
        for field,begin,end in shared.GROUPS:
            bad=bytearray(wanted);bad[begin]^=1
            if shared.differences(bad,wanted)!=[field]:raise RuntimeError('field control escaped')
            count+=1
    for size in (0,shared.SNAPSHOT_BYTES-1,shared.SNAPSHOT_BYTES+1):
        try:shared.differences(bytes(size),model(INPUTS[0]))
        except RuntimeError:count+=1
        else:raise RuntimeError('length control escaped')
    return count

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
        for name in ('loop_carried_isolated_native.cpp','loop_carried_isolated_diff.py','loop_carried_context_native.cpp','loop_carried_context_diff.py','state_guard_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent/name,sources/name)
        report['source_sha256']={p.name:shared.digest(p) for p in sources.iterdir()}
        include=run('llvm-include',[a.llvm_config,'--includedir']).decode().strip()
        generator=work/'generate'
        run('compile-generator',[a.cxx,'-std=c++20','-O0','-I'+str(repo/'include'),'-I'+include,str(sources/'loop_carried_isolated_native.cpp'),str(library),'-Wl,-rpath,'+str(library.parent),'-o',str(generator)])
        run('generator-ldd',['ldd',str(generator)])
        stats={}
        for line in run('generate',[str(generator),str(work)]).decode().splitlines():
            row=dict(field.split('=') for field in line.split());name=row.pop('case')
            if name not in NAMES or name in stats:raise RuntimeError('unexpected or duplicate fixture')
            stats[name]={k:int(v) for k,v in row.items()}
        report['pass_stats']=stats
        if set(stats)!=set(NAMES) or any(r['invalid'] or not r['unknown_retained'] for r in stats.values()):raise RuntimeError('missing fixture, invalid IR or changed unknown edge')
        if a.baseline and any(stats[n]['redirected'] for n in NAMES):raise RuntimeError('old isolated domain unexpectedly redirected')
        if a.require_gains and any(stats[n]['redirected']!=1 or not stats[n]['body_redirected'] or not stats[n]['index16'] for n in NAMES):raise RuntimeError('isolated old-B context did not reach the target load')
        report['self_controls']=self_test()
        (work/'helper.c').write_text(HELPER)
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
                    for key in (v for v in INPUTS if v[0]=='phi'):
                        if shared.differences(model(key,control),model(key)):wanted.add(key)
                        actual=invoke(native,key,'control_'+control);f.write(actual);bad=shared.differences(actual,model(key))
                        report['control_comparisons']+=1;report['control_mismatches']+=bool(bad)
                        if bad:found.add(key)
                        report['rows'].append(dict(level=level,control=control,key=key,fields=bad,snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                report['controls_required']+=len(wanted);report['controls_detected']+=len(wanted&found)
                if found!=wanted:raise RuntimeError('fault control differed from predeclared set: '+control)
        if shared.digest(library)!=pin:raise RuntimeError('runtime changed')
        report['semantic_ok']=report['normal_mismatches']==0
        report['coverage_ok']=report['normal_comparisons']==len(INPUTS)*4 and report['control_comparisons']==len([k for k in INPUTS if k[0]=='phi'])*len(CONTROLS)*2 and report['controls_required']==report['controls_detected']
        report['ok']=report['semantic_ok'] and report['coverage_ok']
    except Exception as e:report['errors'].append(type(e).__name__+': '+str(e));report['ok']=False
    report['artifact_sha256']={str(p.relative_to(work)):shared.digest(p) for p in sorted(work.rglob('*')) if p.is_file()}
    (work/'report.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({k:report.get(k) for k in ('inputs','normal_comparisons','normal_mismatches','control_comparisons','control_mismatches','controls_required','controls_detected','semantic_ok','errors','ok')}))
    return 0 if report['ok'] else 1
if __name__=='__main__':sys.exit(main())
