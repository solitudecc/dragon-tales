#!/usr/bin/env python3
"""Strict structural checks separate from the unchanged native oracle."""
import argparse,hashlib,json,re
from pathlib import Path
from guarded_entry_real import function_span,bind_wrapper
from guarded_entry_prefix_real import ordinary_functions,validate_prefix_report
def cfg(body):return dict(re.findall(r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|^})',body,re.M|re.S))

def prove_first_selector(ir):
 body=function_span(ir,'dragon_static_entry_x86_64_102a0.guarded.real_sp')[0];blocks=cfg(body)
 guard=blocks['guarded.next50'];match=re.search(r'(%[\w.]+) = getelementptr i8, ptr (%[\w.]+), i64 70032\b',guard);assert match
 load=re.search(r'(%[\w.]+) = load i64, ptr '+re.escape(match[1])+r', align 1',guard);assert load
 cmp=re.search(r'(%[\w.]+) = icmp eq i64 '+re.escape(load[1])+r', 67010\b',guard);assert cmp
 edge=re.search(r'br i1 '+re.escape(cmp[1])+r', label %([\w.]+), label %guarded.fallback',guard);assert edge
 assert edge[1].endswith('.entry.prefix')
 reachable=set();pending=[edge[1]]
 while pending:
  name=pending.pop()
  if name in reachable or not name.endswith('.entry.prefix'):continue
  assert name in blocks;reachable.add(name)
  pending.extend(re.findall(r'label %([\w.]+)',blocks[name]))
 assert reachable and all('switch i64' not in blocks[name] for name in reachable)
 target=[]
 for name in reachable:
  text=blocks[name]
  branch=re.search(r'br i1 (%[\w.]+), label %bb_67010, label %(memory.invalid[\w.]+)',text)
  if branch:
   assert re.search(re.escape(branch[1])+r' = icmp ugt i64 %[\w.]+, 70031\b',text)
   assert '= load ' not in text
   assert branch[2] in reachable and re.search(r'store i64 5, ptr %exit, align 1',blocks[branch[2]])
   detail=re.search(r'(%[\w.]+) = getelementptr i8, ptr %exit, i64 48',blocks[branch[2]]);assert detail
   assert re.search(r'store i64 3, ptr '+re.escape(detail[1])+r', align 1',blocks[branch[2]])
   target.append(dict(block=name,target='bb_67010',retained_error=branch[2],instructions=text))
 assert len(target)==1
 assert 'memory.valid13.entry.prefix' not in blocks
 assert 'memory.valid13' in blocks and 'switch i64' in blocks['memory.valid13']
 return dict(guard_block='guarded.next50',guard_real_LOAD=load[0],table_address=70032,expected_target=67010,
             success_entry=edge[1],private_blocks=sorted(reachable),target=target[0],original_switch_retained=True)

def instructions(block):
    # Preserve actual instruction text; ignore blank/comment lines only.
    return [s.strip() for s in block.splitlines() if s.strip() and not s.lstrip().startswith(';')]

def bridge_proof(before,after,symbol,expected_count):
    old=cfg(function_span(before,symbol)[0]);new=cfg(function_span(after,symbol)[0])
    records=[];substitutions={}
    for name,body in new.items():
        for line in instructions(body):
            if not re.match(r'%entry\.prefix\.live[\w.]* = phi ',line):continue
            phi=re.match(r'(%entry\.prefix\.live[\w.]*) = phi (.+?) \[ (%[\w.]+), %([\w.]+) \], \[ (%[\w.]+), %([\w.]+) \]$',line)
            assert phi,line
            result,kind,original,origin,fresh,copied=phi.groups()
            assert copied==origin+'.entry.prefix' and name in old and origin in old and copied in new
            incoming=[]
            for pred,text in old.items():
                incoming.extend(pred for target in re.findall(r'label %([\w.]+)',text) if target==name)
            assert incoming==[origin],(name,incoming)
            oldins=instructions(old[origin]);cloneins=instructions(new[copied])
            assert len(oldins)==len(cloneins),(origin,len(oldins),len(cloneins))
            index=next(i for i,s in enumerate(oldins) if s.startswith(original+' = '))
            assert cloneins[index].startswith(fresh+' = '),(original,fresh,cloneins[index])
            assert fresh!=original
            substitutions.setdefault(name,{})[original]=result
            uses=sum(len(re.findall(re.escape(original)+r'(?![\w.])',s.split(' = ',1)[-1])) for s in instructions(old[name]))
            assert uses>0
            records.append(dict(exit=name,origin=origin,original=original,fresh=fresh,phi=result,type=kind,uses=uses,original_definition=oldins[index],fresh_definition=cloneins[index],phi_instruction=line))
    assert len(records)==expected_count,(len(records),expected_count)
    for name,mapping in substitutions.items():
        expected=[]
        for line in instructions(old[name]):
            expected.append(re.sub(r'%[\w.]+',lambda m:mapping.get(m[0],m[0]),line))
        actual=[s for s in instructions(new[name]) if not re.match(r'%entry\.prefix\.live[\w.]* = phi ',s)]
        assert actual==expected,(name,'original exit body differs beyond proved replacements')
    return records

