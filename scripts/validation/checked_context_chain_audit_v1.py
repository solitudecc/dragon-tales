#!/usr/bin/env python3
"""Read-only D37/V4 -> C59 -> exact LLVM import bridge -> Bit60 chain.

No producer, optimizer or target-native entry point is invoked. Every supplied
inventory is rebound to raw verified LLVM. No original IR/JSON is normalized.
"""
import argparse,copy,difflib,json,os,subprocess,sys,tempfile,stat
from pathlib import Path
import checked_context_structure_audit_v2 as checked
import checked_metadata_alpha_audit_v1 as alpha
import guarded_state_values_set_cycles_v4_audit as v4
family=v4.family
closure=v4.closure
ROLES=('anchor','v_before','v_after','c59_after','bit_before','bit_after')
ORDER=['V4','C59','C59-to-Bit-parse','Bit60']
BRIDGE_SHA='29a27688cdf07c5b07b01bf46ac8d2765df582ef862747770c37e2758432f0da'
SHARED_INVENTORY_SHA='4ba0db00cb71d0ea283b2ad9766dbfaf54fa60afde377a8d48faf6dd2c207942'
INVENTORY_SHAS={
 'ca7b33176ea3dea2764d89443e9a19ef301a135ae4e853ac899237b819c899c2',
 # Separate explicit reference option, verified byte-for-byte on all 28
 # frozen state/generic frames (archive 36dda701...). No plan is rewritten.
 SHARED_INVENTORY_SHA,
}
ANCHOR_SHAS={'27ed455ebec9736544411d3c5fb9f85fa08009742109db49519ded912cb6e148',SHARED_INVENTORY_SHA}
SOURCE_FILES={__file__,alpha.__file__,checked.__file__,v4.__file__,v4.state.__file__,family.__file__,family.audit.__file__,closure.__file__}

def write(path,value):
 path=Path(path);assert not path.exists(), 'refuse to overwrite evidence '+str(path)
 path.write_text(json.dumps(value,indent=2)+'\n')

def raw_text(path):return Path(path).read_bytes().decode('utf-8')

def raw_diff(path,left,right):
 path=Path(path);assert not path.exists()
 path.write_text(''.join(difflib.unified_diff(left.splitlines(True),right.splitlines(True),fromfile='expected-or-before',tofile='actual-or-after')))
 return dict(path=str(path),sha256=checked.sha(path),bytes=path.stat().st_size)

def validate_plan(plan):
 assert plan['schema']=='checked-context-sequential-chain-plan-v1'
 assert plan['stage_order']==ORDER, 'missing or reordered chain stage'
 assert set(plan['frames'])==set(ROLES), 'missing/extra frame role'
 for role in ROLES:
  frame=plan['frames'][role];assert set(frame)=={'ll','json','ll_sha256','json_sha256'}
  for kind in ('ll','json'):assert checked.sha(frame[kind])==frame[kind+'_sha256'], 'frame pin differs '+role+' '+kind
 for name in ('manifest','hints'):
  assert checked.sha(plan[name]['path'])==plan[name]['sha256'], 'specification pin differs'
 tools=plan['tools'];assert set(tools)=={'inventory','anchor_inventory','bridge','runtime','runtime_sha256'}
 for name in ('inventory','anchor_inventory','bridge'):
  assert set(tools[name])=={'path','sha256'} and checked.sha(tools[name]['path'])==tools[name]['sha256'], 'tool pin differs '+name
 assert tools['bridge']['sha256']==BRIDGE_SHA, 'unreviewed import bridge tool'
 assert tools['inventory']['sha256'] in INVENTORY_SHAS, 'unreviewed raw inventory reference tool'
 assert tools['anchor_inventory']['sha256'] in ANCHOR_SHAS, 'unreviewed generic anchor reference tool'
 runtime=Path(tools['runtime']);assert checked.sha(runtime/'libdragon.so')==tools['runtime_sha256']
 assert json.loads((runtime/'MANIFEST.json').read_text())['files']['libdragon.so']['sha256']==tools['runtime_sha256']
 return {r:json.loads(Path(plan['frames'][r]['json']).read_text()) for r in ROLES}

def input_stamps(plan):
 paths=set(SOURCE_FILES)
 paths.update(frame[k] for frame in plan['frames'].values() for k in ('ll','json'))
 paths.update(plan[n]['path'] for n in ('manifest','hints'))
 paths.update(plan['tools'][n]['path'] for n in ('inventory','anchor_inventory','bridge'))
 runtime=Path(plan['tools']['runtime'])
 paths.update(str(runtime/n) for n in ('MANIFEST.json','libdragon.so','libdragon_c.so'))
 result={}
 for name in sorted(paths):
  p=Path(name);a=p.lstat();assert stat.S_ISREG(a.st_mode) and not p.is_symlink(), 'nonregular/symlink input refused'
  result[name]=(a.st_dev,a.st_ino,a.st_nlink,a.st_size,a.st_mtime_ns,a.st_ctime_ns)
 return result

