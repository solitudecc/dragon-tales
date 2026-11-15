#!/usr/bin/env python3
"""Portable nominal actual-after replay. Execution needs a separately sealed plan/GO.

One common main/NoZ3 IR artifact is compiled at O0/O2:32 uninstrumented runs,
then32 separately counted observer runs. Original C/UC16 and before32 are
references, not executed or recounted. No lift or product pass is called.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode=True
import loop_real_after_common as C

def sidecar(path):
    raw=Path(path).read_bytes()
    C.require(len(raw)==24 and raw[:8]==b'DTLOBS1\0','observer sidecar length/magic/EOF differs')
    return struct.unpack('<2Q',raw[8:])

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',type=Path,required=True)
    ap.add_argument('--plan-sha256',required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();plan_bytes=a.plan.read_bytes()
    C.require(C.sha(a.plan)==a.plan_sha256,'plan SHA differs')
    plan=json.loads(plan_bytes);out=a.out.resolve();pins={};rows=[];controls=[];failure=None
    attempts=dict(after=0,observer=0)
    C.require(plan['schema']=='loop-real-after-nominal-plan-v1' and plan['ready'] is True,'nominal plan is not sealed/ready')
    C.require(plan['limits']==C.LIMITS and plan['host_llvm_levels']==['O0','O2'] and plan['groups']==['after','observer'],
              'original limits/domain/groups differ')
    C.require(not out.exists() and not out.is_relative_to(Path(plan['transfer_root']).resolve()),'new output outside frozen inputs required')
    C.require(shutil.disk_usage(out.parent).free>=C.LIMITS['start_free'],'512MiB startup gate')
    C.require(sorted(os.sched_getaffinity(0))==plan['allowed_cpus'],'CPU affinity differs')
    C.require(not os.environ.get('LD_PRELOAD') and not os.environ.get('DRAGON_LOOP_OBSERVER_OUTPUT'),'unexpected preload/observer environment')
    C.require(C.pinned(plan['sources']['runner'],pins).resolve()==Path(__file__).resolve(),'runner identity differs')
    C.require(C.pinned(plan['sources']['common'],pins).resolve()==Path(C.__file__).resolve(),'common helper identity differs')
    data,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    for record in plan['source_pins']:
        C.pinned(record,pins,record.get('allow_symlink',False))
    python=C.pinned(plan['python'],pins,True)
    C.require(str(python)==sys.executable,'explicit reference interpreter differs')
    reference=json.loads(C.pinned(plan['reference_environment'],pins).read_text())
    C.require(reference['ready'] and reference['interpreter']==str(python),'correct reference environment differs')
    C.pinned(reference['UC_native'],pins,True)
    for key in ['backend','shim']:
        C.pinned(plan['runtime'][key],pins,True)
    C.pinned(plan['solver']['library'],pins,True)
    for name in ['opt','llc','cc','readelf']:
        C.pinned(plan['tools'][name],pins,True)
    helper,adapter,base,parse_expected,mutate,compat=C.oracle(roles,plan,pins)
    initial_maps=helper.verify_maps(plan['runtime'],plan['solver'],reference)
    C.require([row['trial'] for row in plan['inputs']]==list(range(16)) and
              [row['value'] for row in plan['inputs']]==C.VALUES,'frozen16 input domain differs')
    trials=[]
    for item in plan['inputs']:
        i=item['trial'];ib=C.pinned(item['input'],pins).read_bytes();eb=C.pinned(item['expected'],pins).read_bytes()
        C.require(Path(item['input']['path'])==roles['initial_frames'][i] and
                  Path(item['expected']['path'])==roles['expected_frames'][i],'trial role mapping differs')
        C.require(len(ib)==66048,'initial512+65536 EOF differs')
        trials.append((i,ib[:512],ib[512:],parse_expected(eb),eb))
    emission=json.loads(C.pinned(plan['bindings']['emission_report'],pins).read_text())
    C.require(emission['input_sha256']==C.AFTER_SHA and emission['production_input_unchanged'] and
              emission['whole_modules_verified'] and emission['binding']['retargeted_direct_calls']==1 and
              emission['binding']['only_one_callee_operand_changed'] and
              emission['observation']['ordinary_fallback_call_unchanged'],'actual binding/observer gates failed')
    modules={group:C.pinned(plan['bindings'][group+'_IR'],pins) for group in ['after','observer']}
    C.require(C.sha(modules['after'])==emission['native_adapter_sha256'] and
              C.sha(modules['observer'])==emission['native_observer_sha256'],'emitted IR pin differs')
    driver=C.pinned(plan['bindings']['driver'],pins)
    observer_helper=C.pinned(plan['bindings']['observer_helper'],pins)
    out.mkdir();sequence=[0];original_run=subprocess.run;observer_output=[None]
    def limited_run(*args,**kwargs):
        # Keep the old before worker's effective per-subprocess180s policy.
        kwargs['timeout']=C.LIMITS['subprocess_seconds']
        if observer_output[0] is not None:
            kwargs['env']=dict(os.environ,DRAGON_LOOP_OBSERVER_OUTPUT=str(observer_output[0]))
        C.require(shutil.disk_usage(out).free>=C.LIMITS['running_free'],'64MiB running floor')
        sequence[0]+=1;tag=out/('subprocess-%03d'%sequence[0]);started=time.monotonic()
        C.write(str(tag)+'.invocation.json',dict(argv=[str(x) for x in args[0]],
                  observer_output=str(observer_output[0]) if observer_output[0] else None,
                  timeout_seconds=C.LIMITS['subprocess_seconds']))
        try:
            result=original_run(*args,**kwargs)
        except BaseException as error:
            C.write(str(tag)+'.result.json',dict(exception=type(error).__name__,detail=str(error),seconds=time.monotonic()-started))
            for name in ['stdout','stderr']:
                raw=getattr(error,name,None)
                if raw is not None:Path(str(tag)+'.'+name).write_bytes(raw if isinstance(raw,bytes) else raw.encode())
            raise
        C.write(str(tag)+'.result.json',dict(returncode=result.returncode,seconds=time.monotonic()-started))
        for name in ['stdout','stderr']:
            raw=getattr(result,name,None)
            if raw is not None:Path(str(tag)+'.'+name).write_bytes(raw if isinstance(raw,bytes) else raw.encode())
        return result
    subprocess.run=limited_run
    def command(argv,work,name):
        with (work/(name+'.stdout')).open('wb') as stdout,(work/(name+'.stderr')).open('wb') as stderr:
            subprocess.run([str(x) for x in argv],cwd=work,check=True,stdout=stdout,stderr=stderr)
    try:
        C.write(out/'INVOCATION.json',dict(schema=plan['schema'],plan_sha256=a.plan_sha256,argv=sys.argv,
            source_pins=pins,comparison_fields=[list(row) for row in adapter.fields],worker_initial_maps=initial_maps,
            input_domain=C.VALUES,groups=plan['groups'],common_IR_from_main_and_NoZ3=True,
            new_C_runs=0,new_Unicorn_runs=0,before_matches_recounted=0,old_UC16_recounted=0,
            lift_runs=0,product_pass_runs=0,emitter_runs=0))
        triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(platform.machine())
        C.require(triple is not None and triple==plan['target_triple'],'host triple differs')
        tools={name:Path(record['path']) for name,record in plan['tools'].items()}
        for group in ['after','observer']:
            for level in ['O0','O2']:
                work=out/group/level;work.mkdir(parents=True)
                shutil.copyfile(driver,work/'driver.c')
                C.require(C.sha(work/'driver.c')==plan['bindings']['driver']['sha256'],'adapted driver copy changed')
                module=modules[group];chosen=module
                C.require(module.read_text().splitlines()[0]==plan['bindings'][group+'_ModuleID_literal'],
                          'actual emitted ModuleID literal differs; never normalize')
                command([tools['opt'],'-passes=verify','-disable-output',module],work,'verify-raw')
                if level=='O2':
                    chosen=work/'optimized.ll'
                    command([tools['opt'],'-S','-passes=default<O2>',module,'-o',chosen],work,'optimize')
                    C.require(chosen.read_text().splitlines()[0]=="; ModuleID = '"+str(module)+"'",'stock opt actual ModuleID differs')
                command([tools['opt'],'-passes=verify','-disable-output',chosen],work,'verify-final')
                command([tools['llc'],'-O0','-filetype=obj','-mtriple='+triple,chosen,'-o','module.o'],work,'llc')
                link=[tools['cc'],'-O0','-I'+str(compat/'include'),'-I'+str(compat/'c_shim'),'driver.c','module.o']
                if group=='observer':link.append(observer_helper)
                command([*link,'-o','native'],work,'native-link')
                command([tools['readelf'],'-d',work/'native'],work,'native-dynamic')
                deps=(work/'native-dynamic.stdout').read_text()
                C.require('libdragon' not in deps and 'libLLVM' not in deps,'Guest binary unexpectedly links an SDK/LLVM DSO')
                bad_ir='unreachable' in chosen.read_text() # unchanged old before gate
                for trial,regs,memory,want,eb in trials:
                    attempts[group]+=1
                    C.write(out/'ATTEMPTS.json',dict(native_attempts_by_group=attempts,
                        current=dict(phase=group,host_llvm_level=level,trial=trial)))
                    observer_output[0]=work/('output-%d.counts'%trial) if group=='observer' else None
                    try:
                        actual,error=base.native(work/'native',regs,memory,work,trial,extra_bytes=64)
                    finally:
                        observer_output[0]=None
                    row=dict(case='flatten-switch-O2',phase=group,host_llvm_level=level,trial=trial)
                    if error:
                        row.update(status=error['kind'],detail=error)
                    else:
                        raw=(work/('output-%d.bin'%trial)).read_bytes();helper.decode_actual(raw)
                        differences,record,root=helper.complete_compare(adapter,want,actual,raw,eb)
                        if bad_ir:differences.append(dict(field='ir',detail='unexpected unreachable'))
                        counts=None
                        if group=='observer':
                            counts=sidecar(work/('output-%d.counts'%trial))
                            if counts!=(1,0):differences.append(dict(field='observer_counts',got=list(counts),expected=[1,0]))
                        row.update(status='different' if differences else 'match',differences=differences,
                            exit=list(record[:7]),budget_left=record[7],root_return=root,raw_snapshot_equal=raw==eb,
                            register_fields_compared=76,memory_bytes_compared=65536,
                            output_sha256=C.sha(work/('output-%d.bin'%trial)),
                            input_sha256=C.sha(work/('input-%d.bin'%trial)),observer_counts=list(counts) if counts else None)
                        C.require((work/('input-%d.bin'%trial)).read_bytes()==regs+memory,'native initial frame copy differs')
                        if not differences:
                            for field in ['RAX','memory','exit']:
                                controls.append(dict(phase=group,host_llvm_level=level,trial=trial,**mutate(adapter,want,actual,field)))
                    rows.append(row);C.write(out/'PROGRESS.json',dict(results=rows,controls=controls))
                    C.require(row['status']=='match','actual after/observer mismatch; preserve raw and stop')
        C.require(C.recheck(pins) and a.plan.read_bytes()==plan_bytes,'input/source/runtime/tool pins changed')
        final_maps=helper.verify_maps(plan['runtime'],plan['solver'],reference)
        C.write(out/'FINAL-MAPS.json',final_maps)
    except BaseException as error:
        failure=dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc())
        C.write(out/'FAILURE.json',failure)
    finally:
        subprocess.run=original_run
        counts={group:sum(row['phase']==group for row in rows) for group in ['after','observer']}
        control_counts={group:sum(row['phase']==group for row in controls) for group in ['after','observer']}
        complete=counts==dict(after=32,observer=32) and control_counts==dict(after=96,observer=96)
        try:
            unchanged=C.recheck(pins)
        except BaseException as error:
            unchanged=False
            if failure is None:failure=dict(type=type(error).__name__,detail='post-pin verification: '+str(error))
        if not unchanged and failure is None:failure=dict(type='InputChanged',detail='post-pin verification failed')
        C.write(out/'RESULTS.json',dict(schema='loop-real-after-nominal-results-v1',plan_sha256=a.plan_sha256,
            correct=complete and failure is None and all(row['status']=='match' for row in rows),complete=complete,
            native_comparisons_by_group=counts,native_attempts_by_group=attempts,
            comparator_controls_by_group=control_counts,results=rows,controls=controls,
            failure=failure,skipped=0,undefined_exclusions=0,normalizations=0,unique_inputs=16,
            commonIR_main_NoZ3_replayed_once=True,NoZ3_duplicate_runs=0,
            new_C_runs=0,new_Unicorn_runs=0,before_matches_recounted=0,old_UC16_recounted=0,
            lift_runs=0,product_pass_runs=0,emitter_runs=0,pins_unchanged=unchanged))
    return 0 if complete and failure is None else 1

if __name__=='__main__':raise SystemExit(main())
