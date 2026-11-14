#!/usr/bin/env python3
"""One frozen B ELF replay, retaining the old complete-state protocol.

Saved C/Unicorn/Python results are references. This runner executes only the
original parsed LLVM or the separately pinned bound wrapper. It never lifts,
calls a product pass, invokes a C oracle or runs Unicorn.
"""
import argparse,collections,hashlib,importlib.util,json,os,platform,re,shutil,struct,subprocess,sys,traceback
from pathlib import Path

OLD='/home/cc/vmc-runs/guarded-store-expressions-20261113/source/replay/guarded_store_expression_real_replay.py'
LIMITS=dict(guest_steps=1000000,subprocess_seconds=180,start_free=536870912,running_free=67108864)

def sha(b):return hashlib.sha256(b).hexdigest()
def digest(p):return sha(Path(p).read_bytes())
def require(ok,msg):
    if not ok:raise RuntimeError(msg)
def write(p,v):Path(p).write_text(json.dumps(v,indent=2,sort_keys=True)+'\n')
def pinned(v):
    p=Path(v['path']);require(p.is_file() and not p.is_symlink(),'unsafe pin '+str(p))
    b=p.read_bytes();require(sha(b)==v['sha256'] and len(b)==v['size'],'changed pin '+str(p));return p
def expected(blob):
    require(len(blob)==66136 and blob[:8]==b'DTFLOW1\0','expected snapshot protocol')
    a,b=struct.unpack_from('<QQ',blob,8);exit_words=struct.unpack('<8Q',blob[-64:])
    require(exit_words== (1,3,4111,61440,1,0,0,0),'saved expected root RET')
    return (blob[24:536],blob[536:66072],collections.Counter({0x2000:a,0x3000:b}),exit_words[:7])
def source_pins(plan):
    pins={str(pinned(v)):v['sha256'] for v in plan['bindings'].values()}
    for v in plan['source_compatibility']['unchanged']:
        require(digest(v['path'])==v['sha256'],'oracle source changed '+v['path']);pins[v['path']]=v['sha256']
    for row in plan['inputs']:
        for name in ('input','expected'):
            v=row[name];pinned(v);pins[v['path']]=v['sha256']
    for v in plan['tools'].values():pinned(v);pins[v['path']]=v['sha256']
    for v in plan['new_sources'].values():pinned(v);pins[v['path']]=v['sha256']
    return pins