def evidence_namespace(out):
 suffixes=['','.anchor-IMPORT.json','.BRIDGE.json','.BRIDGE.expected.ll','.BRIDGE.expected.json','.BRIDGE.ll.diff','.BRIDGE.json.diff','.BRIDGE-COMMAND.json']
 suffixes.extend('.'+r+'-IMPORT.json' for r in ROLES if r!='anchor')
 suffixes.extend('.'+name+'-outside.diff' for name in ('V4','C59','Bit60'))
 for suffix in suffixes:assert not Path(str(out)+suffix).exists(), 'evidence path already exists; refuse possible overwrite'

def bind_anchor(plan,anchor,out):
 t=plan['tools'];f=plan['frames']['anchor'];work=Path(tempfile.mkdtemp(prefix=out.name+'.anchor-import-',dir=out.parent));expected=work/'inventory.json'
 command=[t['anchor_inventory']['path'],f['ll'],anchor['function'],str(expected)]
 # The explicit shared reference's base mode independently emits all five
 # generic fields. Never derive D37 by dropping a state-inventory field.
 if t['anchor_inventory']['sha256']==SHARED_INVENTORY_SHA:command.append('base')
 p=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,LD_LIBRARY_PATH=t['runtime']))
 row=dict(command=command,exit=p.returncode,stdout=p.stdout,stderr=p.stderr,tool_sha256=t['anchor_inventory']['sha256'],runtime_sha256=t['runtime_sha256'],LD_LIBRARY_PATH=t['runtime'],actual_raw_sha256=f['ll_sha256'],actual_json_sha256=f['json_sha256'],exact_generic_schema_parse_verify_inventory=(p.returncode==0 and expected.read_bytes()==Path(f['json']).read_bytes()))
 write(str(out)+'.anchor-IMPORT.json',row)
 assert row['exact_generic_schema_parse_verify_inventory'], 'D37 generic inventory differs from independently verified raw; '+str(work)
 expected.unlink();work.rmdir();return row

def bind_bridge(plan,out):
 t=plan['tools'];source=plan['frames']['c59_after'];actual=plan['frames']['bit_before']
 assert actual['ll'].endswith('.ll') and actual['json']==actual['ll'][:-3]+'.json', 'bridge raw/JSON must share actual producer prefix'
 report=Path(str(out)+'.BRIDGE');assert not Path(str(report)+'.json').exists()
 command=[t['bridge']['path'],source['ll'],actual['ll'][:-3],str(report)]
 p=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,LD_LIBRARY_PATH=t['runtime']))
 row=dict(command=command,exit=p.returncode,stdout=p.stdout,stderr=p.stderr,tool_sha256=t['bridge']['sha256'],runtime_sha256=t['runtime_sha256'],LD_LIBRARY_PATH=t['runtime'],original_C59_after_raw=source,actual_Bit_before=actual,passes_executed=0,native_executions=0)
 receipt=Path(str(report)+'.json');row['reference_report']=json.loads(receipt.read_text()) if receipt.exists() else None
 row['raw_differences']=[]
 for kind in ('ll','json'):
  expected=Path(str(report)+'.expected.'+kind)
  if expected.exists():row['raw_differences'].append(raw_diff(str(report)+'.'+kind+'.diff',raw_text(expected),raw_text(actual[kind])))
 row['source_and_destination_full_inventory_bytes_equal']=Path(source['json']).read_bytes()==Path(actual['json']).read_bytes()
 write(str(out)+'.BRIDGE-COMMAND.json',row)
 assert p.returncode==0 and row['reference_report'], 'C59-after to Bit-before pure LLVM bridge refused'
 r=row['reference_report'];assert r['IR_exact_independent_parse_render'] and r['JSON_exact_independent_parse_render'] and r['passes_executed']==r['target_native_executions']==0 and r['mutation_control']=='none'
 assert row['source_and_destination_full_inventory_bytes_equal'], 'bridge inventory bytes changed'
 return row

