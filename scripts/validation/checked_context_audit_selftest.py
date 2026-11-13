#!/usr/bin/env python3
"""Adversarial controls for the independent copy/epoch proof; no pass execution."""
import argparse,copy,json,sys
from pathlib import Path
import checked_context_structure_audit as audit

def phi_value(inv,operand,pred,value):
 b,i,d=audit.state.tables(inv);phi=d[operand]
 for n,arm in enumerate(phi['incoming']):
  if arm['block']==pred:
   ty=audit.phi_type(phi);arm['value']=value;arm['typed']=ty+' '+value;phi['operands'][n]={'operand':value,'typed':ty+' '+value}
 phi['instruction']='  '+operand+' = phi '+audit.phi_type(phi)+' '+', '.join('[ '+a['value']+', %'+a['block']+' ]' for a in phi['incoming'])
 # state.tables returns copies for defs; update the actual instruction row.
 for row in inv['state_value_structure']:
  for n,x in enumerate(row['instructions']):
   if x['operand']==operand and x['opcode']=='phi':row['instructions'][n]={k:v for k,v in phi.items() if k!='block'}

def main(root,out):
 sys.setrecursionlimit(20000);positives=[];controls=[]
 pairs={name:(json.loads((root/(name+'.before.json')).read_text()),json.loads((root/(name+'.after.json')).read_text())) for name in ('loop','history','duplicate','liveout','sidecall','table_before','table_after')}
 for name,(before,after) in pairs.items():
  r=audit.Proof(before,after).run();positives.append(dict(case=name,contexts=len(r['checked_context_proofs']),queries=r['liveout_edge_value_proof_queries']))
 def control(case,name,mutate):
  before,after=pairs[case];bad=copy.deepcopy(after);mutate(bad)
  try:audit.Proof(before,bad).run()
  except (AssertionError,KeyError,ValueError) as e:controls.append(dict(case=case,fault=name,detected=True,reason=str(e) or type(e).__name__))
  else:raise AssertionError('undetected '+case+' '+name)
 def context(case):return audit.Proof(*pairs[case]).run()['checked_context_proofs'][0]
 loop=context('loop');history=context('history');duplicate=context('duplicate')
 # Valid-SSA stale snapshot substitutes a dominating initial observation for a
 # fresh backedge. A set-of-leaves check alone would miss this epoch error.
 for case,c in [('loop',loop),('history',history),('duplicate',duplicate)]:
  before,after=pairs[case];b,i,d=audit.state.tables(after);rp=audit.phis(i[c['clone_root']]);p=next(x for x in rp if 'history' in x['operand']);initial=audit.incoming(p,c['snapshot'] or c['seed'])
  control(case,'stale-initial-used-as-fresh-backedge',lambda inv,p=p,c=c,initial=initial:phi_value(inv,p['operand'],c['path'][-1][1],initial))
 # Correct labels/counts but wrong actual source SSA values.
 source_history=audit.phis(audit.state.tables(pairs['loop'][1])[1][loop['clone_root']])[0]
 control('loop','wrong-root-initial-value',lambda inv:phi_value(inv,source_history['operand'],loop['seed'],'0'))
 def bad_original_phi(inv):
  b,i,d=audit.state.tables(inv);p=next(p for p in audit.phis(i[loop['root']]) if any(a['block']==loop['fallback'] for a in p['incoming']));phi_value(inv,p['operand'],loop['fallback'],'0')
 control('loop','wrong-ordinary-full-fallback-PHI-value',bad_original_phi)
 def stale_snapshot(inv):
  b,i,d=audit.state.tables(inv);p=audit.phis(i[history['snapshot']])[0];phi_value(inv,p['operand'],history['seed'],'0')
 control('history','historical-snapshot-invented-value',stale_snapshot)
 def flags(inv):
  for row in inv['state_value_structure']:
   if row['block'] in {c[1] for c in loop['path']}:
    for x in row['instructions']:
     if x['opcode']=='add':x['instruction']=x['instruction'].replace(' = add ',' = add nuw ',1);return
  raise AssertionError('control fixture missing add instruction')
 control('loop','invented-copied-nuw-flag',flags)
 def source_effect(inv):
  row=next(r for r in inv['state_value_structure'] if r['block']==loop['path'][1][1]);row['instructions'].insert(0,copy.deepcopy(row['instructions'][0]))
 control('loop','duplicated-path-operation',source_effect)
 def wrong_read(inv):next(s for s in inv['switches'] if s['block']==loop['fallback'])['selector']='%history.transition'
 control('loop','full-fallback-historical-selector',wrong_read)
 def missing_case(inv):next(s for s in inv['switches'] if s['block']==loop['fallback'])['cases'].pop()
 control('loop','missing-real-full-switch-case',missing_case)
 def wrong_target(inv):next(s for s in inv['switches'] if s['block']==loop['path'][-1][1])['cases'][0]['target']=loop['seed']
 control('loop','fast-target-reenters-side-effecting-seed',wrong_target)
 def omit_slot(inv):next(b for b in inv['blocks'] if b['name']==duplicate['path'][-1][1])['successors'].pop()
 control('duplicate','omitted-actual-duplicate-root-slot',omit_slot)
 def unexplained(inv):
  b=copy.deepcopy(inv['blocks'][0]);b['name']='unexpected.effect';inv['blocks'].append(b);r=copy.deepcopy(inv['state_value_structure'][0]);r['block']=b['name'];inv['state_value_structure'].append(r)
 control('loop','unexplained-appended-effects',unexplained)
 # A new PHI value can have the same set of old/fresh leaves yet select the
 # wrong one on a specific edge. The per-edge current-definition proof rejects.
 def swap_repair(inv):
  b,i,d=audit.state.tables(inv)
  p=next(x for items in i.values() for x in audit.phis(items) if x['operand'] not in audit.state.tables(pairs['history'][0])[2] and x['operand'].startswith('%transition.live') and len({a['value'] for a in x['incoming']})>1)
  old=list(p['incoming']);v=old[-1]['value'];phi_value(inv,p['operand'],old[0]['block'],v)
 control('history','correct-leaf-set-wrong-edge-snapshot',swap_repair)
 result=dict(schema='checked-context-audit-selftest-v1',positives=positives,structural_fault_controls=controls,passes_executed=0,native_executions=0,skips=0)
 assert not out.exists();out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'positives':len(positives),'controls':len(controls),'all_detected':True}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('out',type=Path);a=p.parse_args();main(a.root,a.out)