def imports():
    spec=importlib.util.spec_from_file_location('frozen_B_real_replay',OLD)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',required=True,type=Path);ap.add_argument('--phase',choices=['before','after'],required=True)
    ap.add_argument('--out',required=True,type=Path);a=ap.parse_args()
    pb=a.plan.read_bytes();plan=json.loads(pb);out=a.out.resolve()
    require(plan['schema']=='dragon-ordered-dispatch-real-replay-plan-v1' and plan['ready'] is True,'unsealed plan')
    require(plan['case']=='manual-O2' and plan['limits']==LIMITS,'case/limits changed')
    require(plan['host_llvm_levels']==['O0','O2'] and len(plan['inputs'])==16,'domain changed')
    require(not plan['source_compatibility']['changed'],'oracle source compatibility failed')
    require(not out.exists() and not a.plan.resolve().is_relative_to(out),'unsafe output namespace')
    R=imports();A,study,base=R.A,R.study,R.base
    initial_free=A.space(out,LIMITS['start_free']);pins=source_pins(plan)
    require(str(Path(OLD))==plan['new_sources']['frozen_replay']['path'],'wrong frozen replay')
    require(Path(__file__).resolve()==pinned(plan['new_sources']['runner']),'runner identity')
    require(len(study.guest.Adapter('x86_64').fields)==76 and base.MEM_SIZE==65536,'comparison ABI')
    runtime=A.runtime_pins();oracle_sources=A.source_pins()
    require(runtime['backends']==[plan['runtime']['backend']],'wrong mapped LLVM provider')
    require(runtime['shim']==plan['runtime']['shim'],'wrong explicit shim')
    calibration_pins={}
    if a.phase=='after':
        before_path=out.parent/'before/RESULTS.json';before=json.loads(before_path.read_text())
        audit_path=out.parent/'before-saved-audit/REPORT.json';audit=json.loads(audit_path.read_text())
        require(before['correct'] and before['complete'] and before['native_comparisons']==32 and
                before['comparator_controls']==96 and before['plan_sha256']==sha(pb),'before calibration failed')
        require(audit['passed'] and audit['phase']=='before' and audit['result_sha256']==digest(before_path),
                'before saved audit failed')
        calibration_pins={str(p):digest(p) for p in [before_path,audit_path]}
    out.mkdir(parents=True);rows=[];controls=[];failure=None
    original_run=subprocess.run
    def limited_run(*args,**kw):kw['timeout']=180;return original_run(*args,**kw)
    subprocess.run=limited_run
    try:
        phase_key='source_parsed' if a.phase=='before' else 'final_bound_wrapper'
        module=pinned(plan['bindings'][phase_key]);module_bytes=module.read_bytes()
        literal=plan['source_ModuleID_literal'] if a.phase=='before' else plan['after_ModuleID_literal']
        require(module_bytes.decode().splitlines()[0]==literal,'actual stage ModuleID differs')
        driver=pinned(plan['bindings']['old_driver']);elf=pinned(plan['bindings']['ELF'])
        _,regions,symbols=A.elf_parts(elf.read_bytes());adapter=study.guest.Adapter('x86_64')
        require(study.INPUTS==[r['value'] for r in plan['inputs']],'old initial-state input domain changed')
        require(symbols['oracle_entry']==4096 and symbols['kernel']==4112,'ELF entry mismatch')
        trials=[]
        for item in plan['inputs']:
            trial=item['trial'];regs,memory=study.initial(adapter,regions,trial)
            ib=pinned(item['input']).read_bytes();eb=pinned(item['expected']).read_bytes()
            require(bytes(regs)+bytes(memory)==ib,'old initial-frame generator differs')
            want=expected(eb)
            require(study.observables(want)==A.heldout.specification(A.SEED,item['value']),'saved independent Python spec differs')
            trials.append((trial,regs,memory,want,eb))
            (out/f'expected-{trial}.bin').write_bytes(eb)
        write(out/'INVOCATION.json',dict(plan_sha256=sha(pb),phase=a.phase,argv=sys.argv,
            source_pins=pins,runtime=runtime,oracle_sources=oracle_sources,initial_free=initial_free,
            actual_source_module=str(module),actual_ModuleID_literal=literal,calibration_pins=calibration_pins,
            native_C_runs=0,Unicorn_runs=0,producer_runs=0,product_pass_runs=0,lift_runs=0))
        tools={k:Path(v['path']) for k,v in plan['tools'].items()}
        triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}[platform.machine()]
        for level in ['O0','O2']:
            work=out/level;work.mkdir();shutil.copyfile(driver,work/'driver.c')
            require(digest(work/'driver.c')==plan['bindings']['old_driver']['sha256'],'driver copy changed')
            selected=module;R.run([tools['opt'],'-passes=verify','-disable-output',module],work,'verify-source')
            if level=='O2':
                selected=work/'optimized.ll'
                R.run([tools['opt'],'-S','-passes=default<O2>',module,'-o',selected],work,'optimize')
                require(selected.read_text().splitlines()[0]=="; ModuleID = '"+str(module)+"'",'stock-opt ModuleID literal differs')
            R.run([tools['opt'],'-passes=verify','-disable-output',selected],work,'verify-final')
            R.run([tools['llc'],'-O0','-filetype=obj','-mtriple='+triple,selected,'-o','module.o'],work,'llc')
            R.run([tools['cc'],'-O0','-I'+str(base.ROOT/'include'),'-I'+str(base.ROOT/'c_shim'),
                   'driver.c','module.o','-o','native'],work,'native-link')
            bad_ir='unreachable' in selected.read_text()
            for trial,regs,memory,want,eb in trials:
                A.space(out,LIMITS['running_free']);actual,error=base.native(work/'native',regs,memory,work,trial,extra_bytes=64)
                row=dict(case='manual-O2',phase=a.phase,host_llvm_level=level,trial=trial)
                if error:row.update(status=error['kind'],detail=error)
                else:
                    diff,record=adapter.compare(dict(undefined_flags=[]),want,actual,False)
                    root_return=record[:7]==(1,3,4111,61440,1,0,0) and record[7]==0
                    raw_equal=(work/f'output-{trial}.bin').read_bytes()==eb
                    if not root_return:diff.append(dict(field='root_return',got=list(record)))
                    if not raw_equal:diff.append(dict(field='raw_snapshot',detail='complete66136byte mismatch'))
                    if bad_ir:diff.append(dict(field='ir',detail='unexpected unreachable'))
                    row.update(status='different' if diff else 'match',differences=diff,exit=list(record[:7]),
                        budget_left=record[7],root_return=root_return,raw_snapshot_equal=raw_equal,
                        register_fields_compared=76,memory_bytes_compared=65536)
                    if not diff:
                        for field in ['RAX','memory','exit']:
                            controls.append(dict(case='manual-O2',phase=a.phase,host_llvm_level=level,trial=trial,
                                                 **R.mutate_control(adapter,want,actual,field)))
                rows.append(row);write(out/'PROGRESS.json',dict(results=rows,controls=controls))
                require(row['status']=='match','full-state calibration/replay mismatch; raw retained')
        require(pins==source_pins(plan) and a.plan.read_bytes()==pb,'input pins changed during run')
        require(all(digest(p)==h for p,h in calibration_pins.items()),'before calibration changed during after')
        require(A.runtime_pins()==runtime and A.source_pins()==oracle_sources,'oracle runtime/source changed')
    except BaseException as e:
        failure=dict(type=type(e).__name__,detail=str(e),traceback=traceback.format_exc())
        write(out/'FAILURE.json',failure);raise
    finally:
        subprocess.run=original_run
        correct=failure is None and len(rows)==32 and len(controls)==96 and all(r['status']=='match' for r in rows)
        write(out/'RESULTS.json',dict(schema='ordered-real-replay-result-v1',plan_sha256=sha(pb),phase=a.phase,
            correct=correct,complete=len(rows)==32 and len(controls)==96,native_comparisons=len(rows),
            statuses=dict(collections.Counter(r['status'] for r in rows)),root_returns=sum(r.get('root_return',False) for r in rows),
            comparator_controls=len(controls),results=rows,controls=controls,failure=failure,
            new_native_C_comparisons=0,new_Unicorn_comparisons=0,producer_runs=0,product_pass_runs=0,lift_runs=0,
            skipped=0,undefined_exclusions=0,normalizations=0))
    print(a.phase,'32 complete-state matches;96 exact-field controls;0 new C/UC')
    return 0

if __name__=='__main__':raise SystemExit(main())
