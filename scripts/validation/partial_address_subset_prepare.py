#!/usr/bin/env python3
"""Frozen public API checks; baseline records new precision failure without weakening candidate gate."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);p.add_argument('--baseline',action='store_true');a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'before').glob('*')),*a.runtime.glob('*.so')] if p.is_file()};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
profiles=('missing_first','missing_last','short_row','noncase','all_missing','all_noncase','malicious','full_map','bad_manifest');modes=('plain','table_before','table_after','call_inside');rows=[]
for action in ('J','F','H','zero_candidates','zero_rewrites','zero_blocks','zero_instructions'):
 for profile in profiles if action in ('J','F','H') else ('missing_first','full_map'):
  for mode in modes if action=='J' else ('plain',):
   dest=out/(profile if action=='J' else action+'-'+profile);dest.mkdir(exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json');command=[str(r/'driver'),str(r/'before'/(mode+'.ll')),str(output),str(report),profile,action]
   (dest/(mode+'.command.json')).write_text(json.dumps(command)+'\n')
   with (dest/(mode+'.log')).open('wb') as log:
    try:rc=subprocess.run(command,env=env,stdout=log,stderr=log,timeout=60).returncode
    except subprocess.TimeoutExpired:rc='timeout'
   (dest/(mode+'.exit')).write_text(str(rc)+'\n');rows.append(dict(mode=mode,profile=profile,action=action,exit=rc,report=json.load(open(report)) if report.exists() else None));(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
common=[];precision=[];partial={'missing_first':{99},'missing_last':{43},'short_row':{99},'noncase':{43},'malicious':{777,255}}
for row in rows:
 try:
  d=row['report'];assert row['exit']==0 and d and all(d[k] for k in ('option_accepted','verified_output','identity_preserved','metadata_preserved'));assert len(d['rounds'])==1 and d['rounds'][0]['ok'];run=d['rounds'][0]['runs'][0];c=run['counters'];n=c.get('rewritten_edges',0)
  action,profile,mode=row['action'],row['profile'],row['mode'];dest=out/(profile if action=='J' else action+'-'+profile);s=(dest/(mode+'.ll')).read_text();before=(dest/(mode+'.ll.parsed-before.ll')).read_text()
  if action.startswith('zero_') or mode=='call_inside' or profile in ('all_missing','all_noncase','bad_manifest') or (action in ('F','H') and profile!='full_map'):
   assert n==0 and d['unchanged'] and s==before
   if action.startswith('zero_') and profile=='full_map':assert c.get('complete')==0 and c.get('budget_exhausted',0)>0
  if n:
   assert n==1 and not d['unchanged'];fast=re.findall(r'switch i64 (%loaded\.transition\w*), label %(transition\.fallback\w*) \[(.*?)\]',s,re.S);assert len(fast)==1
   selector,fallback,cases=fast[0];casevalues={int(v) for v in re.findall(r'i64 (\d+), label',cases)};expected=partial.get(profile,{43,44,99,127,128,255});assert casevalues==expected
   block=re.search(r'^'+re.escape(fallback)+r':[^\n]*\n(.*?)(?=^\S|\Z)',s,re.M|re.S);assert block;sw=re.search(r'switch i64 '+re.escape(selector)+r', label %good \[(.*?)\]',block.group(1),re.S);assert sw and {int(v) for v in re.findall(r'i64 (\d+), label',sw.group(1))}=={43,44,99,127,128,255,777}
   assert s.count('call void @business_unknown')==1 and '%state = add nsw i64 %abc, -2' in s
  if action=='J' and mode!='call_inside' and profile in partial:
   passed=n==1 and c.get('partial_candidate_edges')==c.get('partial_attempted_edges')==c.get('partial_rewritten_edges')==1 and c.get('partial_unresolved_edges')==0 and c.get('complete')==0 and c.get('budget_exhausted')==0 and run['status']==2
   precision.append(dict(mode=mode,profile=profile,passed=passed))
  if action=='J' and mode!='call_inside' and profile=='full_map':assert n==1 and c.get('partial_rewritten_edges',0)==0
 except Exception as e:common.append(dict(mode=row['mode'],profile=row['profile'],action=row['action'],error=repr(e)))
unchanged={path:sha(Path(path))==pin for path,pin in pins.items()};(out/'pins-after.json').write_text(json.dumps(unchanged,indent=2)+'\n');result=dict(rows=len(rows),common_errors=common,precision=precision,precision_pass=all(x['passed'] for x in precision),baseline=a.baseline,pins_unchanged=all(unchanged.values()));(out/'API-GATES.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));assert not common and all(unchanged.values());assert a.baseline or result['precision_pass']
