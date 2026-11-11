#!/usr/bin/env python3
"""Audit actual D CFG edges; ordinary branches and business cases are retained."""
import copy,json,re,sys
from pathlib import Path
import guarded_business_closure as closure
root=Path(sys.argv[1]);rows=[];controls=0

def check(before,after):
 old={b['name']:b for b in before['blocks']};new={b['name']:b for b in after['blocks']};folded=[]
 for n,b in old.items():
  match=re.search(r'br i1 (true|false),',b['terminator'])
  if not match:continue
  slot=0 if match[1]=='true' else 1
  expected=b['successors'][slot]['target']
  if n in new:assert [e['target'] for e in new[n]['successors']]==[expected],(n,expected,new[n])
  folded.append(dict(block=n,expected=expected,removed=n not in new))
 assert not any(re.search(r'br i1 (true|false),',b['terminator']) for b in new.values())
 # Live dynamic same-read guards must retain actual equality and complete fallback.
 remaining={g['block']:g for g in after['same_value_guards']};switches={s['block']:s for s in after['switches']}
 for g in before['same_value_guards']:
  if g['block'] not in new:continue
  assert g['block'] in remaining;actual=remaining[g['block']]
  for key in ('candidate','success','fallback','same_value_full_switch','fallback_has_candidate_case','fallback_candidate_matches_success'):assert actual[key]==g[key]
  original=next(s for s in before['switches'] if s['block']==g['fallback']);fallback=switches[g['fallback']]
  assert original['cases']==fallback['cases'] and original['default']==fallback['default']
 return folded
for case in ('f-cff','f-combo'):
 for level in ('O0','O2'):
  d=root/'structural-review'/f'{case}-{level}';before=json.loads((d/'tiny.json').read_text());after=json.loads((d/'final.json').read_text());prepared=json.loads((d/'prepared.json').read_text());manifest=json.loads((root/'candidate/guarded-views'/case/level/'manifest.json').read_text());report=json.loads((root/'candidate/constant-branch-tail'/case/level/'report.json').read_text())
  assert report['total_pass_runs']==37 and report['additional_pass_runs']==6 and report['inherited_pass_runs']==31
  assert [r['name'] for r in report['tiny_report']['runs']]==['mem2reg','instcombine','gvn','instcombine','dce']
  assert [r['name'] for r in report['constant_report']['runs']]==['constant-branches']
  folds=check(before,after)
  bad=copy.deepcopy(after);g=bad['same_value_guards'][0];g['candidate']='i64 -123'
  try:check(before,bad)
  except AssertionError:controls+=1
  else:raise AssertionError('corrupt guard accepted')
  live=[x for x in folds if not x['removed']]
  if live:
   bad=copy.deepcopy(after);b=next(x for x in bad['blocks'] if x['name']==live[0]['block']);b['successors']=[{'slot':0,'target':b['name']}]
   try:check(before,bad)
   except AssertionError:controls+=1
   else:raise AssertionError('literal branch wrong edge accepted')
  group,_=closure.classify(prepared,manifest);bs={s['block']:s for s in after['switches']}
  for s in before['switches']:
   if closure.signature(s)!=group and s['block'] in bs:assert s['cases']==bs[s['block']]['cases'] and s['default']==bs[s['block']]['default']
  rows.append(dict(case=case,level=level,constant_stats=report['constant_report']['runs'][0]['counters'],folded_literal_branches=folds,guards_before=len(before['same_value_guards']),guards_after=len(after['same_value_guards']),closure=closure.evaluate(prepared,after,manifest)))
assert len(rows[0]['folded_literal_branches'])==25
result=dict(rows=rows,structural_fault_controls=controls,graph_controls=closure.self_test(),native_executions=0,scope='Actual transformed D CFG; no hypothetical business-case pruning. Full-state native comparison remains the inherited frozen oracle.')
(root/'structural-review/REVIEW.json').write_text(json.dumps(result,indent=2)+'\n')
print([(x['case'],x['level'],len(x['folded_literal_branches']),x['guards_after'],x['closure']['guard_success_entry_closure']) for x in rows]);print('structural controls',controls)
