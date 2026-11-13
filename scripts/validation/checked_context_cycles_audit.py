#!/usr/bin/env python3
"""Separate conditional closure metric gated by a complete checked-copy proof.

Old EQ-only and V4 same-target metrics are retained verbatim (including failures).
Only actual checked-copy witnesses authorize a fast case entering a fresh root.
"""
import argparse,copy,json,sys
from pathlib import Path
import checked_context_structure_audit as checked
import guarded_state_values_set_cycles_v4_audit as v4
state=v4.state
family=v4.family
closure=v4.closure

def evaluate(anchor,value_before,before,after,manifest,hints,declared_central=None):
 # Re-prove the actual V stage instead of trusting its saved report or hints.
 prior=v4.metric(anchor,value_before,before,manifest,hints)
 proof=checked.Proof(before,after).run()
 projectors={}
 def put(block,slots,reason):
  slots=list(slots)
  if block in projectors:assert projectors[block]['slots']==slots, 'conflicting conditional projections'
  projectors[block]={'slots':slots,'reason':reason}
 # V4 has already checked every actual LOAD/fallback/target and all state EQs.
 for g in before['same_value_guards']:put(g['block'],[0],'previously proved scalar same-read success')
 for w in prior['state_guard_witnesses']:put(w['seed'],[0],'previously proved frozen state EQ success')
 for s in prior['validated_sets']:put(s['block'],range(1,len(s['cases'])+1),'previously proved small-switch case success')
 prior_projectors=copy.deepcopy(projectors)
 for c in proof['checked_context_proofs']:
  for source,fresh,slot in c['path'][:-1]:
   if source in prior_projectors:put(fresh,prior_projectors[source]['slots'],'copied equivalent source conditional success')
  terminal=c['path'][-1][1]
  sw=next(s for s in after['switches'] if s['block']==terminal)
  put(terminal,range(1,len(sw['cases'])+1),'new fully proved same-read checked-context case success')
 blocks={b['name']:b for b in after['blocks']};graph={n:[e['target'] for e in b['successors']] for n,b in blocks.items()}
 for n,p in projectors.items():
  assert n in blocks
  actual=blocks[n]['successors'];assert len(p['slots'])==len(set(p['slots'])) and all(0<=k<len(actual) for k in p['slots'])
  graph[n]=[actual[k]['target'] for k in p['slots']]
 # Fixed original D37 CFF family, with source Exit-PC classification. No old
 # central selector (including newly copied full fallback) may be omitted.
 pc=family.pc_map(anchor)
 for n,v in family.pc_map(after).items():assert n not in pc or pc[n]==v;pc[n]=v
 def signature(sw):return (tuple(sorted(closure.int_value(c['value']) for c in sw['cases'])),('exit_pc',pc[sw['default']]) if sw['default'] in pc else ('default',sw['default']))
 old_signature,old_classify=closure.signature,closure.classify
 try:
  closure.signature=signature;closure.classify=family.audit.classify_guarded
  group,_=family.audit.classify_guarded(anchor,manifest)
  central=sorted(s['block'] for s in after['switches'] if signature(s)==group)
  full=[s for s in after['switches'] if signature(s)[0]==group[0]];assert all(s['default'] in pc for s in full), 'unknown default in full CFF family'
  assert central
  if declared_central is not None:family.assert_central_coverage(central,declared_central)
  admission=closure.admission(after,manifest);reachable=closure.walk(graph,admission['success']);remaining=sorted(set(central)&reachable.keys())
  fast={s['block'] for s in prior['validated_sets']}|{c['path'][-1][1] for c in proof['checked_context_proofs']}
  scalar={g['block'] for g in before['same_value_guards']}
  cycles=[]
  for component in closure.components(graph,set(reachable)):
   if (len(component)>1 or component[0] in graph[component[0]]) and (fast|scalar).intersection(component):
    cycles.append({'blocks':sorted(component),'validated_fast_nodes':sorted(fast.intersection(component)),'validated_scalar_nodes':sorted(scalar.intersection(component))})
 finally:closure.signature,closure.classify=old_signature,old_classify
 old_EQ=None;old_EQ_failure=None;old_V4=None;old_V4_failure=None
 try:old_EQ=family.evaluate(anchor,after,manifest)
 except (AssertionError,ValueError,KeyError) as e:old_EQ_failure=str(e) or type(e).__name__
 try:old_V4=v4.metric(anchor,value_before,after,manifest,hints)
 except (AssertionError,ValueError,KeyError) as e:old_V4_failure=str(e) or type(e).__name__
 return dict(schema='checked-context-state-success-v1',checked_context_proof=proof,previous_actual_V_stage_V4_metric=prior,frozen_final_EQ_only_metric=old_EQ,frozen_final_EQ_only_metric_failure=old_EQ_failure,frozen_final_V4_same_target_metric=old_V4,frozen_final_V4_same_target_metric_failure=old_V4_failure,checked_context_state_success_metric=dict(checked_context_guard_success_entry_closure=not remaining and bool(cycles),no_central_reentry=not remaining,central_blocks=central,remaining_central_blocks=remaining,central_witness_paths=[reachable[n] for n in remaining],fast_cycles=cycles,conditional_projectors=projectors,reachable_calls=[n for n in reachable if blocks[n]['calls']],all_ordinary_business_edges_retained=True,full_fallback_semantics_proved_before_projection=True),native_executions=0,scope='Conditional structural closure only after complete graph/SSA copy proof; every ordinary/business edge is retained and only independently proved guard-failure edges are projected. No feasibility, range, immutable-memory or observed-iteration claim.')

def main():
 p=argparse.ArgumentParser()
 for n in ('before','after','before-ll','after-ll','value-before','source-anchor','manifest','hints','out'):p.add_argument('--'+n,required=True)
 p.add_argument('--inventory-tool',required=True);p.add_argument('--inventory-runtime',required=True);p.add_argument('--inventory-runtime-sha256',required=True)
 a=p.parse_args();sys.setrecursionlimit(20000)
 read=lambda n:json.loads(Path(getattr(a,n)).read_text())
 before,after=read('before'),read('after');h=read('hints');hints=[] if h is None else h['hints']
 result=evaluate(read('source_anchor'),read('value_before'),before,after,read('manifest'),hints)
 result['module_identity']=checked.module_identity(Path(a.before_ll).read_text(),Path(a.after_ll).read_text(),before['function'])
 result['actual_LL_JSON_frame_binding']=checked.bind_frames(a.inventory_tool,a.inventory_runtime,a.inventory_runtime_sha256,before['function'],[('before',a.before_ll,a.before),('after',a.after_ll,a.after)],a.out)
 result['bindings']={n:{'path':getattr(a,n),'sha256':checked.sha(getattr(a,n))} for n in ('before','after','before_ll','after_ll','value_before','source_anchor','manifest','hints')}
 out=Path(a.out);assert not out.exists(), 'refuse to overwrite earlier metric';out.write_text(json.dumps(result,indent=2)+'\n')
 r=result['checked_context_state_success_metric'];print(json.dumps({'checked_context_closure':r['checked_context_guard_success_entry_closure'],'remaining_central':r['remaining_central_blocks'],'contexts':len(result['checked_context_proof']['checked_context_proofs']),'native_executions':0}))
if __name__=='__main__':main()
