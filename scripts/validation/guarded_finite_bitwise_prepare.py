#!/usr/bin/env python3
"""Actual existing builtin precision gates for the finite-bitwise extension."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins={str(path):sha(path) for path in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*sorted(a.runtime.glob('*.so'))] if path.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
def invoke(mode,recipe):
 dest=out/recipe;dest.mkdir(exist_ok=True);report=dest/(mode+'.json');output=dest/(mode+'.ll')
 run=subprocess.run([str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n')
 rows.append({'recipe':recipe,'mode':mode,'exit':run.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
for mode in ('mask1','combo_phi','combo_select','xor_mask','table_before','table_after','sparse','dense'):invoke(mode,'normal')
for recipe in ('bad','budget','zero'):invoke('mask1',recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
expected_sets={'mask1':{99,127},'combo_phi':{43,127},'combo_select':{43,127},'xor_mask':{99,127,128,255},'table_before':{99,127},'table_after':{99,127},'sparse':{43,44}}
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['option_accepted'],row
 rounds=d['rounds'];assert len(rounds)==1 and rounds[0]['ok'],row
 count=rounds[0]['runs'][0]['counters'].get('rewritten_edges',0)
 expected=row['recipe']=='normal' and row['mode']!='dense'
 output=out/row['recipe']/(row['mode']+'.ll');parsed=Path(str(output)+'.parsed-before.ll')
 assert bool(count)==expected and d['unchanged']!=expected,row
 if not expected:assert output.read_bytes()==parsed.read_bytes(),row;continue
 expected_count=3 if row['mode']=='combo_phi' else 1;assert count==expected_count,row
 s=output.read_text();fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S)
 assert len(fast)==expected_count,(row,len(fast));observed=set()
 for value,fallback,cases in fast:
  assert re.search(r'^'+re.escape(fallback)+r':.*?switch i64 '+re.escape(value)+r', label %good \[',s,re.M|re.S),row
  observed.update(int(x) for x in re.findall(r'i64 (\d+), label',cases))
 assert observed==expected_sets[row['mode']],(row,observed)
 if row['mode']=='combo_phi':
  unknown=re.search(r'^unknown:.*?\n\s*br label %(\S+)',s,re.M);assert unknown and unknown.group(1)=='head',row
 if row['mode'] in ('mask1','table_before','table_after'):assert 'or disjoint i32' in s and 'zext nneg i32' in s,row
 if row['mode'] in ('combo_phi','combo_select'):assert 'or disjoint i64' in s,row
assert pins=={path:sha(Path(path)) for path in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'all_gates':True}))
