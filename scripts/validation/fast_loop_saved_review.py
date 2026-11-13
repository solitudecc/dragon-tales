#!/usr/bin/env python3
"""Read-only recheck of frozen observer bytes, real CFG SCCs and native counts."""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();root=a.root;frozen=root/'frozen';run=root/'run';pre=json.loads((root/'PREDECLARED.json').read_text());plan=json.loads((frozen/'PLAN.json').read_text());sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert all(sha(frozen/n)==h for n,h in pre['files'].items());assert all(sha(Path(n))==h for n,h in pre['external_pins'].items())
text=(frozen/'raw.ll').read_text();assert sha(frozen/'raw.ll')==plan['input_ir_sha256'];name=plan['function'];m=re.search(r'^define [^\n]*@'+re.escape(name)+r'\(',text,re.M);assert m;end=text.index('\n}',m.end());body=text[m.end():end];labels=list(re.finditer(r'^([A-Za-z0-9_.$-]+):[^\n]*\n',body,re.M));assert labels
blocks={x.group(1):body[x.end():(labels[i+1].start() if i+1<len(labels) else len(body))] for i,x in enumerate(labels)}
graph={n:re.findall(r'\blabel %([A-Za-z0-9_.$-]+)',s) for n,s in blocks.items()};assert all(c in graph for children in graph.values() for c in children)
inventory=json.loads((frozen/'module.after.json').read_text());expected={b['name']:[e['target'] for e in b['successors']] for b in inventory['blocks']};assert graph==expected,'actual raw CFG differs from frozen inventory'
projected={n:list(v) for n,v in graph.items()};guards=set();lookup={s['id']:s for s in plan['sites']};assert set(lookup)==set(range(len(lookup)))
def switch(s):
 m=re.search(r'switch i64 (%[A-Za-z0-9_.$-]+), label %([A-Za-z0-9_.$-]+) \[(.*?)\]',s,re.S);assert m,s[-500:];cases=re.findall(r'i64 ([0-9]+), label %([A-Za-z0-9_.$-]+)',m.group(3));return m.group(1),m.group(2),{int(v):b for v,b in cases}
def local_load(n,value):assert re.search(re.escape(value)+r' = load i64, ptr ',blocks[n]),(n,value)
for site in plan['sites']:
 n=site['block'];assert n in graph
 if site['label'] in ('target_success','business_success') and site['kind']=='branch':
  b=re.search(r'br i1 (%[A-Za-z0-9_.$-]+), label %([A-Za-z0-9_.$-]+), label %([A-Za-z0-9_.$-]+)',blocks[n]);assert b and site['slot']==0
  compare=re.search(re.escape(b.group(1))+r' = icmp eq i64 (%[A-Za-z0-9_.$-]+), ([0-9]+)',blocks[n]);assert compare
  value=compare.group(1);candidate=int(compare.group(2));local_load(n,value);loaded,_,cases=switch(blocks[b.group(3)]);assert value==loaded and cases.get(candidate)==b.group(2);projected[n]=[b.group(2)];guards.add(n)
 elif site['label'] in ('target_success','business_success') and site['kind']=='membership':
  value,default,cases=switch(blocks[n]);local_load(n,value);assert set(site['values'])==set(cases) and site['member'];fallback,_,full=switch(blocks[default]);assert value==fallback and all(full.get(k)==v for k,v in cases.items());projected[n]=list(cases.values());guards.add(n)
entry=next(s for s in plan['sites'] if s['label']=='entry_admission');assert entry['kind']=='branch' and entry['slot']==0;start=graph[entry['block']][0]
seen={start};todo=[start]
while todo:
 for c in projected[todo.pop()]:
  if c not in seen:seen.add(c);todo.append(c)
assert not set(plan['central_blocks'])&seen
# Independent iterative Kosaraju over the actual projected raw CFG.
finish=[];visited=set()
for start in sorted(seen):
 if start in visited:continue
 visited.add(start);stack=[(start,iter(projected[start]))]
 while stack:
  try:c=next(stack[-1][1])
  except StopIteration:finish.append(stack.pop()[0]);continue
  if c in seen and c not in visited:visited.add(c);stack.append((c,iter(projected[c])))
