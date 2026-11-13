#!/usr/bin/env python3
"""Exercise untrusted state/target guards via the actual public builtin."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
from guarded_state_values_guard_audit import audit as audit_guard
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*a.runtime.glob('*.so')] if p.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
rows=[]
def invoke(mode,recipe):
 dest=out/recipe;dest.mkdir(exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json')
 args=[str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe]
 p=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.command.json')).write_text(json.dumps(args)+'\n');(dest/(mode+'.log')).write_bytes(p.stdout);(dest/(mode+'.exit')).write_text(str(p.returncode)+'\n')
 rows.append({'mode':mode,'recipe':recipe,'exit':p.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
for mode in ('plain','table_before','table_after','freeze','flags','duplicate','tail_call'):invoke(mode,'normal')
for recipe in ('wrongvalue','wrongtarget','resumed','bad','budget','zero'):invoke('plain',recipe)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
def block(s,name):
 match=re.search(r'^'+re.escape(name)+r':[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S)
 assert match,name
 return match.group(1)
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['option_accepted'] and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'],row
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in d['rounds']]
 expected=row['recipe'] in ('normal','wrongvalue','wrongtarget','resumed') and row['mode']!='tail_call'
 assert sum(counts)==int(expected) and d['unchanged']!=expected,row
 output=out/row['recipe']/(row['mode']+'.ll');s=output.read_text();before=Path(str(output)+'.parsed-before.ll')
 if not expected:assert output.read_bytes()==before.read_bytes(),row;continue
 c=d['rounds'][0]['runs'][0]['counters'];assert c['complete']==0 and c['budget_exhausted']==0 and d['rounds'][0]['runs'][0]['status']==2,row
 if row['recipe']=='resumed':assert counts==[1,0,0] and output.read_bytes()==(out/'normal/plain.ll').read_bytes(),row
 seed=block(s,'seed');value=51 if row['recipe']=='wrongvalue' else 2
 gate,fallback=audit_guard(before.read_text(),s,value);assert 'call void @seed_unknown' in seed and seed.count('store i64 %state, ptr %seedp')==1,row
 old=block(s,fallback);assert 'br i1 %cmp' in old,row
 assert '%cmp = icmp ugt i64 %state, 50' in seed and s.count('call void @seed_unknown')==1,row
 fast=re.findall(r'br i1 (%[-\w.]+), label %([-\w.]+), label %(transition\.fallback[-\w.]*)',s)
 assert len(fast)==1,row
 fallbackbody=block(s,fast[0][2]);sw=re.search(r'switch i64 (%loaded\.transition[-\w.]*), label %good \[(.*?)\]',fallbackbody,re.S);assert sw,row
 selector,cases=sw.groups();candidate=777 if row['recipe']=='wrongtarget' else 43
 assert re.search(r'icmp eq i64 '+re.escape(selector)+r', '+str(candidate)+r'\b',s),row
 assert {int(v) for v in re.findall(r'i64 (\d+), label',cases)}=={43,44,99,127,128,255,777},row
 if row['mode']=='duplicate':
  small=block(s,'small');phi=re.search(r'%small_state = phi i64 (.*)',small);assert phi and phi.group(1).count('%'+fallback+' ]')==2,row
 if row['mode']=='freeze':assert '%state = freeze i64 %raw' in s,row
 if row['mode']=='flags':assert '%state = add nuw nsw i64 %raw, 0' in s,row
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'committed_always_incomplete':True}))
