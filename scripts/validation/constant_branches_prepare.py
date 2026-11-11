#!/usr/bin/env python3
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);p.add_argument('--baseline',action='store_true');a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False)
env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def literals(s):return len(re.findall(r'^  br i1 (?:true|false),',s,re.M))+len(re.findall(r'^  switch i\d+ -?\d+,',s,re.M))
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'frozen').glob('*')),*sorted((r/'input').glob('*')),*sorted(a.runtime.glob('*.so'))] if p.is_file()};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
def invoke(source,output,recipe):
 output.parent.mkdir(exist_ok=True);report=output.with_suffix('.json');run=subprocess.run([str(r/'driver'),str(source),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 output.with_suffix('.log').write_bytes(run.stdout);output.with_suffix('.exit').write_text(str(run.returncode)+'\n');data=json.loads(report.read_text()) if report.exists() else None
 row={'input':str(source),'output':str(output),'recipe':recipe,'exit':run.returncode,'report':data};rows.append(row);return row
for recipe in ('pre','post'):
 for source in sorted((r/'input').glob('*.ll')):invoke(source,out/recipe/source.name,recipe)
for recipe in ('fold','zero'):invoke(r/'shape-before/normal.ll',out/'shape'/(recipe+'.ll'),recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');counts={}
for row in rows:
 data=row['report'];assert data and data['verified_output'] and data['identity_preserved'],row
 path=Path(row['output']);recipe=row['recipe']
 if a.baseline and recipe!='pre':
  assert row['exit']==3 and not data['option_accepted'] and data['unchanged'] and not path.exists(),row
  assert 'constant-branches' in data['option_error'];continue
 assert row['exit']==0 and data['option_accepted'] and data['pipeline']['ok'],row
 if recipe=='pre':counts[path.name]=literals(path.read_text());continue
 last=data['pipeline']['runs'][-1];assert last['name']=='constant-branches';counters=last['counters']
 if recipe=='zero':assert data['unchanged'] and last['status']==2 and counters['budget_exhausted']>0
 elif recipe=='fold':
  s=path.read_text();assert counters['folded_switches']==1 and counters['folded_branches']==1 and counters['removed_blocks']==1
  assert 'constant.dead:' not in s and s.count('!dragon.guarded-transitions.generated')==2
  assert 'switch i64 %loaded, label %miss [' in s and 'br i1 %again, label %seed, label %finish' in s
 else:
  count=counts[path.name];assert counters['candidate_terminators']==count and counters['folded_switches']+counters['folded_branches']==count,row
  assert counters['remaining_constants']==literals(path.read_text()),row
assert len(counts)==6 and sum(counts.values())>0,counts
for recipe in ('pre','post'):
 if a.baseline and recipe=='post':continue
 shutil.copyfile(r/'frozen/helper.c',out/recipe/'helper.c')
 for p in sorted((r/'frozen').glob('control_*.ll')):shutil.copyfile(p,out/recipe/p.name)
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'literal-candidates.json').write_text(json.dumps(counts,indent=2)+'\n');print(json.dumps({'label':a.label,'all_gates':True,'literal_candidates':counts}))
