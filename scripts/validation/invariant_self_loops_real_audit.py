#!/usr/bin/env python3
from pathlib import Path
import json,sys,copy,hashlib
import guarded_loop_family_audit as family

def verify_loops(before,after):
 assert len(before)==len(after);isolated=[];removed=0
 for b,a in zip(before,after):
  assert (b['function'],b['block'])==(a['function'],a['block'])
  if b['targets']==a['targets']:
   assert all(a[k]==v for k,v in b.items());continue
  assert b['conditional'] and a['conditional'] and b['targets'][0]==b['block'] and b['targets'][1]!=b['block']
  assert b['condition']==a['condition'] and a['targets'][1]==b['targets'][1] and a['targets'][0]!=a['block']
  spin=a['true_target_body'];assert spin['block']==a['targets'][0] and spin['branch'] and not spin['conditional'] and spin['targets']==[spin['block']] and not spin['phis'] and len(spin['instructions'])==1
  assert len(a['phis'])==len(b['phis'])
  for x,y in zip(b['phis'],a['phis']):
   assert x['value']==y['value'];selfs=[e for e in x['incoming'] if e['block']==b['block']];assert len(selfs)==1 and selfs[0]['value']==x['value']
   assert y['incoming']==[e for e in x['incoming'] if e['block']!=b['block']] and y['incoming'];removed+=1
  isolated.append(dict(function=b['function'],block=b['block'],condition=b['condition'],false_exit=b['targets'][1],spin=spin['block'],removed_self_phis=len(b['phis'])))
 return isolated,removed

def main(root):
 rows=[];controls=0
 for name in ('f-cff-O0','f-cff-O2','f-combo-O0','f-combo-O2'):
  d=root/name;b=json.load(open(d/'module.loops-before.json'));a=json.load(open(d/'module.loops-after.json'));isolated,removed=verify_loops(b,a);report=json.load(open(d/'module.report.json'));stats=report['runs'][0]['counters'];assert report['ok'] and stats['isolated_loops']==len(isolated) and stats['removed_self_incomings']==removed and stats['added_blocks']==len(isolated)
  before=json.load(open(d/'module.before.json'));after=json.load(open(d/'module.after.json'));manifest=json.load(open(d/'module.manifest.json'))
  assert before['switches']==after['switches'] and before['same_value_guards']==after['same_value_guards'] and before['equality_checks']==after['equality_checks']
  anchor=json.load(open(root/'anchors'/name/'module.before.json'));frozen_input=json.load(open(root/'anchors'/name/'module.after.json'));assert before==frozen_input
  f=family.evaluate(anchor,after,manifest)
  for fault in ('false-exit','spin-exit','external-value','condition'):
   if not isolated:continue
   bad=copy.deepcopy(a);target=next(v for v in bad if v['function']==isolated[0]['function'] and v['block']==isolated[0]['block'])
   if fault=='false-exit':target['targets'][1]=target['targets'][0]
   elif fault=='spin-exit':target['true_target_body']['targets']=[target['targets'][1]]
   elif fault=='condition':target['condition']='i1 false'
   else:target['phis'][0]['incoming'][0]['value']='i64 0xBAD'
   try:verify_loops(b,bad)
   except AssertionError:controls+=1
   else:raise AssertionError('missed '+fault)
  rows.append(dict(case=name,counters=stats,isolated=isolated,closure=f['closure']['guard_success_entry_closure'],first_remaining_path=min(f['closure']['central_witness_paths'],key=len,default=[]),central_family=f['central_source_pc'],all_target_switches_and_guards_unchanged=True))
 out=dict(rows=rows,structural_fault_controls=controls,native_executions=0,scope='Single explicit K stage on frozen I1 final modules. Preserve divergence and unknown false exits; no progress or complete de-flattening claim.')
 (root/'K-REVIEW.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
if __name__=='__main__':main(Path(sys.argv[1]))
