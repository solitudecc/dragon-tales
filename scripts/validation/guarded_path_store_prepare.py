#!/usr/bin/env python3
"""Actual STORE builtin, old SSA compatibility, partial edges, and full fallback."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins={str(path):sha(path) for path in [r/'path-driver',*sorted((r/'source').glob('*')),*sorted((r/'path-before').glob('*')),*sorted(a.runtime.glob('*.so'))] if path.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'path-driver')],env=env,text=True))
rows=[]
def invoke(mode,recipe):
 dest=out/recipe;dest.mkdir(exist_ok=True);report=dest/(mode+'.json');output=dest/(mode+'.ll')
 run=subprocess.run([str(r/'path-driver'),str(r/'path-before'/(mode+'.ll')),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n')
 rows.append({'recipe':recipe,'mode':mode,'exit':run.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
for recipe in ('old','normal'):
 for mode in ('mixed','mixed_table_after'):invoke(mode,recipe)
for recipe in ('partial','resumed','bad','budget','zero'):invoke('mixed',recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['option_accepted'],row
 rounds=d['rounds'];assert len(rounds)==(3 if row['recipe']=='resumed' else 1) and all(x['ok'] for x in rounds),row
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in rounds]
 expected=row['recipe'] in ('normal','partial','resumed') and row['mode']!='call_path'
 output=out/row['recipe']/(row['mode']+'.ll');parsed=Path(str(output)+'.parsed-before.ll')
 assert bool(sum(counts))==expected and d['unchanged']!=expected,row
 if not expected:assert output.read_bytes()==parsed.read_bytes(),row;continue
 count=1 if row['recipe']=='partial' else 2;assert sum(counts)==count,(row,counts)
 if row['recipe']=='resumed':
  assert counts==[1,1,0],(row,counts)
  assert Path(str(output)+'.round1.ll').read_bytes()==Path(str(output)+'.round2.ll').read_bytes(),row
  assert output.read_bytes()==(out/'normal/mixed.ll').read_bytes(),row
 s=output.read_text();fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S)
 assert len(fast)==count,(row,len(fast))
 for value,fallback,cases in fast:
  assert re.search(r'^'+re.escape(fallback)+r':.*?switch i64 '+re.escape(value)+r', label %good \[',s,re.M|re.S),row
  assert {int(x) for x in re.findall(r'i64 (\d+), label',cases)}=={127},row
 assert re.search(r'^initial_write:.*?\n\s*br label %head\b',s,re.M|re.S),row
 for name in ('head','state_check','copy','mutate','state_load','index','bounds','dispatch'):assert re.search(r'^'+name+r'\.transition\w*:',s,re.M),(row,name)
 assert 'store i64 50, ptr %aptr' in s,row
 assert '%state_a.transition' in s and '%state_b.transition' in s and 'store i64 %state_a.transition' in s,row
 block=re.search(r'^good:[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);assert block,row
 good=re.search(r'^\s*%selected = phi i64 (.*)$',block.group(1),re.M);assert good,row
 assert good.group(1).count('[')==3+3*count,(row,good.group(1))
assert pins=={path:sha(Path(path)) for path in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'all_gates':True}))
