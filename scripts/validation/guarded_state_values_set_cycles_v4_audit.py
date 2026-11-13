#!/usr/bin/env python3
"""Separate set-aware metric; the frozen EQ-only closure is never replaced."""
import copy,json,sys
from collections import Counter
from pathlib import Path
import guarded_state_values_manifest_audit_v4 as state
family=state.family
closure=family.audit.closure

selected_selector=state.selected_selector

def sets(before,after):
 blocks,items,defs=state.validate_inventory(after);switches={s['block']:s for s in after['switches']};verified=[]
 # Every PHI must describe the actual CFG edge multiplicity. This also makes
 # removal of an inventory predecessor/slot an audit error.
 incoming={n:Counter() for n in blocks}
 for name,b in blocks.items():
  for edge in b['successors']:assert edge['target'] in blocks;incoming[edge['target']][name]+=1
 for name,instructions in items.items():
  for i in instructions:
   if i['opcode']=='phi':assert Counter(v['block'] for v in i['incoming'])==incoming[name], 'incomplete actual PHI edge coverage'
 for fast in after['switches']:
  fallback=switches.get(fast['default'])
  if fallback is None or not fast.get('same_block_load'):continue
  assert fast.get('simple_load') and fast['load_block']==fast['block']
  load=defs[fast['selector']];assert load['opcode']=='load' and load['simple'] and load['block']==fast['block']
  assert load['instruction']==fast['load']
  assert selected_selector(defs,fallback['selector'],fast['block'],fallback['block'])==fast['selector'], 'fallback selects a different LOAD'
  assert not blocks[fallback['block']]['calls'] and not blocks[fallback['block']]['stores']
  assert any(s['cases']==fallback['cases'] and s['default']==fallback['default'] for s in before['switches']), 'fallback no longer has original cases/default'
  values=[closure.int_value(c['value']) for c in fast['cases']];assert values and len(values)<=16 and len(values)==len(set(values))
  full={closure.int_value(c['value']):c['target'] for c in fallback['cases']}
  assert all(full.get(closure.int_value(c['value']))==c['target'] for c in fast['cases']), 'fast cases/targets differ from full fallback'
  edges=blocks[fast['block']]['successors'];assert [e['target'] for e in edges]==[fast['default']]+[c['target'] for c in fast['cases']]
  verified.append(dict(block=fast['block'],fallback=fallback['block'],selector=fast['selector'],cases=fast['cases'],original_full_cases=len(full)))
 return verified

def metric(anchor,before,after,manifest,hints,declared_central=None):
 projected,witnesses=state.projected(before,after,hints)
 # Check actual, unprojected edge/PHI inventories. Projection intentionally
 # removes only independently proved guard failures, never real input slots.
 valid_sets=sets(before,after);lookup={b['name']:b for b in projected['blocks']}
 for s in valid_sets:lookup[s['block']]['successors']=lookup[s['block']]['successors'][1:]
 blocks,graph,guards,invalid=closure.graph_with_success_edges(projected);assert not invalid
 # Same source Exit-PC classifier used by the frozen family audit, without
 # changing that audit's scalar-EQ-only cycle criterion.
 pc=family.pc_map(anchor)
 for n,v in family.pc_map(after).items():assert n not in pc or pc[n]==v;pc[n]=v
 def signature(sw):return (tuple(sorted(closure.int_value(c['value']) for c in sw['cases'])),('exit_pc',pc[sw['default']]) if sw['default'] in pc else ('default',sw['default']))
 original_signature,original_classify=closure.signature,closure.classify
 try:
  closure.signature=signature;closure.classify=family.audit.classify_guarded
  group,_=family.audit.classify_guarded(anchor,manifest)
  central=sorted(s['block'] for s in after['switches'] if signature(s)==group)
  full=[s for s in after['switches'] if signature(s)[0]==group[0]];assert all(s['default'] in pc for s in full)
  if declared_central is not None:family.assert_central_coverage(central,declared_central)
  admission=closure.admission(after,manifest);reachable=closure.walk(graph,admission['success']);remaining=sorted(set(central)&reachable.keys())
  scalar={g['block'] for g in guards};small={s['block'] for s in valid_sets};cycles=[]
  for component in closure.components(graph,set(reachable)):
   if (len(component)>1 or component[0] in graph[component[0]]) and (scalar|small).intersection(component):
    cycles.append(dict(blocks=sorted(component),scalar_EQ_guard_nodes=sorted(scalar.intersection(component)),validated_small_switch_nodes=sorted(small.intersection(component))))
 finally:closure.signature,closure.classify=original_signature,original_classify
 return dict(set_aware_guard_success_entry_closure=not remaining and bool(cycles),no_central_reentry=not remaining,central_blocks=central,remaining_central_blocks=remaining,central_witness_paths=[reachable[n] for n in remaining],fast_cycles=cycles,validated_sets=valid_sets,state_guard_witnesses=witnesses,native_executions=0)

