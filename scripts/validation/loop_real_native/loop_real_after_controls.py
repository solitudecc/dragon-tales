"""Execute the frozen four-control domain; no lifting, product pass, C or UC reference."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import traceback
sys.dont_write_bytecode=True
import loop_real_after_common as C

def counts(path):
    raw=Path(path).read_bytes()
    C.require(len(raw)==24 and raw[:8]==b'DTLOBS1\0','observer sidecar length/magic/EOF differs')
    return list(struct.unpack('<2Q',raw[8:]))

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',type=Path,required=True);ap.add_argument('--plan-sha256',required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();out=a.out.resolve();pins={};rows=[];controls=[];failure=None;attempts=[]
    C.require(C.sha(a.plan)==a.plan_sha256,'controls plan SHA differs');raw_plan=a.plan.read_bytes();plan=json.loads(raw_plan)
    C.require(plan['schema']=='loop-real-four-controls-plan-v1' and plan['ready'] and plan['limits']==C.LIMITS,
        'controls plan schema/readiness/limits differ')
    C.require(not out.exists() and sorted(os.sched_getaffinity(0))==[0,1,2,3],'new output/CPU affinity required')
    C.require(shutil.disk_usage(out.parent).free>=536870912,'512MiB startup gate')
    C.require(not os.environ.get('LD_PRELOAD') and not os.environ.get('DRAGON_LOOP_OBSERVER_OUTPUT'),'unexpected preload/observer env')
    ledger,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    for item in plan['sources']:C.pinned(item,pins)
    C.require(any(r['path']==str(Path(__file__)) and r['sha256']==C.sha(__file__) for r in plan['sources']),'worker source not pinned')
    for item in [plan['python'],plan['reference_environment'],plan['runtime']['backend'],plan['runtime']['shim'],plan['solver']['library'],
                 *plan['tools'].values(),*plan['ordinary_executables'].values(),*plan['observer_executables'].values(),
                 plan['source_frame'],plan['nominal_expected']]:C.pinned(item,pins,True)
    for key in ['driver','observer_helper','emission_report']:C.pinned(plan['bindings'][key],pins)
    for row in plan['faults']:C.pinned(row['IR'],pins)
    for row in plan['changed_inputs']:C.pinned(row['input'],pins)
    C.require([r['case'] for r in plan['faults']]==['omitted-wrapper-binding','accepted-plus-two'] and
        [r['case'] for r in plan['changed_inputs']]==['used-table-slot48','SP-plus-eight'],'four-control domain changed')
    C.require(sys.executable==plan['python']['path'],'reference interpreter differs')
    helper,adapter,base,parse_nominal,mutate,compat=C.oracle(roles,plan,pins)
    reference=json.loads(Path(plan['reference_environment']['path']).read_text());C.pinned(reference['UC_native'],pins,True)
    maps=helper.verify_maps(plan['runtime'],plan['solver'],reference)
    original=Path(plan['source_frame']['path']).read_bytes();nominal_blob=Path(plan['nominal_expected']['path']).read_bytes()
    nominal_expected=parse_nominal(nominal_blob);out.mkdir();original_run=subprocess.run;side=[None];seq=[0]
    def run(*args,**kwargs):
        kwargs['timeout']=180
        if side[0] is not None:kwargs['env']=dict(os.environ,DRAGON_LOOP_OBSERVER_OUTPUT=str(side[0]))
        C.require(shutil.disk_usage(out).free>=67108864,'64MiB running floor');seq[0]+=1
        prefix=out/('subprocess-%03d'%seq[0]);started=time.monotonic()
        C.write(str(prefix)+'.invocation.json',dict(argv=[str(x) for x in args[0]],timeout=180,sidecar=str(side[0]) if side[0] else None))
        try:r=original_run(*args,**kwargs)
        except BaseException as error:
            C.write(str(prefix)+'.result.json',dict(exception=type(error).__name__,detail=str(error),seconds=time.monotonic()-started));raise
        C.write(str(prefix)+'.result.json',dict(returncode=r.returncode,seconds=time.monotonic()-started))
        for name in ['stdout','stderr']:
            data=getattr(r,name,None)
            if data is not None:Path(str(prefix)+'.'+name).write_bytes(data if isinstance(data,bytes) else data.encode())
        return r
    subprocess.run=run
    def command(argv,work,name):
        with (work/(name+'.stdout')).open('wb') as stdout,(work/(name+'.stderr')).open('wb') as stderr:
            subprocess.run([str(x) for x in argv],cwd=work,check=True,stdout=stdout,stderr=stderr)
    def native(exe,work,blob,observed,role):
        C.require(len(blob)==66048,'input EOF differs');work.mkdir(parents=True)
        attempts.append(dict(role=role,executable=str(exe),cwd=str(work)));C.write(out/'ATTEMPTS.json',attempts)
        side[0]=work/'output-0.counts' if observed else None
        try:actual,error=base.native(exe,blob[:512],blob[512:],work,0,extra_bytes=64)
        finally:side[0]=None
        C.require(error is None,'native execution failed '+repr(error));raw=(work/'output-0.bin').read_bytes();helper.decode_actual(raw)
        C.require((work/'input-0.bin').read_bytes()==blob,'actual input differs')
        return actual,raw,counts(work/'output-0.counts') if observed else None
    def progress():C.write(out/'PROGRESS.json',dict(results=rows,controls=controls))
    try:
        C.write(out/'INVOCATION.json',dict(plan_sha256=a.plan_sha256,argv=sys.argv,initial_maps=maps,
            original_comparison_fields=adapter.fields,new_C_UC_calls=0,nominal64_recounted=0))
        # Each changed input is first executed on its corresponding original LLVM binary.
        for item in plan['changed_inputs']:
            blob=Path(item['input']['path']).read_bytes()
            delta=[dict(offset=i,space='register' if i<512 else 'Guest',space_offset=i if i<512 else i-512,before=b,after=c)
                for i,(b,c) in enumerate(zip(original,blob)) if b!=c]
            C.require(delta==item['byte_deltas'],'changed input byte delta differs')
            for level in ['O0','O2']:
                work=out/item['case']/level
                peer,expected_blob,_=native(Path(plan['ordinary_executables'][level]['path']),work/'ordinary-peer',blob,False,'ordinary-changed-input')
                peer_record=struct.unpack('<8Q',peer[3]);expected=(peer[0],peer[1],peer[2],peer_record[:7])
                if item['case']=='used-table-slot48':
                    C.require(peer_record[0:2]==(2,1) and peer_record[3]==0xdead and int.from_bytes(peer[0][128:136],'little')==0xdead,
                        'mutated used slot did not produce its concrete ordinary TRANSFER target')
                else:C.require(peer_record==C.ROOT_EXIT,'SP-plus-eight sentinel migration did not preserve ordinary root RET')
                row=dict(case=item['case'],level=level,role='ordinary-changed-input',exit_and_budget=list(peer_record),
                    output_sha256=C.sha(work/'ordinary-peer/output-0.bin'),reference_is_UC=False)
                rows.append(row);progress()
                actual,raw,hit=native(Path(plan['observer_executables'][level]['path']),work/'observer',blob,True,'observer-fallback')
                differences,record=adapter.compare(dict(undefined_flags=[]),expected,actual,False)
                if raw!=expected_blob:differences.append(dict(field='raw_snapshot',detail='complete66136byte mismatch'))
                C.require(not differences and hit==[0,1],'fallback output/count differs from same-input ordinary LLVM peer')
                rows.append(dict(case=item['case'],level=level,role='observer-fallback',status='match',differences=differences,
                    observer_counts=hit,exit_and_budget=list(record),raw_snapshot_equal=raw==expected_blob,
                    output_sha256=C.sha(work/'observer/output-0.bin')))
                for field in ['RAX','memory','exit']:
                    controls.append(dict(case=item['case'],level=level,**mutate(adapter,expected,actual,field)))
                progress()
        # These valid-IR mutants must preserve nominal full state while failing the actual1/0 observation gate.
        for item in plan['faults']:
            for level in ['O0','O2']:
                work=out/item['case']/level;work.mkdir(parents=True);module=Path(item['IR']['path']);chosen=module
                shutil.copyfile(plan['bindings']['driver']['path'],work/'driver.c')
                C.require(C.sha(work/'driver.c')==plan['bindings']['driver']['sha256'],'driver copy differs')
                tools={k:Path(v['path']) for k,v in plan['tools'].items()}
                command([tools['opt'],'-passes=verify','-disable-output',module],work,'verify-raw')
                if level=='O2':
                    chosen=work/'optimized.ll';command([tools['opt'],'-S','-passes=default<O2>',module,'-o',chosen],work,'optimize')
                    C.require(chosen.read_text().splitlines()[0]=="; ModuleID = '"+str(module)+"'",'actual opt ModuleID differs')
                command([tools['opt'],'-passes=verify','-disable-output',chosen],work,'verify-final')
                command([tools['llc'],'-O0','-filetype=obj','-mtriple='+plan['target_triple'],chosen,'-o','module.o'],work,'llc')
                command([tools['cc'],'-O0','-I'+str(compat/'include'),'-I'+str(compat/'c_shim'),'driver.c','module.o',
                    plan['bindings']['observer_helper']['path'],'-o','native'],work,'native-link')
                command([tools['readelf'],'-d',work/'native'],work,'native-dynamic')
                deps=(work/'native-dynamic.stdout').read_text();C.require('libdragon' not in deps and 'libLLVM' not in deps,'mutant Guest binary links SDK/LLVM')
                C.require('unreachable' not in chosen.read_text(),'unexpected unreachable in valid mutant')
                actual,raw,hit=native(work/'native',work/'execution',original,True,'observer-runtime-fault')
                differences,record,is_root=helper.complete_compare(adapter,nominal_expected,actual,raw,nominal_blob)
                C.require(not differences and is_root,'observer-only fault changed nominal full state')
                C.require(hit==item['expected_observer'] and hit!=[1,0],'specified runtime observer fault not detected')
                rows.append(dict(case=item['case'],level=level,role='observer-runtime-fault',nominal_fullframe_match=True,
                    observer_counts=hit,expected_nominal_gate=[1,0],observer_gate_rejected=True,specified_detection=True,
                    exit_and_budget=list(record),output_sha256=C.sha(work/'execution/output-0.bin')));progress()
        C.require(C.recheck(pins) and a.plan.read_bytes()==raw_plan,'post source/input/tool/runtime pins differ')
        C.write(out/'FINAL-MAPS.json',helper.verify_maps(plan['runtime'],plan['solver'],reference))
    except BaseException as error:
        failure=dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc());C.write(out/'FAILURE.json',failure)
    finally:
        subprocess.run=original_run
        totals=dict(ordinary_changed_input_references=sum(r['role']=='ordinary-changed-input' for r in rows),
            observer_fallback_fullframe_matches=sum(r['role']=='observer-fallback' for r in rows),
            runtime_observer_fault_observations=sum(r['role']=='observer-runtime-fault' for r in rows),
            specified_observer_fault_detections=sum(r.get('specified_detection',False) for r in rows),comparator_controls=len(controls))
        try:unchanged=C.recheck(pins) and a.plan.read_bytes()==raw_plan
        except BaseException:unchanged=False
        complete=totals==plan['expected_counts'];passed=complete and failure is None and unchanged
        C.write(out/'RESULTS.json',dict(schema='loop-real-four-controls-results-v1',passed=passed,complete=complete,
            plan_sha256=a.plan_sha256,counts=totals,results=rows,controls=controls,attempts=attempts,failure=failure,
            pins_unchanged=unchanged,new_C_runs=0,new_UC_runs=0,nominal64_recounted=0,skipped=0,undefined_exclusions=0,normalizations=0))
    return 0 if passed else 1

if __name__=='__main__':raise SystemExit(main())
