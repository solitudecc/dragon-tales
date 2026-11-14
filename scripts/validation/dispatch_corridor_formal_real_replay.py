#!/usr/bin/env python3
"""After-only actual-library replay of one frozen real B ELF.

References the completed original0619 LLVM32 calibration. No C, Unicorn,
before-native, producer, lift or product-pass invocation is made by this file.
The frozen nominal helper and B oracle retain their original comparison code.
"""
import argparse,collections,hashlib,importlib.util,json,platform,shutil,subprocess,sys,traceback
from pathlib import Path
NOMINAL_SHA='b5516949a17d9de8c71f3529ae3df777f75a9a89ad20d72533ef16f1b2813e5a'
PLAN_SHA='9bacce5f629b95d9582b299f7f2457d2fc0f67f9a3340e56f3dceedcc2b21458'
BEFORE_SHA='5ff6f2b30b58cccb95c3f539ee18a13cd11039deb4cbaf700cac1fa5ffb87be7'
AUDIT_SHA='f991295d76a5f2a5920e38fe441deb9fba82d7c8b4b26824bce521b4d6bfdb13'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def nominal():
    p=Path('/home/cc/vmc-runs/ordered-dispatch-corridor-real-native-20261114-v1/source/dispatch_corridor_real_replay.py')
    assert sha(p)==NOMINAL_SHA
    spec=importlib.util.spec_from_file_location('frozen_nominal_real_helper',p)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--plan',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();N=nominal();check=N.require
    pb=a.plan.read_bytes();plan=json.loads(pb);out=a.out.resolve()
    check(plan['schema']=='ordered-formal-real-replay-plan-v1' and plan['ready'],'unsealed formal plan')
    check(plan['case']=='manual-O2' and plan['limits']==N.LIMITS,'domain/limits changed')
    check(not out.exists() and not a.plan.resolve().is_relative_to(out),'unsafe output')
    reference=N.pinned(plan['nominal_plan']);check(sha(reference)==PLAN_SHA,'wrong nominal source calibration')
    old=json.loads(reference.read_text());check(plan['inputs']==old['inputs'],'original16inputs changed')
    before=N.pinned(plan['before_calibration']['result']);audit=N.pinned(plan['before_calibration']['audit'])
    check(sha(before)==BEFORE_SHA and sha(audit)==AUDIT_SHA,'old calibration hash mismatch')
    b=json.loads(before.read_text());v=json.loads(audit.read_text())
    check(b['correct'] and b['complete'] and b['native_comparisons']==32 and b['comparator_controls']==96 and
          v['passed'] and v['result_sha256']==sha(before),'old original-source calibration failed')
    calpins={str(p):sha(p) for p in [reference,before,audit]}
    source=N.pinned(plan['bindings']['source_parsed']);old_source=N.pinned(old['bindings']['source_parsed'])
    check(source.read_bytes()==old_source.read_bytes(),'actual original source differs from completed calibration')
    for key in ['raw','ELF','old_driver','B_payload','B_frozen','old_C_results','old_replay_results']:
        check(plan['bindings'][key]['sha256']==old['bindings'][key]['sha256'],'old source/ABI changed '+key)
    actual=json.loads(N.pinned(plan['bindings']['actual_producer_result']).read_text())
    report=json.loads(N.pinned(plan['bindings']['static_report']).read_text())
    prefix=json.loads(N.pinned(plan['bindings']['prefix_gate']).read_text())
    check(actual['exit']==0 and not actual['failed'] and actual['pins_unchanged'] and actual['stage_count']==19,'actual producer failed')
    check(report['hard_structure_gate_passed'] and report['ordinary_functions_unchanged'] and report['source_module_unchanged'],'actual structural/identity gate failed')
    check(prefix['passed'] and prefix['files_compared']==18 and prefix['normalization'] is False,'strict prefix gates failed')
    check(all(actual['normal_API_bindings'].values()),'actual library API bindings incomplete')
    R=N.imports();A,study,base=R.A,R.study,R.base
    initial=A.space(out,N.LIMITS['start_free']);pins=N.source_pins(plan)
    check(Path(__file__).resolve()==N.pinned(plan['new_sources']['runner']),'formal runner identity')
    runtime=A.runtime_pins();oracle_sources=A.source_pins()
    check(runtime['backends']==[plan['runtime']['backend']] and runtime['shim']==plan['runtime']['shim'],'formal runtime mismatch')
    out.mkdir(parents=True);rows=[];controls=[];failure=None;old_run=subprocess.run
    def limited_run(*args,**kw):kw['timeout']=180;return old_run(*args,**kw)
    subprocess.run=limited_run
    try:
        module=N.pinned(plan['bindings']['final_bound_wrapper']);driver=N.pinned(plan['bindings']['old_driver'])
        check(module.read_text().splitlines()[0]==plan['after_ModuleID_literal']=="; ModuleID = 'guarded-source'",'actual producer ModuleID mismatch')
        final=report['stages'][-1];check(final['stage']=='06-control-flow' and final['bound_sha256']==sha(module),'wrong final actual module')
        elf=N.pinned(plan['bindings']['ELF']);_,regions,symbols=A.elf_parts(elf.read_bytes())
        adapter=study.guest.Adapter('x86_64');check(len(adapter.fields)==76 and base.MEM_SIZE==65536,'comparison ABI')
        check(study.INPUTS==[x['value'] for x in plan['inputs']] and symbols['oracle_entry']==4096 and symbols['kernel']==4112,'input/ELF domain')
        trials=[]
        for item in plan['inputs']:
            i=item['trial'];regs,memory=study.initial(adapter,regions,i);ib=N.pinned(item['input']).read_bytes();eb=N.pinned(item['expected']).read_bytes()
            check(bytes(regs)+bytes(memory)==ib,'initial frame differs');want=N.expected(eb)
            check(study.observables(want)==A.heldout.specification(A.SEED,item['value']),'saved Python specification differs')
            trials.append((i,regs,memory,want,eb));(out/f'expected-{i}.bin').write_bytes(eb)
        N.write(out/'INVOCATION.json',dict(plan_sha256=N.sha(pb),phase='after',argv=sys.argv,source_pins=pins,
            calibration_pins=calpins,runtime=runtime,oracle_sources=oracle_sources,initial_free=initial,
            actual_source_module=str(module),actual_ModuleID_literal=plan['after_ModuleID_literal'],
            native_C_runs=0,Unicorn_runs=0,producer_runs=0,product_pass_runs=0,lift_runs=0,old_before_matches_recounted=0))
        tools={k:Path(v['path']) for k,v in plan['tools'].items()};triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}[platform.machine()]
        for level in ['O0','O2']:
            w=out/level;w.mkdir();shutil.copyfile(driver,w/'driver.c');selected=module
            R.run([tools['opt'],'-passes=verify','-disable-output',module],w,'verify-source')
            if level=='O2':
                selected=w/'optimized.ll';R.run([tools['opt'],'-S','-passes=default<O2>',module,'-o',selected],w,'optimize')
                check(selected.read_text().splitlines()[0]=="; ModuleID = '"+str(module)+"'",'stock-opt actual ModuleID differs')
            R.run([tools['opt'],'-passes=verify','-disable-output',selected],w,'verify-final')
            R.run([tools['llc'],'-O0','-filetype=obj','-mtriple='+triple,selected,'-o','module.o'],w,'llc')
            R.run([tools['cc'],'-O0','-I'+str(base.ROOT/'include'),'-I'+str(base.ROOT/'c_shim'),'driver.c','module.o','-o','native'],w,'native-link')
            bad_ir='unreachable' in selected.read_text()
            for i,regs,memory,want,eb in trials:
                A.space(out,N.LIMITS['running_free']);got,error=base.native(w/'native',regs,memory,w,i,extra_bytes=64)
                row=dict(case='manual-O2',phase='after',host_llvm_level=level,trial=i)
                if error:row.update(status=error['kind'],detail=error)
                else:
                    differences,record=adapter.compare(dict(undefined_flags=[]),want,got,False)
                    root=record==(1,3,4111,61440,1,0,0,0);raw=(w/f'output-{i}.bin').read_bytes()==eb
                    if not root:differences.append(dict(field='root_return',got=list(record)))
                    if not raw:differences.append(dict(field='raw_snapshot',detail='complete66136byte mismatch'))
                    if bad_ir:differences.append(dict(field='ir',detail='unexpected unreachable'))
                    row.update(status='different' if differences else 'match',differences=differences,exit=list(record[:7]),budget_left=record[7],root_return=root,
                        raw_snapshot_equal=raw,register_fields_compared=76,memory_bytes_compared=65536)
                    if not differences:
                        for field in ['RAX','memory','exit']:controls.append(dict(case='manual-O2',phase='after',host_llvm_level=level,trial=i,**R.mutate_control(adapter,want,got,field)))
                rows.append(row);N.write(out/'PROGRESS.json',dict(results=rows,controls=controls));check(row['status']=='match','actual formal full-state mismatch; raw preserved')
        check(pins==N.source_pins(plan) and a.plan.read_bytes()==pb,'formal inputs changed')
        check(all(sha(p)==h for p,h in calpins.items()),'referenced calibration changed')
        check(A.runtime_pins()==runtime and A.source_pins()==oracle_sources,'oracle source/runtime changed')
    except BaseException as e:
        failure=dict(type=type(e).__name__,detail=str(e),traceback=traceback.format_exc());N.write(out/'FAILURE.json',failure);raise
    finally:
        subprocess.run=old_run;ok=failure is None and len(rows)==32 and len(controls)==96 and all(x['status']=='match' for x in rows)
        N.write(out/'RESULTS.json',dict(schema='ordered-formal-real-replay-v1',plan_sha256=N.sha(pb),phase='after',correct=ok,complete=len(rows)==32 and len(controls)==96,
            native_comparisons=len(rows),root_returns=sum(x.get('root_return',False) for x in rows),statuses=dict(collections.Counter(x['status'] for x in rows)),
            comparator_controls=len(controls),results=rows,controls=controls,failure=failure,new_native_C_comparisons=0,new_Unicorn_comparisons=0,
            producer_runs=0,product_pass_runs=0,lift_runs=0,skipped=0,undefined_exclusions=0,normalizations=0))
    print('actual formal real32 full-state matches/96controls;0newC/UC/before')
if __name__=='__main__':main()
