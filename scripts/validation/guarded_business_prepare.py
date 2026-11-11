#!/usr/bin/env python3
"""Run the stable public pipeline driver on a frozen runtime; never silently retry a different recipe."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);p.add_argument('--baseline',action='store_true');a=p.parse_args()
r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted(a.runtime.glob('*.so'))] if p.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
for recipe in ('default','enabled','zero'):
 dest=out/recipe;dest.mkdir()
 for mode in ('plain','unrelated','state_after','table_before','table_after','call_path'):
  report=dest/(mode+'.json');output=dest/(mode+'.ll')
  cmd=[str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe]
  run=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n')
  data=json.loads(report.read_text()) if report.exists() else None;rows.append({'recipe':recipe,'mode':mode,'exit':run.returncode,'report':data})
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:
 data=row['report'];assert data and data['verified_output'] and data['identity_preserved'],row
 output=out/row['recipe']/(row['mode']+'.ll');parsed=output.with_suffix('.ll.parsed-before.ll')
 if a.baseline and row['recipe']!='default':
  assert row['exit']==3 and not data['option_accepted'] and data['unchanged'] and not output.exists(),row
  assert 'business_corridors' in data['option_error'],row
  continue
 assert row['exit']==0 and data['option_accepted'] and data['pipeline']['ok'],row
 if row['recipe']=='enabled' and row['mode']!='call_path':
  assert data['pipeline']['changed'] and not data['unchanged'],row
  counters=data['pipeline']['runs'][0]['counters'];assert counters['rewritten_edges']==1,row
  s=output.read_text();expected=('head','state_check','copy','mutate','state_load','index','bounds','dispatch')
  assert all(re.search(r'^'+b+r'\.transition:',s,re.M) for b in expected),(row,s)
  assert s.count('transition.matches = icmp eq i64 %loaded.transition,')==1
  hint=99 if row['mode']=='unrelated' else 44
  assert f'icmp eq i64 %loaded.transition, {hint}' in s
  assert 'switch i64 %loaded.transition, label %miss [' in s
  assert '%fresh.transition = freeze i64 %freshbits.transition' in s and '%used_history.transition = add i64 %transition.old, 0' in s
 else:assert data['unchanged'] and not data['pipeline']['changed'] and output.read_bytes()==parsed.read_bytes(),row
if not a.baseline:
 report=out/'budget.json';output=out/'budget.ll';run=subprocess.run([str(r/'driver'),str(r/'before/plain.ll'),str(output),str(report),'budget'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (out/'budget.log').write_bytes(run.stdout);(out/'budget.exit').write_text(str(run.returncode)+'\n');data=json.loads(report.read_text())
 assert run.returncode==0 and data['unchanged'] and data['verified_output'] and data['identity_preserved']
 result=data['pipeline']['runs'][0];assert result['status']==2 and result['counters']['budget_exhausted']>0
assert pins=={path:sha(Path(path)) for path in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'baseline':a.baseline,'rows':len(rows),'all_gates':True}))
