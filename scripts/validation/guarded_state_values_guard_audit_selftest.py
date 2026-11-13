#!/usr/bin/env python3
"""Checker controls only, with old public outputs and synthetic freeze insertion."""
import argparse,json,re,subprocess
from pathlib import Path
from guarded_state_values_guard_audit import audit
p=argparse.ArgumentParser();p.add_argument('old_root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();out=a.output;out.mkdir(exist_ok=False);rows=[]
for mode in ('plain','freeze'):
 old=(a.old_root/'main/normal'/(mode+'.ll')).read_text();before=(a.old_root/'main/normal'/(mode+'.ll.parsed-before.ll')).read_text()
 try:audit(before,old,2)
 except AssertionError:rows.append({'mode':mode,'control':'actual-old-unsafe-output','detected':True})
 else:raise AssertionError('old unsafe output admitted')
 eq=re.search(r'(%[-\w.]+) = icmp eq i64 %state, 2\b',old);assert eq;line=re.search(r'^  br i1 '+re.escape(eq.group(1))+r',[^\n]+$',old,re.M);assert line;safe=old[:line.start()]+'  %checker.defined = freeze i1 '+eq.group(1)+'\n'+line.group().replace('br i1 '+eq.group(1),'br i1 %checker.defined')+old[line.end():];audit(before,safe,2)
 src=out/(mode+'-synthetic-freeze.ll');src.write_text(safe);r=subprocess.run(['opt-21','-passes=verify','-disable-output',str(src)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180);assert r.returncode==0,r.stderr;rows.append({'mode':mode,'control':'synthetic-freeze-positive-only','passed':True,'native_executions':0})
 faults={
  'freeze-original-cmp':safe.replace('freeze i1 '+eq.group(1),'freeze i1 %cmp',1),
  'EQ-different-state':safe.replace('icmp eq i64 %state, 2','icmp eq i64 %raw, 2',1),
  'original-cmp-modified':safe.replace('icmp ugt i64 %state, 50','icmp ugt i64 %state, 49',1),
  'state-definition-modified':safe.replace('%state = freeze i64 %raw','%state = freeze i64 %entry_state',1) if mode=='freeze' else safe.replace('%state = add i64 %raw, 0','%state = add i64 %raw, 1',1),
  'raw-state-load-modified':safe.replace('%raw = load i64, ptr %statep, align 1','%raw = load i64, ptr %seedp, align 1',1),
  'fallback-direction-modified':safe.replace('br i1 %cmp, label %large, label %small','br i1 %cmp, label %small, label %large',1),
  'EQ-wrong-prediction':safe.replace('icmp eq i64 %state, 2','icmp eq i64 %state, 3',1),
  'branch-bypasses-freeze':safe.replace('br i1 %checker.defined','br i1 '+eq.group(1),1)}
 for label,text in faults.items():
  assert text!=safe,label
  try:audit(before,text,2)
  except AssertionError:rows.append({'mode':mode,'control':label,'detected':True})
  else:raise AssertionError('checker failed '+label)
summary={'scope':'Strict fixture checker selftest; actual old outputs reject, synthetic single-freeze inputs verify; no candidate or native execution','positive':2,'negative':18,'rows':rows};(out/'SUMMARY.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='rows'}))
