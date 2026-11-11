#!/usr/bin/env python3
"""Validate actual small-switch success edges before conditional CFG projection."""
import copy,json,sys
from pathlib import Path
import guarded_business_closure as closure

def validate(before,after,hint):
 blocks={b['name']:b for b in after['blocks']};switches={s['block']:s for s in after['switches']}
 data=bytes.fromhex(hint['tables'][0]['bytes']);expected={int.from_bytes(data[n:n+8],'little') for n in range(0,len(data),8)}
 assert len(expected)==6
 witnessed=[]
 for fast in after['switches']:
  fallback=switches.get(fast['default'])
  if not fallback or not blocks[fast['block']]['generated']:continue
  # Require a real simple LOAD that executes once in the fast block and is used
  # by both switches; equality of LLVM parser operand identity is exposed as SSA.
  assert fast.get('simple_load') and fast.get('same_block_load')
  assert fast['selector']==fallback['selector'] and fast['load']==fallback['load']
  assert fast['load_block']==fallback['load_block']==fast['block']
  assert not fallback['same_block_load'] and not blocks[fallback['block']]['stores'] and not blocks[fallback['block']]['calls']
  values=[closure.int_value(c['value']) for c in fast['cases']]
  assert len(values)==6 and set(values)==expected
  full={closure.int_value(c['value']):c['target'] for c in fallback['cases']}
  assert all(full.get(closure.int_value(c['value']))==c['target'] for c in fast['cases'])
  assert any(s['cases']==fallback['cases'] and s['default']==fallback['default'] for s in before['switches'])
  edges=blocks[fast['block']]['successors'];assert len(edges)==7 and edges[0]['target']==fallback['block']
  assert [e['target'] for e in edges[1:]]==[c['target'] for c in fast['cases']]
  witnessed.append({'fast':fast['block'],'fallback':fallback['block'],'selector':fast['selector'],'candidates':values,'case_targets':[c['target'] for c in fast['cases']],'original_full_cases':len(full)})
 assert witnessed,'no actual six-target fast switch found'
 # Preserve previously validated single-target guards.
 guards={g['block']:g for g in after['same_value_guards']}
 for g in before['same_value_guards']:
  assert g['block'] in guards
  for key in ('candidate','success','fallback','same_value_full_switch','fallback_has_candidate_case','fallback_candidate_matches_success'):assert guards[g['block']][key]==g[key]
 return witnessed

def classify_guarded(prepared,manifest):
 ranges=[(int(row['offset'],16),int(row['offset'],16)+len(bytes.fromhex(row['bytes']))) for row in manifest['memory']]
 _,_,guards,invalid=closure.graph_with_success_edges(prepared)
 if invalid:raise ValueError('invalid original same-read guard anchor')
 by_fallback={g['fallback']:g for g in guards};groups=set()
 for sw in prepared['switches']:
  anchor=sw.get('same_block_load',False)
  g=by_fallback.get(sw['block'])
  if g is not None:
   anchor=(sw.get('load')==g['load'] and sw.get('load_block')==g['block'] and sw['selector']==g['load'].strip().split(' = ',1)[0])
  if anchor and 'address_decimal' in sw and any(a<=int(sw['address_decimal']) and int(sw['address_decimal'])+8<=b for a,b in ranges):groups.add(closure.signature(sw))
 if len(groups)!=1:raise ValueError('CFF signature not uniquely anchored by actual same-read target LOAD')
 group=next(iter(groups));return group,[s for s in prepared['switches'] if closure.signature(s)==group]

def evaluate(before,after,manifest,hint):
 witnesses=validate(before,after,hint)
 projected=copy.deepcopy(after);lookup={b['name']:b for b in projected['blocks']}
 for w in witnesses:lookup[w['fast']]['successors']=lookup[w['fast']]['successors'][1:]
 saved_classify=closure.classify
 try:
  closure.classify=classify_guarded;result=closure.evaluate(before,projected,manifest)
 finally:closure.classify=saved_classify
 result['limits']='Conditional graph excludes only verified actual single-target guard failures and verified same-read six-case small-switch defaults. Six ordinary business case edges all remain. No file bytes are treated as current memory facts. This is not proof every runtime guard succeeds.'
 return witnesses,result

def classification_controls():
 def block(name,targets):return dict(name=name,successors=[dict(slot=i,target=t) for i,t in enumerate(targets)])
 load='  %v = load i64, ptr %p, align 1'
 guard=dict(block='g',success='body',fallback='cff-fallback',load=load,candidate='i64 7',same_value_full_switch=True,fallback_has_candidate_case=True,fallback_candidate_matches_success=True)
 cff=dict(block='cff-fallback',selector='%v',load=load,load_block='g',same_block_load=False,address_decimal='16',cases=[dict(value='i64 7',target='body')],default='cff-exit')
 business=dict(block='business',selector='%b',same_block_load=True,address_decimal='32',cases=[dict(value='i64 7',target='body')],default='business-exit')
 prepared=dict(blocks=[block('g',['body','cff-fallback']),block('body',[]),block('cff-fallback',[])],same_value_guards=[guard],switches=[cff,business]);manifest=dict(memory=[dict(offset='0x10',bytes='0700000000000000')])
 group,originals=classify_guarded(prepared,manifest)
 assert [x['block'] for x in originals]==['cff-fallback'] and group!=closure.signature(business)
 bad=copy.deepcopy(prepared);bad['switches'][0]['load']='  %old = load i64, ptr %p, align 1'
 try:classify_guarded(bad,manifest)
 except ValueError:pass
 else:raise AssertionError('different-execution LOAD classified as proven anchor')
 return dict(cross_block_actual_load_anchor=1,same_cases_different_default_kept_separate=1,rejected_wrong_load_identity=1,native_executions=0)

if __name__=='__main__':
 root=Path(sys.argv[1]);rows=[];controls=0
 for case in ('f-cff-O0','f-cff-O2','f-combo-O0','f-combo-O2'):
  d=root/case;before=json.loads((d/'module.before.json').read_text());after=json.loads((d/'module.after.json').read_text());manifest=json.loads((d/'module.manifest.json').read_text());hint=json.loads((root/'hints'/(case+'.json')).read_text())
  witnesses,result=evaluate(before,after,manifest,hint)
  for fault in ('wrong-case','wrong-target','wrong-fallback-case','wrong-selector'):
   bad=copy.deepcopy(after);fast=next(s for s in bad['switches'] if s['block']==witnesses[0]['fast']);fallback=next(s for s in bad['switches'] if s['block']==witnesses[0]['fallback'])
   if fault=='wrong-case':fast['cases'][0]['value']='i64 -123'
   elif fault=='wrong-target':fast['cases'][0]['target']=fast['block']
   elif fault=='wrong-fallback-case':fallback['cases']=fallback['cases'][1:]
   else:fallback['selector']='%different.execution.load'
   try:validate(before,bad,hint)
   except AssertionError:controls+=1
   else:raise AssertionError('undetected structural fault '+fault)
  rows.append(dict(case=case,actual_small_sets=witnesses,closure=result))
 (root/'REVIEW.json').write_text(json.dumps(dict(rows=rows,structural_fault_controls=controls,classification_controls=classification_controls(),native_executions=0),indent=2)+'\n')
 assert rows[0]['closure']['guard_success_entry_closure'],'cff O0 strict actual fast-loop closure gate failed'
 print([(r['case'],len(r['actual_small_sets']),r['closure']['guard_success_entry_closure']) for r in rows]);print('controls',controls)
