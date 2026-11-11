#!/usr/bin/env python3
"""Bind the real native export to the independently checked E module."""
import copy,hashlib,json,sys
from pathlib import Path
import guarded_target_sets_real_audit as audit
from guarded_entry_real import bind_wrapper
root=Path(sys.argv[1]);rows=[];faults=0
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for case in ('f-cff','f-combo'):
 for level in ('O0','O2'):
  d=root/'candidate/target-set-tail'/case/level;g=root/'candidate/guarded-views'/case/level;activation=json.loads((d/'activation.json').read_text());manifest=json.loads((g/'manifest.json').read_text());hint=json.loads((root/'harness/hints'/(case+'-'+level+'.json')).read_text());before=json.loads((d/'module.before.json').read_text());after=json.loads((d/'module.after.json').read_text())
  assert activation['state']=='finished' and activation['returncode']==0 and activation['total_pass_runs']==38
  assert sha(d/'D-final.ll')==activation['input_sha256'] and sha(d/'module.after.ll')==activation['after_sha256']
  raw=(d/'module.after.ll').read_text();assert raw==(g/'after-raw.ll').read_text()
  bound,binding=bind_wrapper(raw,manifest['root_symbol'],manifest['view_symbol'],int(manifest['entry'],16));assert bound==(g/'after-bound.ll').read_text()
  assert json.loads((d/'module.manifest.json').read_text())==manifest
  verification=json.loads((d/'module.verify.json').read_text());assert verification['after'] and verification['entry_manifest_unchanged']
  actual,closure=audit.evaluate(before,after,manifest,hint)
  for fault in ('wrong-case','wrong-target','wrong-fallback-case','wrong-selector'):
   bad=copy.deepcopy(after);fast=next(s for s in bad['switches'] if s['block']==actual[0]['fast']);fallback=next(s for s in bad['switches'] if s['block']==actual[0]['fallback'])
   if fault=='wrong-case':fast['cases'][0]['value']='i64 -123'
   elif fault=='wrong-target':fast['cases'][0]['target']=fast['block']
   elif fault=='wrong-fallback-case':fallback['cases']=fallback['cases'][1:]
   else:fallback['selector']='%wrong_execution'
   try:audit.validate(before,bad,hint)
   except AssertionError:faults+=1
   else:raise AssertionError('corrupt final E structure accepted: '+fault)
  report=json.loads((d/'module.report.json').read_text());assert report['ok'] and len(report['runs'])==1 and report['runs'][0]['name']=='guarded-target-sets'
  rows.append(dict(case=case,level=level,raw_native_export_sha256=sha(g/'after-raw.ll'),bound_native_export_sha256=sha(g/'after-bound.ll'),wrapper_binding=binding,E_stats=report['runs'][0]['counters'],actual_small_sets=actual,closure=closure))
result=dict(rows=rows,structural_fault_controls=faults,classification_controls=audit.classification_controls(),native_executions=0,scope='Saved structure audit, independent of the unchanged full-state execution oracle. Actual E module is exactly the exported raw module and its unique first-kernel-bound native module. In-memory D object is deliberately not represented as having run E.')
(root/'E-REVIEW.json').write_text(json.dumps(result,indent=2)+'\n');assert rows[0]['closure']['guard_success_entry_closure'];print([(r['case'],r['level'],r['closure']['guard_success_entry_closure']) for r in rows]);print('controls',faults)
