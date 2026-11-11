#!/usr/bin/env python3
"""Audit actual emitted guards while retaining the frozen whole precision failure."""
import argparse,hashlib,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args();out=a.root/a.label
rows=json.loads((out/'summary.json').read_text());assert len(rows)==13;failures=[];observed=[]
for row in rows:
 recipe,mode=row['recipe'],row['mode'];d=row['report'];assert row['exit']==0 and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['option_accepted']
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in d['rounds']];actual=sum(counts)
 should_change=recipe in ('normal','partial','resumed') and mode not in ('four','unknown_arm')
 expected=(1 if recipe=='partial' else 3 if mode=='history' else 1) if should_change else 0
 if actual!=expected:failures.append({'recipe':recipe,'mode':mode,'expected':expected,'actual':counts})
 path=out/recipe/(mode+'.ll');s=path.read_text();observed.append({'recipe':recipe,'mode':mode,'counts':counts,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
 assert d['unchanged']==(actual==0)
 if actual==0:assert path.read_bytes()==Path(str(path)+'.parsed-before.ll').read_bytes();continue
 assert should_change
 fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S);assert len(fast)==actual
 candidates={43,44,99,127} if mode=='history' else {43,44,99,127,128,255,777} if mode=='three' else {44,127}
 for value,fallback,cases in fast:
  assert {int(v) for v in re.findall(r'i64 (\d+), label',cases)}==candidates
  old=re.search(r'^'+re.escape(fallback)+r':[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);assert old
  sw=re.search(r'switch i64 '+re.escape(value)+r', label %good \[(.*?)\]',old.group(1),re.S);assert sw
  assert {int(v) for v in re.findall(r'i64 (\d+), label',sw.group(1))}=={43,44,99,127,128,255,777}
 if mode=='history':assert re.search(r'^head\.transition\w*:',s,re.M) and re.search(r'%c0\.transition\w* = load i1',s)
 good=re.search(r'^good:[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);assert good
 phi=re.search(r'^\s*%selected = phi i64 (.*)$',good.group(1),re.M);assert phi
 assert phi.group(1).count('[')==3+3*actual+(2 if mode in ('history','three') else 1)*actual
 if recipe=='resumed':
  if counts!=[1,1,1,0]:failures.append({'recipe':'resumed','expected_rounds':[1,1,1,0],'actual':counts})
  if path.read_bytes()!=(out/'normal/history.ll').read_bytes():failures.append({'recipe':'resumed','expected_final_equals_normal':True,'actual':False})
original_exit=int((a.root/(a.label+'-prepare.exit')).read_text());assert bool(failures)==(original_exit!=0)
result={'actual_guard_shape_verified':True,'original_precision_gate':not failures,'original_prepare_exit':original_exit,'precision_failures':failures,'observations':observed,'native_executions_added':0}
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='observations'},indent=2))