def controls_for_bridge(before,after,symbol,count):
    records=bridge_proof(before,after,symbol,count);r=records[0]
    wrong=[after.replace(r['phi_instruction'],r['phi_instruction'].replace('[ '+r['fresh']+',','[ '+r['original']+',')),
           after.replace(r['phi_instruction'],r['phi_instruction'].replace('%'+r['origin']+' ]','%'+r['origin']+'.entry.prefix ]')),
           after.replace(r['phi_instruction'],r['phi_instruction'].replace(' = phi ',' = freeze '))]
    bodies=cfg(function_span(after,symbol)[0]);exitbody=bodies[r['exit']]
    use=next(s for s in instructions(exitbody) if r['phi'] in s and ' = phi ' not in s)
    wrong.append(after.replace(exitbody,exitbody.replace(use,use.replace(r['phi'],r['original'])),1))
    wrong.append(after.replace(exitbody,exitbody.replace(use,'; deliberately dropped use',1),1))
    for value in wrong:
        assert value!=after
        try:bridge_proof(before,value,symbol,count)
        except (AssertionError,KeyError,StopIteration):continue
        raise AssertionError('bridge fault control accepted')
    return len(wrong)

def audit(root,recipe):
    activation=json.loads((root/'entry-prefix-activation.json').read_text());guards=json.loads((root/'guarded-entry-activation.json').read_text());rows=[]
    assert activation['prefix_options']==({'max_blocks':32,'max_instructions':2048} if recipe=='A' else {'max_blocks':128,'max_instructions':16384})
    for entry in activation['records']:
        g=next(x for x in guards['records'] if x['entry']==entry['entry']);base=root/'guarded-views'/g['profile']/g['level'];prefix=root/'entry-prefix-views'/g['profile']/g['level']
        before=(base/'before-raw.ll').read_text();after=(prefix/'after-raw.ll').read_text();bound=(prefix/'after-bound.ll').read_text();symbol=g['manifest']['view_symbol']
        expected,_=bind_wrapper(after,g['manifest']['root_symbol'],symbol,entry['entry']);assert expected==bound
        assert ordinary_functions(before,symbol)==ordinary_functions(after,symbol)
        outcome=validate_prefix_report(entry['report'],before,after)
        assert entry['actual_prefix_runs']==1 and entry['actual_total_pass_runs']==25 and entry['actual_legacy_iterations']==3
        counters=entry['report']['runs'][0]['counters'];row=dict(entry=entry['entry'],profile=g['profile'],level=g['level'],outcome=outcome,counters=counters)
        if recipe=='A' and entry['entry']==0x10090:
            assert counters['changed']==1 and counters['complete']==0 and counters['budget_exhausted']==1 and counters['rejected']==0
            row['bridges']=bridge_proof(before,after,symbol,3);row['bridge_fault_controls']=controls_for_bridge(before,after,symbol,3)
        if recipe=='B' and entry['entry']==0x10770:
            assert counters['changed']==1 and counters['complete']==1 and counters['budget_exhausted']==0 and counters['rejected']==0
            assert counters['cloned_blocks']==110 and counters['cloned_instructions']==7892
            row['bridges']=bridge_proof(before,after,symbol,1);r=row['bridges'][0]
            assert r['origin']=='bb_68024' and r['exit']=='memory.valid69' and r['original']=='%83' and r['uses']==2
            assert r['original_definition']=='%83 = sub i64 %82, 8'
            row['bridge_fault_controls']=controls_for_bridge(before,after,symbol,1)
        rows.append(row)
    raw=(root/'entry-prefix-views/f-cff/O0/after-raw.ll').read_text();rawblock=cfg(function_span(raw,'dragon_static_entry_x86_64_102a0.guarded.real_sp')[0])['memory.valid13.entry.prefix'];assert 'load i64' in rawblock and 'switch i64' in rawblock
    final=(root/'guarded-views/f-cff/O0/after-bound.ll').read_text();proof=prove_first_selector(final)
    controls=[final.replace('label %bb_67010, label %memory.invalid14.entry.prefix','label %bb_66208, label %memory.invalid14.entry.prefix'),final.replace('i64 70032\n','i64 70024\n'),final.replace('label %memory.invalid14.entry.prefix','label %common.ret'),final.replace(proof['target']['instructions'],proof['target']['instructions'].replace('  br i1','  %audit.bad = load i64, ptr %2, align 1\n  br i1'))]
    for value in controls:
        assert value!=final
        try:prove_first_selector(value)
        except (AssertionError,KeyError):continue
        raise AssertionError('first-selector fault control accepted')
    return dict(ok=True,recipe=recipe,precision_gate_met=True,first_selector_proof=proof,rows=rows,first_selector_controls=len(controls),native_executions=0)

def self_test():
    before='''define void @kernel(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  br label %orig
orig:
  %v = load i64, ptr %state, align 1
  br label %tail
tail:
  store i64 %v, ptr %memory, align 1
  ret void
}
'''
    after=before.replace('tail:\n  store i64 %v','tail:\n  %entry.prefix.live = phi i64 [ %v, %orig ], [ %fresh, %orig.entry.prefix ]\n  store i64 %entry.prefix.live').replace('\n}\n','\norig.entry.prefix:\n  %fresh = load i64, ptr %state, align 1\n  br label %tail\n}\n')
    assert bridge_proof(before,after,'kernel',1)[0]['uses']==1
    return dict(positive_bridge_cases=1,rejected_bridge_controls=controls_for_bridge(before,after,'kernel',1),native_executions=0)

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path);p.add_argument('--recipe',choices=('A','B'));p.add_argument('--self-test',action='store_true');a=p.parse_args()
    if a.self_test:report=self_test()
    else:
        assert a.root is not None and a.recipe is not None
        report=audit(a.root,a.recipe);(a.root/'continuation-precision-review.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
