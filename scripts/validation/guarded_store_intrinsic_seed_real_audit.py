#!/usr/bin/env python3
"""Read-only intrinsic-seed parent/child CFG audit; no LLVM/native execution."""
from pathlib import Path
import argparse,collections,hashlib,json,re,traceback,time
IDS=('manual-O0','manual-O2','flatten-switch-O0','flatten-switch-O2')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def parse(p):
 blocks={};inside=False;block=None
 for n,line in enumerate(p.read_text().splitlines(),1):
  if line.startswith('define '):inside='@dragon_static_entry_x86_64_1010(' in line;block=None
  if not inside:continue
  if line=='}':inside=False;continue
  m=re.match(r'^([^ ;\t]+):',line)
  if m:block=m[1];blocks[block]=dict(line=n,lines=[])
  elif block:blocks[block]['lines'].append((n,line))
 for b in blocks.values():
  terms=[i for i,(n,l) in enumerate(b['lines']) if re.match(r'  (?:br |switch |ret |unreachable)',l)]
  assert terms
  b['term_line']=b['lines'][terms[-1]][0];b['term']='\n'.join(l for n,l in b['lines'][terms[-1]:])
  b['successors']=re.findall(r'label %([^ ,\]\n]+)',b['term'])
 return blocks
def cases(term):return [(int(v),target) for v,target in re.findall(r'i64 (-?[0-9]+), label %([^ \]\n]+)',term)]
def scc(graph):
 index={};low={};stack=[];active=set();groups=[]
 def visit(v):
  index[v]=low[v]=len(index);stack.append(v);active.add(v)
  for w in graph[v]:
   if w not in graph:continue
   if w not in index:visit(w);low[v]=min(low[v],low[w])
   elif w in active:low[v]=min(low[v],index[w])
  if low[v]==index[v]:
   group=[]
   while True:
    w=stack.pop();active.remove(w);group.append(w)
    if w==v:break
   groups.append(group)
 for v in graph:
  if v not in index:visit(v)
 return groups
def path(graph,start,end,allowed):
 queue=collections.deque([start]);pred={start:None}
 while queue:
  v=queue.popleft()
  if v==end:
   result=[]
   while v is not None:result.append(v);v=pred[v]
   return list(reversed(result))
  for w in graph.get(v,[]):
   if w in allowed and w not in pred:pred[w]=v;queue.append(w)
 raise AssertionError(('no SCC path',start,end))
def math_calls(blocks):
 return collections.Counter((b,callee) for b,r in blocks.items() for n,l in r['lines']
  for callee in re.findall(r'\bcall\b[^@\n]*@(llvm\.fsh[lr]\.i(?:32|64))\(',l))
def scan(before,after):
 new=set(after)-set(before);contexts=[];old_switches={b:r for b,r in before.items() if r['term'].startswith('  switch')}
 for seed,r in before.items():
  updated=after[seed];assert len(r['successors'])==len(updated['successors'])
  for slot,old in enumerate(r['successors']):
   entry=updated['successors'][slot]
   if entry==old:continue
   assert entry in new
   todo=[entry];seen=set();guards=[]
   while todo:
    b=todo.pop()
    if b in seen or b not in new:continue
    seen.add(b);node=after[b]
    if node['term'].startswith('  switch') and node['successors'][0].startswith('transition.fallback'):
     selector=re.search(r'switch i64 (%[^, ]+)',node['term'])[1];fallback=node['successors'][0]
     load=[(n,l) for n,l in node['lines'] if re.match(r'  '+re.escape(selector)+r' = load i64, ptr ',l)]
     assert len(load)==1 and 'volatile' not in load[0][1] and 'atomic' not in load[0][1]
     fb=after[fallback];assert re.search(r'switch i64 (%[^, ]+)',fb['term'])[1]==selector
     assert not any(re.search(r'= load ',l) for n,l in fb['lines'])
     originals=[(b,r) for b,r in old_switches.items() if cases(r['term'])==cases(fb['term']) and r['successors'][0]==fb['successors'][0]]
     assert len(originals)==1
     targets=cases(node['term']);assert targets and all(x in cases(originals[0][1]['term']) for x in targets)
     guards.append(dict(block=b,line=node['term_line'],actual_condition=selector,
       actual_target_LOAD=dict(line=load[0][0],text=load[0][1]),original_terminal=originals[0][0],
       fast_cases=[dict(value=v,target=t) for v,t in targets],miss=fallback,
       same_observation_fallback=True,fallback_full_case_default_map_exact=True))
    todo.extend(node['successors'])
   assert len(guards)==1
   source_calls=[dict(line=n,text=l) for n,l in after[seed]['lines'] if '@llvm.fsh' in l]
   chain=[b for b in sorted(seen,key=lambda b:after[b]['line']) if b!=guards[0]['miss']]
   contexts.append(dict(seed=seed,seed_line=after[seed]['line'],slot=slot,old_destination=old,new_entry=entry,
        source_math_calls=source_calls,copied_chain=chain,guard=guards[0]))
 assert math_calls(before)==math_calls({b:after[b] for b in before})
 copied_calls=[dict(block=b,line=n,text=l) for b in new for n,l in after[b]['lines'] if re.search(r'\bcall\b',l)]
 assert copied_calls==[]
 return contexts,new