def main(root):
 rows=[];controls=[]
 for case in ('f-cff-O0','f-cff-O2','f-combo-O0','f-combo-O2'):
  d=root/case
  if not (d/'module.V.json').exists():continue
  before=json.load(open(d/'module.before.json'));after=json.load(open(d/'module.V.json'));manifest=json.load(open(d/'module.manifest.json'));h=json.load(open(root/'hints'/(case+'.json')));hints=h['hints'] if h is not None else [];anchor=json.load(open(root/'anchors'/case/'module.before.json'))
  # Preserve actual prior failure, including inability to classify new PHI forms.
  old=None;old_failure=None
  try:old=family.evaluate(anchor,after,manifest)
  except (AssertionError,ValueError) as e:old_failure=str(e) or type(e).__name__
  result=metric(anchor,before,after,manifest,hints)
  sample=result['validated_sets'][0];actual=result['central_blocks'];assert actual
  for fault in ('small-load-identity','full-selector','full-cases','small-target','small-case','omitted-central'):
   bad=copy.deepcopy(after);sw={s['block']:s for s in bad['switches']};fast=sw[sample['block']];fallback=sw[sample['fallback']];declared=None
   if fault=='small-load-identity':fast['load']='  %different = load i64, ptr %different.pointer, align 1'
   elif fault=='full-selector':fallback['selector']='%different.load'
   elif fault=='full-cases':fallback['cases']=fallback['cases'][1:]
   elif fault=='small-target':fast['cases'][0]['target']=fast['block']
   elif fault=='small-case':fast['cases'][0]['value']='i64 123456789'
   else:declared=actual[1:]
   try:metric(anchor,before,bad,manifest,hints,declared)
   except (AssertionError,KeyError,ValueError):controls.append(dict(case=case,fault=fault,detected=True))
   else:raise AssertionError('undetected '+case+' '+fault)
  for hint,w in zip(hints,result['state_guard_witnesses']):
   for fault in ('state-reread','state-direction','state-phi','target-reread','target-full-cases'):
    bad=copy.deepcopy(after);blocks,items,defs=state.tables(bad)
    if fault=='state-reread':
     freeze=defs[items[hint['block']][-1]['condition']];defs[freeze['operands'][0]['operand']]['operands'][0]['operand']='%wrong.state.read'
    elif fault=='state-direction':blocks[w['state_fallback']]['successors'].reverse()
    elif fault=='state-phi':
     source_blocks=state.tables(before)[0];targets=[e['target'] for e in source_blocks[hint['block']]['successors']];phi=next(i for target in targets for i in items[target] if i['opcode']=='phi');phi['incoming'][0]['value']='%wrong.phi.value'
    elif fault=='target-reread':next(s for s in bad['switches'] if s['block']==w['target_fallback_chain'][0]['block'])['selector']='%wrong.target.read'
    else:next(s for s in bad['switches'] if s['block']==w['target_fallback_chain'][-1]['block'])['cases'].pop()
    try:metric(anchor,before,bad,manifest,hints)
    except (AssertionError,KeyError,ValueError):controls.append(dict(case=case,fault=fault,detected=True))
    else:raise AssertionError('undetected '+case+' '+fault)
  rows.append(dict(case=case,frozen_target_only_EQ_cycle_metric=old,frozen_metric_failure=old_failure,set_aware_state_success_metric=result))
 result=dict(schema='state-values-set-cycles-v4',rows=rows,structural_fault_controls=controls,native_executions=0,scope='Read-only new metric on the already executed V-stage outputs. All actual terminator slots and PHIs are checked before projection; each state guard has a single frozen i1 EQ, an exact selected clone path and same-read fallbacks with at most one fallback-local PHI. Original scalar-EQ-only metric/failures retained; no source range, memory preservation, reachability or observed-loop claim.')
 output=root/'STATE-VALUES-SET-CYCLES-V4-REVIEW.json';assert not output.exists(), 'refuse to overwrite an earlier audit'
 output.write_text(json.dumps(result,indent=2)+'\n')
 for r in rows:print(r['case'],'set-aware',r['set_aware_state_success_metric']['set_aware_guard_success_entry_closure'],'cycles',[(len(c['blocks']),len(c['scalar_EQ_guard_nodes']),len(c['validated_small_switch_nodes'])) for c in r['set_aware_state_success_metric']['fast_cycles']])
 print('controls',len(controls));return result
if __name__=='__main__':main(Path(sys.argv[1]))