def outside_stage(plan,role1,role2,name,out,allow_alpha=False):
 a=raw_text(plan['frames'][role1]['ll']);b=raw_text(plan['frames'][role2]['ll']);function=json.loads(Path(plan['frames'][role1]['json']).read_text())['function']
 try:return dict(stage=name,old_outside_byte_identity=checked.module_identity(a,b,function),metadata_alpha_certificate=None)
 except AssertionError as e:
  row=dict(stage=name,old_outside_byte_identity=None,old_outside_byte_identity_failure=str(e),original_outside_diff=raw_diff(str(out)+'.'+name+'-outside.diff',alpha.outside(a,function),alpha.outside(b,function)))
  if not allow_alpha:raise AssertionError(name+' whole-module outside byte identity refused; '+row['original_outside_diff']['path'])
  row['metadata_alpha_certificate']=alpha.prove(a,b,function)
  assert row['metadata_alpha_certificate']['old_outside_byte_identity'] is False
  return row

def initial_projectors(before,prior):
 p={}
 def put(n,slots,kind,reason):
  row=dict(slots=list(slots),kind=kind,reason=reason,proved_at='V4')
  assert n not in p or p[n]['slots']==row['slots'], 'conflicting initial projection'
  p[n]=row
 for g in before['same_value_guards']:put(g['block'],[0],'scalar','V4 actual same-read scalar EQ success')
 for w in prior['state_guard_witnesses']:put(w['seed'],[0],'state','V4 actual frozen-state EQ success')
 for s in prior['validated_sets']:put(s['block'],range(1,len(s['cases'])+1),'fast','V4 actual same-read small-switch case success')
 return p

def propagate(before,after,proof,old,name):
 result=copy.deepcopy(old);switches={s['block']:s for s in after['switches']}
 def copied(source,fresh):
  if source not in old:return
  row=copy.deepcopy(old[source]);row.update(proved_at=name,source_projector=source,reason='fully proved copied equivalent conditional success')
  assert fresh not in result, 'new copied projector already exists';result[fresh]=row
 for context in proof['checked_context_proofs']:
  for source,fresh,_ in context['path'][:-1]:copied(source,fresh)
  source,fresh,_=context['path'][-1]
  # The full fallback is the exact original terminal switch, so an earlier
  # terminal projection transports there. The new small switch has its own
  # independently proved case slots, never the old source's slot numbers.
  copied(source,context['fallback'])
  sw=switches[fresh];assert fresh not in result
  result[fresh]=dict(slots=list(range(1,len(sw['cases'])+1)),kind='fast',reason='new completely proved same-actual-LOAD case success',proved_at=name)
 blocks={b['name']:b for b in after['blocks']}
 for node,row in result.items():
  assert node in blocks and row['slots'] and len(set(row['slots']))==len(row['slots'])
  assert all(0<=k<len(blocks[node]['successors']) for k in row['slots']), 'invalid inherited actual successor slot'
 return result

def closure_metric(anchor,after,manifest,projectors,declared=None):
 blocks={b['name']:b for b in after['blocks']};graph={n:[e['target'] for e in b['successors']] for n,b in blocks.items()}
 for n,p in projectors.items():graph[n]=[blocks[n]['successors'][k]['target'] for k in p['slots']]
 pc=family.pc_map(anchor)
 for n,v in family.pc_map(after).items():assert n not in pc or pc[n]==v;pc[n]=v
 def signature(sw):return (tuple(sorted(closure.int_value(c['value']) for c in sw['cases'])),('exit_pc',pc[sw['default']]) if sw['default'] in pc else ('default',sw['default']))
 old_signature,old_classify=closure.signature,closure.classify
 try:
  closure.signature=signature;closure.classify=family.audit.classify_guarded
  group,_=family.audit.classify_guarded(anchor,manifest)
  central=sorted(s['block'] for s in after['switches'] if signature(s)==group)
  full=[s for s in after['switches'] if signature(s)[0]==group[0]]
  assert central and all(s['default'] in pc for s in full), 'missing central family or unknown full-family default'
  if declared is not None:family.assert_central_coverage(central,declared)
  admission=closure.admission(after,manifest);reachable=closure.walk(graph,admission['success']);remaining=sorted(set(central)&reachable.keys())
  fast={n for n,p in projectors.items() if p['kind']=='fast'};scalar={n for n,p in projectors.items() if p['kind']=='scalar'};cycles=[]
  for component in closure.components(graph,set(reachable)):
   if (len(component)>1 or component[0] in graph[component[0]]) and (fast|scalar).intersection(component):cycles.append(dict(blocks=sorted(component),validated_fast_nodes=sorted(fast.intersection(component)),validated_scalar_nodes=sorted(scalar.intersection(component))))
 finally:closure.signature,closure.classify=old_signature,old_classify
 return dict(checked_context_guard_success_entry_closure=not remaining and bool(cycles),no_central_reentry=not remaining,central_blocks=central,remaining_central_blocks=remaining,central_witness_paths=[reachable[n] for n in remaining],fast_cycles=cycles,conditional_projectors=projectors,reachable_calls=[n for n in reachable if blocks[n]['calls']],full_family_source_pc_evidence=[dict(block=s['block'],default=s['default'],exit_pc=pc[s['default']],central=s['block'] in central) for s in full],all_ordinary_business_edges_retained=True,full_fallback_semantics_proved_before_projection=True)

