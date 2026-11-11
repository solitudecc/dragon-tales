#!/usr/bin/env python3
"""Freeze actual public F API outputs and verify selected-slot continuation."""
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
 data=json.loads(report.read_text()) if report.exists() else None
 rows.append({'recipe':recipe,'mode':mode,'exit':run.returncode,'report':data})
for recipe in ('normal','resumed'):
 for mode in ('plain','select_cast','table_before','table_after','call_path','flagged'):invoke(mode,recipe)
for recipe in ('bad','budget','zero'):invoke('plain',recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['option_accepted'],row
 rounds=d['rounds'];assert len(rounds)==(5 if row['recipe']=='resumed' else 1) and all(x['ok'] for x in rounds),row
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in rounds]
 expected=row['recipe'] in ('normal','resumed') and row['mode']!='call_path'
 output=out/row['recipe']/(row['mode']+'.ll');parsed=Path(str(output)+'.parsed-before.ll')
 assert bool(sum(counts))==expected and d['unchanged']!=expected,row
 if not expected:
  assert output.read_bytes()==parsed.read_bytes(),row
  continue
 count=1 if row['mode']=='select_cast' else 3
 assert sum(counts)==count,(row,counts)
 if row['recipe']=='resumed':
  assert counts==([1,0,0,0,0] if count==1 else [1,1,1,0,0]),(row,counts)
  assert Path(str(output)+'.round3.ll').read_bytes()==Path(str(output)+'.round4.ll').read_bytes(),row
 s=output.read_text()
 # Every small selector reads once in its own block; fallback reuses that SSA.
 fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S)
 assert len(fast)==count,(row,len(fast))
 for value,fallback,cases in fast:
  assert re.search(r'^'+re.escape(fallback)+r':.*?switch i64 '+re.escape(value)+r', label %good \[',s,re.M|re.S),row
  predicted=[int(x) for x in re.findall(r'i64 (\d+), label',cases)]
  assert predicted==[43,127] if row['mode']=='select_cast' else predicted in ([44],[99]),(row,predicted)
 if row['mode']!='select_cast':
  unknown=re.search(r'^unknown:.*?\n\s*br label %(\S+)',s,re.M);assert unknown and unknown.group(1)=='head',row
  assert re.search(r'^left:.*?br i1 %odd, label %head\.transition\w*, label %head\.transition\w*',s,re.M|re.S),row
  assert re.search(r'^right:.*?br label %head\.transition',s,re.M|re.S),row
 if row['mode']=='flagged':assert s.count('add nuw nsw i64')>=2,row
assert pins=={path:sha(Path(path)) for path in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'all_gates':True}))
