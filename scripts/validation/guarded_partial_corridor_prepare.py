#!/usr/bin/env python3
"""Exercise actual public partial builtin and its explicitly incomplete counters."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*a.runtime.glob('*.so')] if p.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
def invoke(mode,recipe):
 dest=out/recipe;dest.mkdir(exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json')
 p=subprocess.run([str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.log')).write_bytes(p.stdout);(dest/(mode+'.exit')).write_text(str(p.returncode)+'\n')
 rows.append({'mode':mode,'recipe':recipe,'exit':p.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
for recipe in ('old','normal'):
 for mode in ('plain','table_before','table_after','call_inside'):invoke(mode,recipe)
for recipe in ('wrong','resumed','bad','budget','zero'):invoke('plain',recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['option_accepted'] and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'],row
 rounds=d['rounds'];assert len(rounds)==(3 if row['recipe']=='resumed' else 1) and all(x['ok'] for x in rounds)
 counters=[x['runs'][0]['counters'] for x in rounds];counts=[x.get('rewritten_edges',0) for x in counters]
 expected=row['recipe'] in ('normal','wrong','resumed') and row['mode']!='call_inside'
 assert sum(counts)==int(expected) and d['unchanged']!=expected,row
 output=out/row['recipe']/(row['mode']+'.ll');before=Path(str(output)+'.parsed-before.ll')
 if not expected:assert output.read_bytes()==before.read_bytes(),row;continue
 c=counters[0];assert c.get('partial_candidate_edges')==c.get('partial_attempted_edges')==c.get('partial_rewritten_edges')==1 and c.get('partial_unresolved_edges')==0 and c.get('complete')==0 and c.get('budget_exhausted')==0,row
 assert rounds[0]['runs'][0]['status']==2,row
 if row['recipe']=='resumed':
  assert counts==[1,0,0] and all(x.get('partial_rewritten_edges',0)==0 for x in counters[1:]),row
  assert output.read_bytes()==(out/'normal/plain.ll').read_bytes(),row
 s=output.read_text();fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S);assert len(fast)==1,row
 selector,fallback,cases=fast[0];candidate=777 if row['recipe']=='wrong' else 43
 assert {int(v) for v in re.findall(r'i64 (\d+), label',cases)}=={candidate},row
 block=re.search(r'^'+re.escape(fallback)+r':[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);assert block,row
 sw=re.search(r'switch i64 '+re.escape(selector)+r', label %good \[(.*?)\]',block.group(1),re.S);assert sw and {int(v) for v in re.findall(r'i64 (\d+), label',sw.group(1))}=={43,44,99,127,128,255,777},row
 assert s.count('call void @business_unknown')==1,row
 assert '%poststate = load i64, ptr %statep' in s and '%state = select i1 %choose, i64 50, i64 %poststate' in s,row
 good=re.search(r'^good:[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);phi=re.search(r'^\s*%selected = phi i64 (.*)$',good.group(1),re.M) if good else None
 assert phi and phi.group(1).count('[')==(6 if candidate==777 else 7),row
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'partial_commits_always_incomplete':True}))
