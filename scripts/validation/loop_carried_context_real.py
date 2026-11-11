#!/usr/bin/env python3
"""Batch-context precision around the unchanged frozen full-state replay.

The 01c5abd baseline already resolves more states after repeated calls. This
gate measures one-call coverage and records copies, never CFF removal.
"""
import argparse,hashlib,json,re,sys
from pathlib import Path
import state_context_real as prior
sys.dont_write_bytecode=True

ENTRY=0x10080
HEAD='bb_65712'
BASE=69696

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,indent=2)+'\n')

def batch_contexts(before,after,final):
    old,new=prior.blocks(before),prior.blocks(after)
    result=dict(contexts=[],partial_contexts=[],invalid_redirects=[],unknown_incoming=[],unknown_retained=[],valid=False)
    if HEAD not in old or HEAD not in new:return result
    branch=re.search(r'^\s*br i1 (%[\w.]+),',old[HEAD],re.M)
    condition=(re.search(r'^\s*'+re.escape(branch[1])+r' = icmp ugt i64 (%[\w.]+), 50\s*$',old[HEAD],re.M) if branch else None)
    phi=(re.search(r'^\s*'+re.escape(condition[1])+r' = phi i64 (.*)$',old[HEAD],re.M) if condition else None)
    if not phi:return result
    incoming=re.findall(r'\[\s*([^,]+),\s*%([\w.]+)\s*\]',phi[1])
    original_switches={name:shape for name,body in old.items() if (shape:=prior.switch_shape(body))}
    known={pred:int(value) for value,pred in incoming if re.fullmatch(r'-?\d+',value.strip())}
    result['known_incoming']=known
    result['unknown_incoming']=[pred for value,pred in incoming if pred not in known]
    result['unknown_retained']=[pred for pred in result['unknown_incoming'] if pred in new and HEAD in prior.successors(new[pred])]
    for pred,state in known.items():
        if pred not in new:continue
        copies=[label for label in prior.successors(new[pred]) if label not in old and label in new]
        if len(copies)!=1:continue
        current=copies[0];path=[];bounds=None
        for _ in range(6):
            if current in old or current not in new or current in path:break
            path.append(current)
            conditional=re.search(r'^\s*br i1 (%[\w.]+), label %([\w.]+), label %([\w.]+)\s*$',new[current],re.M)
            if conditional:
                bounds=(current,*conditional.groups());break
            direct=re.search(r'^\s*br label %([\w.]+)\s*$',new[current],re.M)
            if not direct:break
            current=direct[1]
        if not bounds:
            result['partial_contexts'].append(dict(predecessor=pred,state=state,path=path))
            continue
        bound_block,cond,left,right=bounds
        candidates=[]
        for table,failure in ((left,right),(right,left)):
            if table not in new or table in old or failure not in old:continue
            shape=prior.switch_shape(new[table])
            if not shape:continue
            originals=[name for name,s in original_switches.items() if s['cases']==shape['cases'] and s['default']==shape['default']]
            if len(originals)!=1:continue
            old_table=originals[0]
            loads=re.findall(r'^\s*(%[\w.]+) = load i64, ptr ',new[table],re.M)
            selector_link=any(prior.identity_uses_load(prior.root_body(after),shape['selector'],value) for value in loads)
            retained=prior.switch_shape(new.get(old_table,''))
            retained=bool(retained and retained['cases']==shape['cases'] and retained['default']==shape['default'] and re.search(r'= load i64, ptr ',new[old_table]))
            index=BASE+state*8
            associated=prior.constant_guest_loads(final,index)
            raw_condition=re.search(r'^\s*%[\w.]+ = icmp ugt i64 (-?\d+), 50\s*$',new[path[0]],re.M)
            row=dict(predecessor=pred,state=state,path=path,bounds=bound_block,table=table,failure=failure,
                     raw_state_matches=bool(raw_condition and int(raw_condition[1])==state),
                     copied_bound_load=prior.condition_has_copied_load(new,path,cond),
                     selector_uses_table_load=selector_link,original_switch_retained=retained,
                     same_cases_default=True,case_count=len(shape['cases']),index=index,target_loads=associated)
            row['valid']=bool(0<=state<=50 and row['raw_state_matches'] and row['copied_bound_load'] and selector_link and retained and associated)
            candidates.append(row)
        if len(candidates)==1:result['contexts'].append(candidates[0])
        elif any(label not in old and label in new and prior.switch_shape(new[label]) for label in (left,right)):
            result['invalid_redirects'].append(dict(predecessor=pred,state=state,path=path))
        else:result['partial_contexts'].append(dict(predecessor=pred,state=state,path=path))
    result['distinct_states']=sorted({row['state'] for row in result['contexts'] if row['valid']})
    result['distinct_indices']=sorted({row['index'] for row in result['contexts'] if row['valid']})
    result['valid']=bool(result['contexts'] and not result['invalid_redirects'] and all(row['valid'] for row in result['contexts']) and
                         set(result['unknown_incoming'])==set(result['unknown_retained']) and
                         re.search(r'^\s*br i1 ',new[HEAD],re.M))
    return result