reverse={n:[] for n in seen}
for n in seen:
 for c in projected[n]:
  if c in seen:reverse[c].append(n)
visited=set();cycles=[]
for start in reversed(finish):
 if start in visited:continue
 group=set();todo=[start];visited.add(start)
 while todo:
  n=todo.pop();group.add(n)
  for c in reverse[n]:
   if c not in visited:visited.add(c);todo.append(c)
 if group&guards and (len(group)>1 or start in projected[start]):cycles.append(group)
assert {frozenset(x) for x in cycles}=={frozenset(x) for x in plan['fast_sccs']}
fast=set().union(*cycles);assert all(s.get('in_fast_scc')==(s['block'] in fast) for s in plan['sites'] if 'in_fast_scc' in s)
observations=json.loads((run/'observations.json').read_text());assert len(observations)==8 and {(x['mode'],x['input']) for x in observations}=={(m,i) for m in ('normal','double-count') for i in range(4)}
rows=[];normals={}
for mode in ('normal','double-count'):
 for i in range(4):
  raw=(run/f'{mode}-{i}.counts').read_bytes();n,=struct.unpack('<Q',raw[:8]);assert n==len(lookup) and len(raw)==8*(n+1);counts=list(struct.unpack('<'+'Q'*n,raw[8:]));assert (run/f'{mode}-{i}.state').read_bytes()==(frozen/f'output-{i}.bin').read_bytes();record=next(x for x in observations if x['mode']==mode and x['input']==i);assert record['counts']==counts
  totals={label:sum(counts[s['id']] for s in plan['sites'] if s['label']==label) for label in ('root_entry','entry_admission','entry_fallback','target_failure','business_failure','central_dispatcher')}
  witnesses=[{'id':s['id'],'block':s['block'],'count':counts[s['id']]} for s in plan['sites'] if s['label'] in ('target_success','business_success') and s['block'] in fast and counts[s['id']]>1]
  if mode=='normal':assert totals=={'root_entry':1,'entry_admission':1,'entry_fallback':0,'target_failure':0,'business_failure':0,'central_dispatcher':0} and witnesses;normals[i]=counts
  else:assert counts==[2*x for x in normals[i]] and counts!=normals[i]
  rows.append({'mode':mode,'input':i,'full_state_bytes':(run/f'{mode}-{i}.state').stat().st_size,'counts':counts,'totals':totals,'recomputed_fast_scc_witnesses':witnesses})
emit=json.loads((run/'omit-central.json').read_text());central=next(s['id'] for s in plan['sites'] if s['label']=='central_dispatcher');assert emit['omitted_sites']==[central] and [s['id'] for s in emit['emitted_sites']]==[i for i in lookup if i!=central]
# Check that all intrinsic declarations and effect attributes retain canonical bytes.
canonical=root/'review-canonical-before.ll';assert not canonical.exists();cmd=['opt-21','-passes=verify','-S',str(frozen/'raw.ll'),'-o',str(canonical)];r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180);assert r.returncode==0
relevant=lambda text:[x.strip() for x in text.splitlines() if (x.startswith('declare ') and '@llvm.' in x) or x.startswith('attributes #')]
before=relevant(canonical.read_text())
for mode in ('normal','double-count','omit-central'):assert relevant((run/(mode+'.ll')).read_text())==before
summary={'scope':'Actual frozen L2 cffO2 IR; inherited runner historical cffO0 label is not evidence of input identity','function':plan['function'],'input_ir_sha256':plan['input_ir_sha256'],'actual_cfg_blocks':len(graph),'recomputed_fast_scc_sizes':sorted(len(x) for x in cycles),'normal_full_state_matches':4,'double_count_full_state_matches':4,'double_count_controls_detected':4,'omit_central_emission_control_detected':1,'all_four_inputs_have_repeated_actual_fast_scc_guard':True,'intrinsic_declarations_and_attributes_preserved':True,'source_and_external_pins_stable':True,'rows':rows};dest=root/'INDEPENDENT-REVIEW.json';assert not dest.exists();dest.write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='rows'}))
