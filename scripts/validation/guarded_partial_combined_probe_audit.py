from pathlib import Path
import json,sys,copy
import guarded_loop_family_audit as family
root=Path(sys.argv[1]);rows=[];controls=0
for name in ('f-cff-O0','f-cff-O2','f-combo-O0','f-combo-O2'):
 d=root/name
 if not (d/'module.after.json').exists():continue
 before=json.load(open(d/'module.before.json'));after=json.load(open(d/'module.after.json'));manifest=json.load(open(d/'module.manifest.json'));anchor=json.load(open(root/'anchors'/name/'module.before.json'));expected=json.load(open(root/'anchors'/name/'module.after.json'));assert before==expected
 result=family.evaluate(anchor,after,manifest);reports={};count=0
 for stage,names in [('K',['invariant-self-loops']),('J',['guarded-partial-corridors']),('tail-tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('tail-constant',['constant-branches']),('tail-E',['guarded-target-sets'])]:
  report=json.load(open(d/('module.'+stage+'-report.json')));assert report['ok'] and report['iterations']==1 and [r['name'] for r in report['runs']]==names;reports[stage]=report;count+=len(report['runs'])
 assert count==9
 sets=result['actual_sets'];assert sets
 for fault in ('wrong-case','wrong-target','missing-fallback-case','wrong-selector'):
  bad=copy.deepcopy(after);fast=next(s for s in bad['switches'] if s['block']==sets[0]['block']);fallback=next(s for s in bad['switches'] if s['block']==sets[0]['fallback'])
  if fault=='wrong-case':fast['cases'][0]['value']='i64 -123'
  elif fault=='wrong-target':fast['cases'][0]['target']=fast['block']
  elif fault=='missing-fallback-case':fallback['cases']=fallback['cases'][1:]
  else:fallback['selector']='%wrong_execution'
  try:family.evaluate(anchor,bad,manifest)
  except AssertionError:controls+=1
  else:raise AssertionError('missed '+fault)
 rows.append(dict(case=name,reports=reports,**result,first_remaining_path=min(result['closure']['central_witness_paths'],key=len,default=[])))
result=dict(rows=rows,structural_controls=controls,family_controls=family.selftest(),native_executions=0,scope='Frozen I2 finals + explicit K/J/tiny/constant/E tail9; baseline47 not rerun; unknown/fullfallback retained; no native/observed-loop claim')
(root/'COMBINED-CLOSURE-REVIEW.json').write_text(json.dumps(result,indent=2)+'\n')
for r in rows:print(r['case'],r['closure']['guard_success_entry_closure'],r['reports']['J']['runs'][0]['counters'],r['first_remaining_path'])
