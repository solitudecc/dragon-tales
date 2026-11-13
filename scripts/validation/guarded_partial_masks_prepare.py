#!/usr/bin/env python3
"""Strict mask-prediction API, identity, incomplete and full-fallback gates."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*a.runtime.glob('*.so')] if p.is_file()};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True));rows=[]
def invoke(mode,recipe):
 dest=out/recipe;dest.mkdir(exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json');cmd=[str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),recipe];sub=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180);(dest/(mode+'.command.json')).write_text(json.dumps(cmd)+'\n');(dest/(mode+'.log')).write_bytes(sub.stdout);(dest/(mode+'.exit')).write_text(str(sub.returncode)+'\n');rows.append({'mode':mode,'recipe':recipe,'exit':sub.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
for mode in ('plain','history','table_before','table_after','freeze_bool','call_inside'):invoke(mode,'normal')
for recipe in ('wrong','resumed','bad','budget','zero','old','oldfinite','oldstore'):invoke('plain',recipe)
for fatal in ('undef','poison','shift'):invoke('fatal_'+fatal,'normal')
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
def block(text,name):
 m=re.search(r'^'+re.escape(name)+r':[^\n]*\n(.*?)(?=^\S|\Z)',text,re.M|re.S);assert m,name;return m.group(1)
for row in rows:
 d=row['report'];assert row['exit']==0 and d and d['option_accepted'] and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'],row;rounds=d['rounds'];assert len(rounds)==(3 if row['recipe']=='resumed' else 1) and all(x['ok'] for x in rounds);counters=[x['runs'][0]['counters'] for x in rounds];counts=[x.get('rewritten_edges',0) for x in counters];expected=row['recipe'] in ('normal','wrong','resumed') and row['mode']!='call_inside' and not row['mode'].startswith('fatal_');assert sum(counts)==int(expected) and d['unchanged']!=expected,row
 output=out/row['recipe']/(row['mode']+'.ll');before=Path(str(output)+'.parsed-before.ll')
 if not expected:assert output.read_bytes()==before.read_bytes(),row;continue
 c=counters[0];assert c.get('partial_candidate_edges')==c.get('partial_attempted_edges')==c.get('partial_rewritten_edges')==1 and c.get('partial_unresolved_edges')==0 and c.get('complete')==0 and c.get('budget_exhausted')==0 and rounds[0]['runs'][0]['status']==2,row
 if row['recipe']=='resumed':assert counts==[1,0,0] and output.read_bytes()==(out/'normal/plain.ll').read_bytes(),row
 text=output.read_text();fast=re.findall(r'switch i64 (%loaded\.transition[-\w.]*), label %(transition\.fallback[-\w.]*) \[(.*?)\]',text,re.S);assert len(fast)==1,row;selector,fallback,cases=fast[0];values={int(v) for v in re.findall(r'i64 (\d+), label',cases)};assert values==({777} if row['recipe']=='wrong' else {43,44}),row
 old=block(text,fallback);sw=re.search(r'switch i64 '+re.escape(selector)+r', label %good \[(.*?)\]',old,re.S);assert sw and {int(v) for v in re.findall(r'i64 (\d+), label',sw.group(1))}=={43,44,99,127,128,255,777},row
 assert text.count('call void @business_unknown')==1,row
 assert '%pre_unknown = load i64, ptr %statep' in text and '%post_unknown = load i64, ptr %statep' in text and '%before_bool = icmp ne i64 %prebit, 0' in text,row
 assert '%partial = and i64 %post_unknown, %mask' in text and '%state = or i64 %partial, %other' in text and '%side.flagged = add nuw nsw i64 %side, 0' in text,row
 choose='%historical_choice' if row['mode']=='history' else '%fresh';assert '%other = select i1 '+choose+', i64 0, i64 40' in text,row
 if row['mode']=='history':assert '%historical_choice = xor i1 %previous_fresh, %before_bool' in text,row
 if row['mode']=='freeze_bool':assert '%fresh = freeze i1 %fresh.raw' in text,row
 good=block(text,'good');phi=re.search(r'^\s*%selected = phi i64 (.*)$',good,re.M);assert phi and phi.group(1).count('[')==(6 if row['recipe']=='wrong' else 8),row
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':len(rows),'partial_commits_always_incomplete':True}))
