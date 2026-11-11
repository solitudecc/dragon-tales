#!/usr/bin/env python3
"""Saved final F native binding and same-read fallback audit; no new executions."""
from pathlib import Path
import json,sys,copy,hashlib
import guarded_target_sets_real_audit as audit
import guarded_loop_family_audit as family
from guarded_entry_real import bind_wrapper
root=Path(sys.argv[1]);rows=[];controls=0
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def validate_sets(before,after):
 bs={b['name']:b for b in after['blocks']};sw={s['block']:s for s in after['switches']};sets=[]
 for fast in after['switches']:
  fallback=sw.get(fast['default'])
  if not fallback or not bs[fast['block']]['generated']:continue
  assert fast.get('simple_load') and fast.get('same_block_load') and fast['selector']==fallback['selector'] and fast['load']==fallback['load'] and fast['load_block']==fallback['load_block']==fast['block']
  full={audit.closure.int_value(c['value']):c['target'] for c in fallback['cases']}
  assert 0<len(fast['cases'])<=16 and all(full.get(audit.closure.int_value(c['value']))==c['target'] for c in fast['cases'])
  assert any(audit.closure.signature(s)==audit.closure.signature(fallback) for s in before['switches'])
  sets.append(dict(block=fast['block'],fallback=fallback['block'],cases=fast['cases'],selector=fast['selector']))
 return sets
for case in ('f-cff','f-combo'):
 for level in ('O0','O2'):
  d=root/'candidate/state-corridor-tail'/case/level;g=root/'candidate/guarded-views'/case/level;o=root/'candidate/oracle'/case/level/'s0-product-O0'
  activation=json.loads((d/'activation.json').read_text());manifest=json.loads((g/'manifest.json').read_text());before=json.loads((d/'module.before.json').read_text());after=json.loads((d/'module.after.json').read_text())
  assert activation['state']=='finished' and activation['returncode']==0 and activation['total_pass_runs']==45
  assert sha(d/'D-final.ll')==activation['input_sha256'] and sha(d/'module.after.ll')==activation['after_sha256']
  raw=(d/'module.after.ll').read_text();assert raw==(g/'after-raw.ll').read_text()
  bound,binding=bind_wrapper(raw,manifest['root_symbol'],manifest['view_symbol'],int(manifest['entry'],16));assert bound==(g/'after-bound.ll').read_text()==(o/'raw.ll').read_text()
  commands=[json.loads(line) for line in (o/'commands.jsonl').read_text().splitlines()];llc=[c for c in commands if c and c[0]=='llc-21'];assert len(llc)==1 and str(o/'raw.ll') in llc[0]
  assert json.loads((d/'module.manifest.json').read_text())==manifest
  verify=json.loads((d/'module.verify.json').read_text());assert verify['after'] and verify['entry_manifest_unchanged']
  family_result=family.evaluate(before,after,manifest);sets=family_result['actual_sets'];assert sets
  for fault in ('wrong-case','wrong-target','missing-fallback-case','wrong-selector'):
   bad=copy.deepcopy(after);fast=next(s for s in bad['switches'] if s['block']==sets[0]['block']);fallback=next(s for s in bad['switches'] if s['block']==sets[0]['fallback'])
   if fault=='wrong-case':fast['cases'][0]['value']='i64 -123'
   elif fault=='wrong-target':fast['cases'][0]['target']=fast['block']
   elif fault=='missing-fallback-case':fallback['cases']=fallback['cases'][1:]
   else:fallback['selector']='%wrong_execution'
   try:family.evaluate(before,bad,manifest)
   except AssertionError:controls+=1
   else:raise AssertionError('missed '+fault)
  closure=family_result['closure'];family.assert_central_coverage(family_result['central_blocks'],family_result['central_blocks'])
  reports={}
  for stage,names in [('F',['guarded-state-corridors']),('tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('constant',['constant-branches']),('', ['guarded-target-sets'])]:
   report=json.loads((d/('module.'+(stage+'-' if stage else '')+'report.json')).read_text());assert report['ok'] and report['iterations']==1 and [r['name'] for r in report['runs']]==names;reports[stage or 'E']=report
  rows.append(dict(case=case,level=level,reports=reports,actual_sets=sets,closure=closure,central_blocks=family_result['central_blocks'],central_source_pc=family_result['central_source_pc'],source_pc_groups=family_result['source_pc_groups'],raw_sha256=sha(g/'after-raw.ll'),compiled_bound_sha256=sha(o/'raw.ll'),llc_command=llc[0],binding=binding))
result=dict(rows=rows,structural_fault_controls=controls,classification_controls=audit.classification_controls(),family_controls=family.selftest(),native_executions=0,scope='Saved output binding; original full state comparator unchanged. All validated fast cases retained and only same-read guard failures excluded. F may legitimately redirect original case edges; final fast/fallback target equality checked. No observed loop execution claimed.')
(root/'G-REVIEW.json').write_text(json.dumps(result,indent=2)+'\n');assert rows[0]['closure']['guard_success_entry_closure'];print([(r['case'],r['level'],r['closure']['guard_success_entry_closure']) for r in rows]);print('fault controls',controls)
