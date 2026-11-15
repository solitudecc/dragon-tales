"""After-only32 clean-IR replay using the exact original compilation/comparison protocol."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback
sys.dont_write_bytecode=True
import loop_real_after_common as C

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--plan',type=Path,required=True)
    ap.add_argument('--plan-sha256',required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();raw_plan=a.plan.read_bytes();C.require(C.sha(a.plan)==a.plan_sha256,'clean native plan SHA differs')
    plan=json.loads(raw_plan);out=a.out.resolve();pins={};rows=[];controls=[];attempts=0;failure=None
    C.require(plan['schema']=='loop-clean-native-plan-v1' and plan['ready'] and plan['limits']==C.LIMITS and
        plan['host_llvm_levels']==['O0','O2'] and plan['new_observer_evidence'] is False,'clean native domain/limits differ')
    C.require(not out.exists() and sorted(os.sched_getaffinity(0))==[0,1,2,3],'new output/CPU required')
    C.require(shutil.disk_usage(out.parent).free>=536870912,'512MiB startup gate')
    C.require(not os.environ.get('LD_PRELOAD') and not os.environ.get('DRAGON_LOOP_OBSERVER_OUTPUT'),'unexpected preload/observer environment')
    ledger,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    for row in [*plan['sources'],plan['python'],plan['reference_environment'],plan['runtime']['backend'],plan['runtime']['shim'],
        plan['solver']['library'],*plan['tools'].values(),plan['module'],plan['binding_report'],plan['cleanup_report'],plan['driver']]:C.pinned(row,pins,True)
    C.require(sys.executable==plan['python']['path'] and any(x['path']==str(Path(__file__)) for x in plan['sources']),'interpreter/worker identity differs')
    emission=json.loads(Path(plan['binding_report']['path']).read_text())
    C.require(emission['binding_only'] and emission['observer_calls']==0 and emission['production_input_unchanged'] and
        emission['whole_module_verified'] and emission['binding']['only_one_callee_operand_changed'] and
        emission['native_adapter_sha256']==plan['module']['sha256'],'binding-only gates failed')
    helper,adapter,base,expected,mutate,compat=C.oracle(roles,plan,pins)
    reference=json.loads(Path(plan['reference_environment']['path']).read_text());C.pinned(reference['UC_native'],pins,True)
    maps=helper.verify_maps(plan['runtime'],plan['solver'],reference)
    C.require([r['trial'] for r in plan['inputs']]==list(range(16)) and [r['value'] for r in plan['inputs']]==C.VALUES,'frozen16 domain differs')
    trials=[]
    for row in plan['inputs']:
        i=row['trial'];ip=C.pinned(row['input'],pins);ep=C.pinned(row['expected'],pins)
        C.require(ip==roles['initial_frames'][i] and ep==roles['expected_frames'][i],'reference/input mapping differs')
        ib=ip.read_bytes();eb=ep.read_bytes();C.require(len(ib)==66048,'initial EOF differs');trials.append((i,ib,eb,expected(eb)))
    out.mkdir();original_run=subprocess.run;sequence=[0]
    def run(*args,**kwargs):
        kwargs['timeout']=180;C.require(shutil.disk_usage(out).free>=67108864,'64MiB running floor')
        sequence[0]+=1;prefix=out/('subprocess-%03d'%sequence[0]);started=time.monotonic()
        C.write(str(prefix)+'.invocation.json',dict(argv=[str(x) for x in args[0]],timeout_seconds=180))
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
    try:
        C.write(out/'INVOCATION.json',dict(plan_sha256=a.plan_sha256,argv=sys.argv,source_pins=pins,initial_maps=maps,
            fields=adapter.fields,observer_evidence=False,previous76_recounted=0,new_C_runs=0,new_UC_runs=0))
        tools={k:Path(v['path']) for k,v in plan['tools'].items()};module=Path(plan['module']['path'])
        C.require(module.read_text().splitlines()[0]==plan['module_first_line'],'actual bound clean ModuleID differs')
        for level in ['O0','O2']:
            work=out/level;work.mkdir();chosen=module
            shutil.copyfile(plan['driver']['path'],work/'driver.c');C.require(C.sha(work/'driver.c')==plan['driver']['sha256'],'frozen adapted driver copy differs')
            command([tools['opt'],'-passes=verify','-disable-output',module],work,'verify-raw')
            if level=='O2':
                chosen=work/'optimized.ll';command([tools['opt'],'-S','-passes=default<O2>',module,'-o',chosen],work,'optimize')
                C.require(chosen.read_text().splitlines()[0]=="; ModuleID = '"+str(module)+"'",'stock opt actual ModuleID differs')
            command([tools['opt'],'-passes=verify','-disable-output',chosen],work,'verify-final')
            command([tools['llc'],'-O0','-filetype=obj','-mtriple='+plan['target_triple'],chosen,'-o','module.o'],work,'llc')
            command([tools['cc'],'-O0','-I'+str(compat/'include'),'-I'+str(compat/'c_shim'),'driver.c','module.o','-o','native'],work,'native-link')
            command([tools['readelf'],'-d',work/'native'],work,'native-dynamic')
            deps=(work/'native-dynamic.stdout').read_text();C.require('libdragon' not in deps and 'libLLVM' not in deps,'Guest binary links SDK/LLVM')
            bad_ir='unreachable' in chosen.read_text()
            for trial,ib,eb,want in trials:
                attempts+=1;C.write(out/'ATTEMPTS.json',dict(count=attempts,level=level,trial=trial))
                actual,error=base.native(work/'native',ib[:512],ib[512:],work,trial,extra_bytes=64)
                C.require(error is None,'clean native execution failed '+repr(error))
                raw=(work/('output-%d.bin'%trial)).read_bytes();helper.decode_actual(raw)
                differences,record,is_root=helper.complete_compare(adapter,want,actual,raw,eb)
                if bad_ir:differences.append(dict(field='ir',detail='unexpected unreachable'))
                C.require((work/('input-%d.bin'%trial)).read_bytes()==ib,'native initial frame differs')
                rows.append(dict(level=level,trial=trial,status='different' if differences else 'match',differences=differences,
                    raw_snapshot_equal=raw==eb,root_return=is_root,exit_and_budget=list(record),register_fields=76,
                    output_sha256=C.sha(work/('output-%d.bin'%trial)),input_sha256=C.sha(work/('input-%d.bin'%trial))))
                if not differences:
                    for field in ['RAX','memory','exit']:controls.append(dict(level=level,trial=trial,**mutate(adapter,want,actual,field)))
                C.write(out/'PROGRESS.json',dict(results=rows,controls=controls));C.require(not differences,'clean complete-state mismatch; preserve and stop')
        C.require(C.recheck(pins) and a.plan.read_bytes()==raw_plan,'post source/input/tool/runtime pins differ')
        C.write(out/'FINAL-MAPS.json',helper.verify_maps(plan['runtime'],plan['solver'],reference))
    except BaseException as error:
        failure=dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc());C.write(out/'FAILURE.json',failure)
    finally:
        subprocess.run=original_run
        try:unchanged=C.recheck(pins) and a.plan.read_bytes()==raw_plan
        except BaseException:unchanged=False
        complete=len(rows)==32 and len(controls)==96;correct=complete and failure is None and unchanged
        C.write(out/'RESULTS.json',dict(schema='loop-clean-native-results-v1',correct=correct,complete=complete,
            native_attempts=attempts,native_comparisons=len(rows),comparator_controls=len(controls),results=rows,controls=controls,
            failure=failure,pins_unchanged=unchanged,new_C_runs=0,new_UC_runs=0,new_observer_evidence=False,
            previous76_recounted=0,skipped=0,undefined_exclusions=0,normalizations=0,unique_inputs=16,plan_sha256=a.plan_sha256))
    return 0 if correct else 1

if __name__=='__main__':raise SystemExit(main())
