#!/usr/bin/env python3
"""Measure bounded state contexts around an unchanged full-state real replay.

The old single-guard gate is preserved in its original script. This separate
gate accepts duplicated target switches only when checks, cases and defaults
remain, and reports duplication as such rather than CFF removal.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
sys.dont_write_bytecode=True

ENTRY=0x10080
GUARD='bb_65712'
CONSTANT_INDEX=70096

def root_body(ir):
    match=re.search(r'^define [^\n]*@dragon_static_entry_x86_64_10080\([^\n]*\).*?^}',ir,re.M|re.S)
    if not match:raise RuntimeError('missing frozen f-cff O2 root')
    return match[0]

def blocks(ir):
    return dict(re.findall(r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|^})',root_body(ir),re.M|re.S))

def successors(body):return re.findall(r'label %([\w.]+)',body)

def switch_shape(body):
    switch=re.search(r'^\s*switch i64 (%[\w.]+), label %([\w.]+) \[\n(.*?)^\s*\]',body,re.M|re.S)
    if not switch:return None
    return dict(selector=switch[1],default=switch[2],cases=[(int(v),dest) for v,dest in
        re.findall(r'i64 (-?\d+), label %([\w.]+)',switch[3])])

def identity_uses_load(root,selector,loaded):
    definitions=dict(re.findall(r'^\s*(%[\w.]+) = (.*)$',root,re.M))
    pending=[selector];seen=set()
    while pending and len(seen)<256:
        value=pending.pop()
        if value==loaded:return True
        if value in seen:continue
        seen.add(value);rhs=definitions.get(value,'')
        if rhs.startswith(('phi i64 ','freeze i64 ')):
            pending.extend(re.findall(r'%[\w.]+',rhs))
    return False

def constant_guest_loads(ir,index=CONSTANT_INDEX):
    root=root_body(ir);descriptors={'%memory'}
    for _ in range(4):
        for dest,base in re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr (%[\w.]+), i64 0\b',root):
            if base in descriptors:descriptors.add(dest)
    bases={dest for dest,address in re.findall(r'(%[\w.]+) = load ptr, ptr (%[\w.]+),',root) if address in descriptors}
    addresses={dest for dest,base,offset in re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr (%[\w.]+), i64 (-?\d+)\b',root)
               if base in bases and int(offset)==index}
    result=[]
    for value,pointer in re.findall(r'(%[\w.]+) = load i64, ptr (%[\w.]+),',root):
        if pointer not in addresses:continue
        consumers=[name for name,body in blocks(ir).items() if (shape:=switch_shape(body)) and
                   identity_uses_load(root,shape['selector'],value)]
        if consumers:result.append(dict(value=value,pointer=pointer,index=index,target_switches=consumers))
    return result

def condition_has_copied_load(new,path,condition):
    definitions={name:rhs for body in new.values() for name,rhs in
                 re.findall(r'^\s*(%[\w.]+) = (.*)$',body,re.M)}
    copied_loads={name for block in path for name in
                  re.findall(r'^\s*(%[\w.]+) = load i64, ptr ',new[block],re.M)}
    pending=[condition];seen=set()
    while pending and len(seen)<256:
        value=pending.pop()
        if value in copied_loads:return True
        if value in seen:continue
        seen.add(value);pending.extend(re.findall(r'%[\w.]+',definitions.get(value,'')))
    return False

def context_shape(before,after,final_before,final_after):
    old,new=blocks(before),blocks(after)
    record=dict(known_state=50,constant_index=CONSTANT_INDEX,copied_path=[],valid=False,
                constant_loads_before=constant_guest_loads(final_before),constant_loads_after=constant_guest_loads(final_after))
    if GUARD not in old or GUARD not in new:return record
    condition=re.search(r'^\s*(%[\w.]+) = icmp ugt i64 (%[\w.]+), 50\s*$',old[GUARD],re.M)
    if not condition:return record
    phi=re.search(r'^\s*'+re.escape(condition[2])+r' = phi i64 (.*)$',old[GUARD],re.M)
    if not phi:return record
    known=re.findall(r'\[\s*50,\s*%([\w.]+)\s*\]',phi[1])
    if len(known)!=1:return record
    predecessor=known[0];record['known_predecessor']=predecessor
    if predecessor not in new:return record
    fresh=[label for label in successors(new[predecessor]) if label in new and label not in old]
    if len(fresh)!=1:return record
    current=fresh[0];seen=set();bounds=None
    for _ in range(6):
        if current in old or current not in new or current in seen:return record
        seen.add(current);record['copied_path'].append(current)
        branch=re.search(r'^\s*br i1 (%[\w.]+), label %([\w.]+), label %([\w.]+)\s*$',new[current],re.M)
        if branch:bounds=current;condition=branch[1];table=branch[2];failure=branch[3];break
        next_=re.search(r'^\s*br label %([\w.]+)\s*$',new[current],re.M)
        if not next_:return record
        current=next_[1]
    if bounds is None or table in old or table not in new:return record
    shape=switch_shape(new[table])
    if shape is None:return record
    matches=[name for name,body in old.items() if (candidate:=switch_shape(body)) and
             candidate['cases']==shape['cases'] and candidate['default']==shape['default']]
    if len(matches)!=1:return record
    old_table=matches[0]
    copied_loads=re.findall(r'^\s*(%[\w.]+) = load i64, ptr ',new[table],re.M)
    original_loads=re.findall(r'^\s*(%[\w.]+) = load i64, ptr ',old[old_table],re.M)
    switch_reads_copied_load=bool(len(copied_loads)==len(original_loads) and any(
        identity_uses_load(root_body(after),shape['selector'],value) for value in copied_loads))
    original_bounds=[name for name,body in old.items() if successors(body)==[old_table,failure]]
    if len(original_bounds)!=1:return record
    original_bounds=original_bounds[0]
    expected_unknown=[name for name,body in old.items() if name!=predecessor and GUARD in successors(body)]
    retained_unknown=[name for name in expected_unknown if name in new and GUARD in successors(new[name])]
    record.update(bounds_copy=bounds,original_bounds=original_bounds,table_copy=table,original_table=old_table,
                  failure=failure,switch_cases=len(shape['cases']),switch_default=shape['default'],
                  original_table_preserved=new.get(old_table)==old[old_table],
                  original_guard_conditional=bool(re.search(r'^\s*br i1 ',new[GUARD],re.M)),
                  remaining_unknown_incoming=retained_unknown,
                  all_original_unknown_incoming_preserved=set(expected_unknown)==set(retained_unknown),
                  bounds_depend_on_copied_load=condition_has_copied_load(new,record['copied_path'],condition),
                  switch_reads_copied_load=switch_reads_copied_load)
    # Comparing exact old table instructions would reject legitimate outside
    # SSA repair names. Require the actual LOAD + same complete switch instead.
    old_shape=switch_shape(new.get(old_table,''))
    original_retained=bool(old_shape and old_shape['cases']==shape['cases'] and old_shape['default']==shape['default']
                           and re.search(r'= load i64, ptr ',new[old_table]))
    record['original_table_preserved']=original_retained
    record['valid']=bool(original_retained and record['original_guard_conditional'] and record['remaining_unknown_incoming']
                         and record['all_original_unknown_incoming_preserved'] and record['bounds_depend_on_copied_load']
                         and record['switch_reads_copied_load']
                         and re.search(r'= load i64, ptr ',new[table]) and failure in old and table not in old
                         and len(record['constant_loads_after'])>len(record['constant_loads_before']))
    return record

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--frozen',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--tigress',required=True)
    p.add_argument('--ssa-after-gvn',action='store_true')
    group=p.add_mutually_exclusive_group(required=True);group.add_argument('--baseline',action='store_true');group.add_argument('--require-context',action='store_true')
    a=p.parse_args();a.out=a.out.resolve()
    import dragon as d
    import guest_memory_checked_edges_real as backend
    original_add=d.PassPipeline.add;captures=[];active=None
    def capture(stage):
        def record(ctx,opts):
            nonlocal active
            if stage=='before':
                if active is not None:raise RuntimeError('nested context capture')
                active=dict(before=ctx.target.ir())
            else:
                if active is None:raise RuntimeError('context capture missing before')
                active['after']=ctx.target.ir();captures.append(active);active=None
            return d.PassResult()
        return record
    def add(pipeline,name,**options):
        if name!='dispatch-specialize':return original_add(pipeline,name,**options)
        pipeline.register('measure-context-before',capture('before'),read_only=True)
        pipeline.register('measure-context-after',capture('after'),read_only=True)
        original_add(pipeline,'measure-context-before');original_add(pipeline,name,**options)
        return original_add(pipeline,'measure-context-after')
    old_argv=sys.argv;d.PassPipeline.add=add
    try:
        sys.argv=[str(Path(backend.__file__)),'--frozen',str(a.frozen),'--out',str(a.out),'--tigress',a.tigress]
        if a.ssa_after_gvn:sys.argv.append('--ssa-after-gvn')
        status=backend.main()
    finally:sys.argv=old_argv;d.PassPipeline.add=original_add
    modules=json.loads((a.out/'modules.json').read_text());rows=[]
    if len(captures)!=4 or len(modules)!=4 or active is not None:raise RuntimeError('missing or extra module captures')
    for number,(module,ir) in enumerate(zip(modules,captures)):
        work=Path(module['work']);before=work/'before-context.ll';after=work/'after-context.ll'
        before.write_text(ir['before']);after.write_text(ir['after'])
        row=dict(kernel=module['kernel'],work=str(work),capture_index=number,before_sha256=digest(before),after_sha256=digest(after))
        if module['kernel']==ENTRY:
            row['context']=context_shape(ir['before'],ir['after'],(work/'before-pipeline.ll').read_text(),(work/'after-pipeline.ll').read_text())
        rows.append(row)
    focus=[row for row in rows if row['kernel']==ENTRY]
    valid=len(focus)==1 and bool(focus[0]['context']['valid'])
    summary=json.loads((a.out/'summary.json').read_text())
    result=dict(ok=status==0 and summary['ok'] and (not valid if a.baseline else valid),
        baseline=a.baseline,require_context=a.require_context,semantic_returncode=status,semantic_summary_sha256=digest(a.out/'summary.json'),
        harness_sha256=digest(__file__),backend_sha256=digest(backend.__file__),rows=rows,
        target_switch_counts=summary['root_counts'],dispatch_specialize=summary['dispatch_specialize_runs'],
        GuestSSA_incomplete_modules=summary['GuestSSA_incomplete_modules'],exclusions=[],skips=[],normalizations=[],
        context_switch_duplication_is_not_CFF_removal=True)
    save(a.out/'context-summary.json',result)
    print(json.dumps(dict(ok=result['ok'],baseline=a.baseline,context=focus[0]['context'] if focus else None),indent=2))
    return 0 if result['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
