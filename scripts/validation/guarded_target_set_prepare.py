#!/usr/bin/env python3
"""Actual public target-set API, identity checks, and independent structural gates."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins={str(path):sha(path) for path in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*sorted(a.runtime.glob('*.so'))] if path.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
def invoke(mode,recipe,dest):
 dest.mkdir(exist_ok=True);report=dest/(mode+'.json');output=dest/(mode+'.ll')
 run=subprocess.run([str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n')
 data=json.loads(report.read_text()) if report.exists() else None
 row={'recipe':recipe,'mode':mode,'exit':run.returncode,'report':data};rows.append(row)
 return row,output
for recipe in ('normal','wrong'):
 for mode in ('plain','table_before','table_after','call_before','call_inside'):invoke(mode,recipe,out/recipe)
for recipe in ('none','bad','noncase','overset','budget'):invoke('plain',recipe,out/recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['option_accepted'] and d['pipeline']['ok'],row
 output=out/row['recipe']/(row['mode']+'.ll');parsed=Path(str(output)+'.parsed-before.ll')
 expected=row['recipe'] in ('normal','wrong') and row['mode']!='call_inside'
 assert d['pipeline']['changed']==expected and d['unchanged']!=expected,row
 if not expected:
  assert output.read_bytes()==parsed.read_bytes(),row
  continue
 counters=d['pipeline']['runs'][0]['counters'];assert counters['rewritten_edges']==1,row
 s=output.read_text();assert re.search(r'^dispatch\.transition:',s,re.M)
 small=re.search(r'switch i64 %loaded\.transition, label %transition\.fallback \[(.*?)\]',s,re.S);assert small,row
 actual=[int(v) for v in re.findall(r'i64 (\d+), label',small.group(1))]
 assert actual==([43,44,99,127,128,255] if row['recipe']=='normal' else [777]),(row,actual)
 full=re.search(r'^transition\.fallback:.*?switch i64 %loaded\.transition, label %good \[(.*?)\]',s,re.S|re.M);assert full,row
 assert [int(v) for v in re.findall(r'i64 (\d+), label',full.group(1))]==[43,44,99,127,128,255,777],row
 good=re.search(r'^good:.*?\n\s*%selected = phi i64 (.*)',s,re.M);assert good,row
 assert good.group(1).count('[')==(8 if row['recipe']=='normal' else 6),(row,good.group(1))
 assert s.count('%loaded.transition = load i64,')==1,row
 assert '!dragon.guarded-entry' in s and '!oracle.untouched' in s,row
assert pins=={path:sha(Path(path)) for path in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'all_gates':True}))
