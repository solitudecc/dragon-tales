#!/usr/bin/env python3
"""Public API/stats/IR/refusal gates for the frozen bit-domain driver.

LLVM verification and native complete-state checks remain separate. This script
checks actual PHI slot multiplicity and same-read full fallback shape; it does
not claim a general semantic proof from printed instruction names.
"""
import argparse,hashlib,json,os,re,subprocess
from collections import Counter
from pathlib import Path
import guarded_bit_slices_oracle as model

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def blocks(text):
 body=text[text.index('\nentry:')+1:text.index('\n}',text.index('define i64 @oracle'))]
 labels=list(re.finditer(r'^([-a-zA-Z$._0-9]+):[^\n]*\n',body,re.M))
 return {m[1]:body[m.end():labels[j+1].start() if j+1<len(labels) else len(body)] for j,m in enumerate(labels)}
def switch(body):
 m=re.search(r'switch i64 (%[-a-zA-Z$._0-9]+), label %([-a-zA-Z$._0-9]+) \[(.*?)\]',body,re.S)
 assert m;cases=[(int(k),target) for k,target in re.findall(r'i64 (-?[0-9]+), label %([-a-zA-Z$._0-9]+)',m[3])]
 assert len(cases)==len({k for k,_ in cases});return m[1],m[2],cases

def shape(before,after):
 source,actual=blocks(before),blocks(after);assert list(actual)[:len(source)]==list(source)
 incoming={n:Counter() for n in actual}
 for name,body in actual.items():
  edges=[switch(body)[1]]+[x[1] for x in switch(body)[2]] if 'switch i64 ' in body else re.findall(r'\blabel %([-a-zA-Z$._0-9]+)',body.strip().splitlines()[-1])
  for target in edges:assert target in actual;incoming[target][name]+=1
 phis=0
 for name,body in actual.items():
  for ty,arms in re.findall(r'^\s*%[-a-zA-Z$._0-9]+ = phi (i[0-9]+|ptr) (.*)$',body,re.M):
   phis+=1;assert Counter(re.findall(r'\[ [^,]+, %([-a-zA-Z$._0-9]+) \]',arms))==incoming[name],('PHI does not cover actual edge slots',name)
 assert not re.search(r'\b(?:undef|poison)\b',after)
 for callee in ('prefix_unknown','side_unknown'):
  assert after.count('call void @'+callee+'(')==before.count('call void @'+callee+'('),'CALL copied/omitted'
 original=switch(source['dispatch']);products=[];contexts=backedges=0
 for name,body in actual.items():
  if name in source or 'switch i64 ' not in body:continue
  selector,fallback,cases=switch(body)
  if fallback not in actual or 'switch i64 ' not in actual[fallback]:continue
  fsel,fdefault,fcases=switch(actual[fallback]);assert selector==fsel
  assert [k for k,_ in fcases]==[k for k,_ in original[2]] and fdefault==original[1]
  assert re.search(re.escape(selector)+r' = load i64, ptr ',body),'fast switch does not consume its same-block actual load'
  loops=[(k,t) for k,t in cases if t not in source]
  if loops:assert len({t for _,t in loops})==1;contexts+=1;backedges+=len(loops)
  products.append(dict(block=name,selector=selector,fallback=fallback,fast_cases=cases,loop_slots=len(loops)))
 return dict(products=products,actual_loop_contexts=contexts,actual_loop_backedges=backedges,phi_values=phis,blocks=len(actual))

def main():
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
 out=a.root/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
 files=[a.root/'driver',*sorted((a.root/'source').glob('*')),*sorted((a.root/'before').glob('*')),*sorted(a.runtime.glob('*.so'))]
 pins={str(p):sha(p) for p in files if p.is_file()};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n')
 (out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(a.root/'driver')],text=True,env=env))
 recipes=[(m,'normal') for m in model.MODES]
 recipes += [('l64t1',r) for r in ('wrong','straight','resumed','bad','budget','zero','old','oldfinite','oldstore')]
 recipes += [('refuse_'+bad,'refusal') for bad in ('direct_undef','direct_poison','width','above')]
 rows=[]
 for mode,recipe in recipes:
  dest=out/recipe;dest.mkdir(exist_ok=True);output,report=dest/(mode+'.ll'),dest/(mode+'.json')
  command=[str(a.root/'driver'),str(a.root/'before'/(mode+'.ll')),str(output),str(report),'normal' if recipe=='refusal' else recipe]
  (dest/(mode+'.command.json')).write_text(json.dumps(command,indent=2)+'\n')
  proc=subprocess.run(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
  (dest/(mode+'.log')).write_bytes(proc.stdout);(dest/(mode+'.exit')).write_text(str(proc.returncode)+'\n')
  rec=json.loads(report.read_text()) if report.exists() else None;row=dict(mode=mode,recipe=recipe,exit=proc.returncode,report=rec);rows.append(row)
  (out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
  assert proc.returncode==0 and rec and all(rec[k] for k in ('option_accepted','verified_output','identity_preserved','metadata_preserved')),row
  counters=[r['runs'][0]['counters'] for r in rec['rounds']];assert all(r['ok'] for r in rec['rounds']);rewrites=sum(c.get('rewritten_edges',0) for c in counters)
  negative=mode=='call_inside' or recipe in ('bad','budget','zero','old','oldfinite','oldstore','refusal')
  if negative:
   assert not rewrites and rec['unchanged'],row
   assert output.read_bytes()==Path(str(output)+'.parsed-before.ll').read_bytes()
  else:
   assert rewrites>0 and not rec['unchanged'],row
   row['shape']=shape(Path(str(output)+'.parsed-before.ll').read_text(),output.read_text())
   assert row['shape']['products'];assert row['shape']['actual_loop_contexts']==sum(c.get('loop_contexts',0) for c in counters)
   assert row['shape']['actual_loop_backedges']==sum(c.get('loop_backedges',0) for c in counters)
   if recipe in ('wrong','straight'):assert row['shape']['actual_loop_contexts']==0
   else:assert row['shape']['actual_loop_contexts']>0
   assert all(c.get('complete')==0 for c in counters if c.get('rewritten_edges',0))
  (out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
 assert pins=={s:sha(Path(s)) for s in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n')
 print(json.dumps(dict(label=a.label,recipes=len(rows),native_executions=0,pins_stable=True)))
if __name__=='__main__':main()