def legacy(anchor,vbefore,after,manifest,hints):
 out={}
 for name,call in [('EQ_only',lambda:family.evaluate(anchor,after,manifest)),('V4_same_target',lambda:v4.metric(anchor,vbefore,after,manifest,hints))]:
  try:out[name]=dict(result=call(),failure=None)
  except (AssertionError,ValueError,KeyError) as e:out[name]=dict(result=None,failure=str(e) or type(e).__name__)
 return out

def run(plan,out):
 result=dict(schema='checked-context-sequential-chain-proof-v1',established=False,stage_order=ORDER,passes_executed=0,native_executions=0,scope='Conditional structural closure after ordered raw-bound V4/C59/parser/Bit proofs. No feasibility, memory immutability, source range, whole-CFF completion or observed native iteration claim.')
 stage='plan'
 try:
  evidence_namespace(out);initial_stamps=input_stamps(plan);inv=validate_plan(plan);result['plan']=plan;result['auditor_source_pins']={str(p):checked.sha(p) for p in sorted(SOURCE_FILES)}
  function=inv['v_before']['function'];assert all(inv[r]['function']==function for r in ROLES), 'frame selected function identity changed'
  manifest=json.loads(Path(plan['manifest']['path']).read_text());h=json.loads(Path(plan['hints']['path']).read_text());hints=[] if h is None else h['hints']
  stage='anchor-raw-binding';result['anchor_raw_binding']=bind_anchor(plan,inv['anchor'],out)
  stage='all-pass-frame-raw-bindings';t=plan['tools'];result['actual_LL_JSON_frame_bindings']=checked.bind_frames(t['inventory']['path'],t['runtime'],t['runtime_sha256'],function,[(r,plan['frames'][r]['ll'],plan['frames'][r]['json']) for r in ROLES if r!='anchor'],out)
  stage='V4';result['V4_module_identity']=outside_stage(plan,'v_before','v_after','V4',out,True);prior=v4.metric(inv['anchor'],inv['v_before'],inv['v_after'],manifest,hints);result['initial_actual_V4_metric']=prior;result['V4_proof_scope']='Unchanged frozen V4 guard/path/PHI checks plus whole-module outside identity. This is not a new general proof of every unrelated V-stage instruction; complete ordinary-edge/value copy proofs apply to C59 and Bit60.';p=initial_projectors(inv['v_after'],prior)
  stage='C59';result['C59_module_identity']=outside_stage(plan,'v_after','c59_after','C59',out);proof=checked.Proof(inv['v_after'],inv['c59_after']).run();result['C59_copy_proof']=proof;p=propagate(inv['v_after'],inv['c59_after'],proof,p,'C59');result['C59_conditional_metric']=closure_metric(inv['anchor'],inv['c59_after'],manifest,p)
  stage='C59-to-Bit-parse';result['exact_whole_module_import_bridge']=bind_bridge(plan,out)
  stage='Bit60';result['Bit60_module_identity']=outside_stage(plan,'bit_before','bit_after','Bit60',out);proof=checked.Proof(inv['bit_before'],inv['bit_after']).run();result['Bit60_copy_proof']=proof;p=propagate(inv['bit_before'],inv['bit_after'],proof,p,'Bit60');result['final_conditional_metric']=closure_metric(inv['anchor'],inv['bit_after'],manifest,p,plan.get('declared_central'))
  result['legacy_final_metrics']=legacy(inv['anchor'],inv['v_before'],inv['bit_after'],manifest,hints)
  stage='final-input-recheck';validate_plan(plan);assert input_stamps(plan)==initial_stamps, 'input/tool/runtime stat changed during proof'
  result['all_frame_spec_tool_runtime_pins_and_stats_unchanged_after_proof']=True;result['established']=True
 except (AssertionError,KeyError,ValueError,OSError,RuntimeError) as e:result['refusal']=dict(stage=stage,reason=str(e) or type(e).__name__)
 write(out,result);return result

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--plan',required=True);parser.add_argument('--out',required=True);a=parser.parse_args();sys.setrecursionlimit(20000);out=Path(a.out);assert not out.exists();r=run(json.loads(Path(a.plan).read_text()),out);print(json.dumps(dict(established=r['established'],refusal=r.get('refusal'),closure=r.get('final_conditional_metric',{}).get('checked_context_guard_success_entry_closure'),passes_executed=0,native_executions=0)));sys.exit(0 if r['established'] else 1)
if __name__=='__main__':main()