def baseline_ok(record):return record.get('valid') and len(record['contexts'])==1 and record['distinct_states']==[50]
def batching_ok(record):return record.get('valid') and len(record['contexts'])>1 and len(record['distinct_states'])>1

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--frozen',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--tigress',required=True)
    p.add_argument('--ssa-after-gvn',action='store_true');p.add_argument('--require-batching',action='store_true')
    a=p.parse_args();a.out=a.out.resolve()
    import dragon as d
    import guest_memory_checked_edges_real as backend
    original_add=d.PassPipeline.add;captures=[];active=None
    def capture(stage):
        def record(ctx,opts):
            nonlocal active
            if stage=='before':
                if active is not None:raise RuntimeError('nested batch capture')
                active=dict(before=ctx.target.ir())
            else:
                if active is None:raise RuntimeError('missing batch before')
                active['after']=ctx.target.ir();captures.append(active);active=None
            return d.PassResult()
        return record
    def add(pipeline,name,**options):
        if name!='dispatch-specialize':return original_add(pipeline,name,**options)
        pipeline.register('batch-capture-before',capture('before'),read_only=True)
        pipeline.register('batch-capture-after',capture('after'),read_only=True)
        original_add(pipeline,'batch-capture-before');original_add(pipeline,name,**options)
        return original_add(pipeline,'batch-capture-after')
    old_argv=sys.argv;d.PassPipeline.add=add
    try:
        sys.argv=[str(Path(backend.__file__)),'--frozen',str(a.frozen),'--out',str(a.out),'--tigress',a.tigress]
        if a.ssa_after_gvn:sys.argv.append('--ssa-after-gvn')
        status=backend.main()
    finally:sys.argv=old_argv;d.PassPipeline.add=original_add
    modules=json.loads((a.out/'modules.json').read_text());rows=[]
    if len(captures)!=4 or len(modules)!=4 or active is not None:raise RuntimeError('missing/extra batch captures')
    for module,ir in zip(modules,captures):
        work=Path(module['work']);before=work/'before-batch.ll';after=work/'after-batch.ll'
        before.write_text(ir['before']);after.write_text(ir['after'])
        row=dict(kernel=module['kernel'],work=str(work),before_sha256=digest(before),after_sha256=digest(after))
        if module['kernel']==ENTRY:row['batch']=batch_contexts(ir['before'],ir['after'],(work/'after-pipeline.ll').read_text())
        rows.append(row)
    focus=[row for row in rows if row['kernel']==ENTRY]
    summary=json.loads((a.out/'summary.json').read_text())
    valid=len(focus)==1 and (batching_ok(focus[0]['batch']) if a.require_batching else baseline_ok(focus[0]['batch']))
    result=dict(ok=status==0 and summary['ok'] and bool(valid),require_batching=a.require_batching,
                semantic_returncode=status,semantic_summary_sha256=digest(a.out/'summary.json'),
                harness_sha256=digest(__file__),rows=rows,target_switch_counts=summary['root_counts'],
                dispatch_specialize=summary['dispatch_specialize_runs'],GuestSSA_incomplete_modules=summary['GuestSSA_incomplete_modules'],
                context_copies_not_CFF_removal=True,exclusions=[],skips=[],normalizations=[])
    save(a.out/'batch-summary.json',result);print(json.dumps(dict(ok=result['ok'],focus=focus),indent=2));return 0 if result['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