def view(after,contexts,removed):
 graph={b:[x for x in r['successors'] if x not in removed] for b,r in after.items() if b not in removed}
 pruned=[]
 for c in contexts:
  guard=c['guard'];block=guard['block'];assert block in graph
  old=list(graph[block]);miss=guard['miss']
  graph[block]=[x['target'] for x in guard['fast_cases']]
  assert old[0]==miss
  pruned.append(dict(from_block=block,to_block=miss,reason='conditional view: only the actual-target equality success arms'))
 guard_blocks={c['guard']['block'] for c in contexts}
 groups=[sorted(g) for g in scc(graph) if len(g)>1 and set(g)&guard_blocks]
 records=[]
 for group in groups:
  members=set(group)
  outgoing=[dict(source=b,target=x) for b in group for x in graph[b] if x not in members]
  records.append(dict(members=group,member_count=len(group),fast_guards=sorted(members&guard_blocks),
      original_business_seeds=sorted(members&{c['seed'] for c in contexts}),
      retained_outgoing_edges=outgoing,closed_component_without_exits=not outgoing))
 return graph,dict(removed_original_blocks=sorted(removed),removed_guard_miss_edges=pruned,
                   all_other_edges_and_shared_business_blocks_retained=True,business_SCCs=records)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--parent',type=Path,required=True)
 p.add_argument('--child',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 a=p.parse_args();assert not a.out.exists();a.out.mkdir(parents=True)
 report=dict(schema='intrinsic-seed-real-scc-independent-v1',passed=False,failures=[],
             native_or_product_runs=0,checker_sha256=sha(Path(__file__)),cases=[])
 pins={};started=time.monotonic();active={}
 try:
  for case in IDS:
   active=dict(case=case);paths={}
   for label,root in (('parent',a.parent),('child',a.child)):
    paths[label]={}
    for kind,suffix in (('before','-after.ll.parsed-before.ll'),('after','-after.ll'),('stats','-report.json')):
     q=root/(case+suffix);assert q.is_file() and not q.is_symlink();pins[str(q)]=sha(q);paths[label][kind]=q
   assert paths['parent']['before'].read_bytes()==paths['child']['before'].read_bytes()
   before=parse(paths['child']['before']);parent=parse(paths['parent']['after']);child=parse(paths['child']['after'])
   pc,pn=scan(before,parent);cc,cn=scan(before,child)
   ps=json.loads(paths['parent']['stats'].read_bytes());cs=json.loads(paths['child']['stats'].read_bytes())
   assert ps['verified'] is True and cs['verified'] is True
   assert len(pc)==ps['rewritten_edges'] and len(cc)==cs['rewritten_edges']
   assert len(pn)==ps['cloned_blocks'] and len(cn)==cs['cloned_blocks']
   assert cs['loop_contexts']==cs['loop_backedges']==0
   added=[c for c in cc if (c['seed'],c['slot']) not in {(x['seed'],x['slot']) for x in pc}]
   row=dict(case=case,before_bytes_identical=True,
       after_bytes_identical=paths['parent']['after'].read_bytes()==paths['child']['after'].read_bytes(),
       parent_statistics=ps,child_statistics=cs,parent_contexts=pc,child_contexts=cc,
       new_incoming_contexts=added,new_cloned_math_calls=0,
       original_math_call_multiset_unchanged=True)
   if case=='manual-O2':
    assert {c['seed'] for c in added}=={'memory.valid103','memory.valid113'}
    assert parent['memory.valid125']['successors']==child['memory.valid125']['successors']==['bb_4752']
    row['memory_valid125']=dict(rewritten=False,successors=['bb_4752'],
        source_math_calls=[dict(line=n,text=l) for n,l in child['memory.valid125']['lines'] if '@llvm.fsh' in l],
        cause='not established by aggregate counters; no additional pass execution or budget change')
    sets=(('minimal-switch-only',{'memory.valid109'}),
          ('dispatch-prefix',{'bb_4752','bb_4765','memory.valid107','bb_4787','memory.valid109'}))
    views={}
    for label,removed in sets:
     pg,pv=view(parent,pc,removed);cg,cv=view(child,cc,removed)
     assert pv['business_SCCs']==[]
     assert len(cv['business_SCCs'])==1 and cv['business_SCCs'][0]['member_count']==48
     assert not cv['business_SCCs'][0]['closed_component_without_exits']
     members=set(cv['business_SCCs'][0]['members'])
     witness=path(cg,'memory.valid103','memory.valid113',members)
     witness+=path(cg,'memory.valid113','memory.valid103',members)[1:]
     cv['cycle_witness_via_both_new_seeds']=witness
     views[label]=dict(parent=pv,child=cv)
    assert views['minimal-switch-only']['child']['business_SCCs'][0]['members']==views['dispatch-prefix']['child']['business_SCCs'][0]['members']
    row['conditional_views']=views
   else:assert row['after_bytes_identical'] is True and added==[]
   report['cases'].append(row)
  for name,h in pins.items():assert sha(Path(name))==h
  report.update(passed=True,input_sha256=pins,
      conclusion='manualO2 now has one 48-block conditional inner-business SCC despite loopContexts0; memory.valid125 outer-round continuation still enters original dispatcher. No full dispatcher removal or feasibility/native claim.',
      boundary='Conditional topology deletes only explicitly listed original central blocks and new equality-guard miss edges. Bounds, final-output side exits and all business/shared blocks are retained; the SCC has legitimate exits and is not an exitless/infinite loop.')
 except BaseException as e:
  report['failures'].append(dict(case=active,type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()));raise
 finally:
  report['seconds']=time.monotonic()-started;(a.out/'REPORT.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
 print('manualO2 parent0 -> child1 SCC,48members; new seeds103/113;125unmodified;0copied calls')
 print('REPORT '+sha(a.out/'REPORT.json'))
if __name__=='__main__':main()
