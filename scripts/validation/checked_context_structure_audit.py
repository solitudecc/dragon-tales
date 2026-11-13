#!/usr/bin/env python3
"""Independent checked-corridor graph/SSA copy proof, not an execution oracle.

Correspondence is proposed from changed actual edge slots, then proved from all
instructions and CFG/PHI slots. Names/metadata/counters never certify a copy.
The original V4 metric remains unchanged; redirected fast targets require this
separate proof. Refusals are conservative, including ambiguous clone shapes.
"""
import argparse, copy, hashlib, json, re, sys, subprocess, os, tempfile
from collections import Counter, defaultdict, deque
from pathlib import Path
import guarded_state_values_manifest_audit_v4 as state

SSA=state.SSA

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def rhs(i):return state.rhs(i['instruction'])
def skeleton(text):return SSA.sub('%#',text)
def term_skeleton(i):
 text=rhs(i)
 for kind in ('dragon.guarded-transitions.generated','dragon.guarded-checked-corridors.generated'):
  text=re.sub(r', !'+re.escape(kind)+r' ![0-9]+','',text)
 return skeleton(text)
def phi_type(i):
 m=re.match(r'phi (i[1-9][0-9]*|ptr(?: addrspace\([0-9]+\))?) ',rhs(i));assert m, 'unsupported scalar PHI syntax'
 return m[1]
def nonphis(items):return [i for i in items if i['opcode']!='phi']
def phis(items):return [i for i in items if i['opcode']=='phi']
def incoming(phi,pred):
 v=[x['value'] for x in phi['incoming'] if x['block']==pred]
 assert v and len(set(v))==1, 'missing/inconsistent actual PHI predecessor value'
 return v[0]
def validate(inv):
 # V4's scalar target-only projection is deliberately not invoked: an actual
 # checked seed may now enter a proved equivalent copy, rather than old target.
 stripped=copy.copy(inv);stripped['same_value_guards']=[]
 b,i,d=state.validate_inventory(stripped)
 # Independently bind PHI incoming values to actual instruction operands too.
 for name,items in i.items():
  for x in phis(items):
   assert len(x['operands'])==len(x['incoming'])
   assert [v['operand'] for v in x['operands']]==[v['value'] for v in x['incoming']]
   assert [v['typed'] for v in x['operands']]==[v['typed'] for v in x['incoming']]
   ty=phi_type(x)
   for v in x['incoming']:assert v['typed']==ty+' '+v['value']
   rendered='phi '+ty+' '+', '.join('[ '+v['value']+', %'+v['block']+' ]' for v in x['incoming'])
   assert rhs(x)==rendered or rhs(x).startswith(rendered+', !'), 'printed PHI differs from actual incoming operands'
 return b,i,d

class Proof:
 def __init__(self,before,after):
  assert before['function']==after['function']
  self.before,self.after=before,after
  self.bb,self.bi,self.bd=validate(before);self.ab,self.ai,self.ad=validate(after)
  assert list(self.ab)[:len(self.bb)]==list(self.bb), 'original block prefix/order changed'
  self.bs={s['block']:s for s in before['switches']};self.asw={s['block']:s for s in after['switches']}
  self.pred={n:[] for n in self.ab}
  for b in self.ab.values():
   for e in b['successors']:self.pred[e['target']].append((b['name'],e['slot']))
  def reachable(blocks):
   seen=set();pending=[next(iter(blocks))]
   while pending:
    n=pending.pop()
    if n in seen:continue
    seen.add(n);pending.extend(e['target'] for e in blocks[n]['successors'])
   return seen
  self.reachable=reachable(self.ab);self.before_reachable=reachable(self.bb)
  self.positions={x['operand']:(b,n) for b,items in self.ai.items() for n,x in enumerate(items) if x['operand'] in self.ad}
  self.events=defaultdict(lambda:defaultdict(list));self.contexts=[];self.edge_origins={};self.changed={};self.used_new=set();self.snapshots=[]
  self.body_pairs=[];self.phi_pairs=[];self.eliminated_phi_arms=[];self.calls=0;self.stores=0;self.current_queries=0
  # Every original SSA definition remains a definition event at its actual
  # location. Extra occurrences are installed only after clone shape matching.
  for value,d in self.bd.items():
   assert value in self.ad, 'original definition was removed; unsupported proof shape'
   assert self.ad[value]['opcode']==d['opcode']
   b,n=self.positions[value];assert b==d['block']
   self.events[value][b].append((n,value))

 def discover(self):
  for b,old in self.bb.items():
   edges=self.ab[b]['successors'];assert len(edges)==len(old['successors']), 'ordinary successor slot removed'
   for n,(a,z) in enumerate(zip(old['successors'],edges)):
    assert a['slot']==z['slot']==n
    if a['target']!=z['target']:
     assert z['target'] not in self.bb, 'ordinary edge redirected to another original block'
     assert b in self.before_reachable and b in self.reachable, 'unreachable selected seed is unsupported'
     self.changed[(b,n)]=a['target'];self.contexts.append(self.path(b,n,a['target'],z['target']))
  assert self.used_new==set(self.ab)-set(self.bb), 'unexplained appended block/effect'
  # Every original CFG edge has its identity; each cloned side exit and full
  # fallback receives the exact corresponding original source edge identity.
  for b in self.bb:
   for e in self.ab[b]['successors']:
    if (b,e['slot']) not in self.changed:self.edge_origins[(b,e['slot'])]=(b,e['slot'])
  for c in self.contexts:
   for source,fresh,slot in c['path']:
    if slot is not None:
     for e in self.ab[fresh]['successors']:
      if e['slot']!=slot:self.edge_origins[(fresh,e['slot'])]=(source,e['slot'])
   terminal,last,_=c['path'][-1]
   for e in self.ab[c['fallback']]['successors']:self.edge_origins[(c['fallback'],e['slot'])]=(terminal,e['slot'])
   old=self.bs[terminal]
   for n,case in enumerate(self.asw[last]['cases'],1):
    origin=next(j+1 for j,x in enumerate(old['cases']) if x['value']==case['value'])
    self.edge_origins[(last,n)]=(terminal,origin)

 def path(self,seed,selected,source,entry):
  assert entry not in self.used_new
  snapshot=None;fresh=entry
  # A pure PHI+unconditional bridge is a snapshot only if it differs from the
  # source root's non-PHI body. Ambiguity is refused, not guessed from its name.
  entry_body=nonphis(self.ai[entry]);old_body=nonphis(self.bi[source])
  if len(entry_body)==1 and entry_body[0]['opcode']=='br' and 'condition' not in entry_body[0] and (len(old_body)!=1 or old_body[0]['opcode']!='br' or 'condition' in old_body[0]):
   assert phis(self.ai[entry]), 'empty unexplained entry bridge'
   snapshot=entry;fresh=self.ab[entry]['successors'][0]['target'];self.used_new.add(entry)
  root=fresh;pairs=[];seen=set();source_seen=set()
  while True:
   assert source in self.bb and fresh in self.ab and fresh not in self.bb and fresh not in seen and fresh not in self.used_new
   assert source not in source_seen and len(seen)<32, 'repeated/unbounded source corridor'
   seen.add(fresh);source_seen.add(source);self.used_new.add(fresh)
   old=self.bb[source]['successors'];new=self.ab[fresh]['successors']
   # Terminal: small actual-LOAD switch with full original same-read fallback.
   terminal=self.asw.get(fresh);full=self.asw.get(terminal['default']) if terminal else None
   source_sw=self.bs.get(source)
   if terminal and full and source_sw and full['cases']==source_sw['cases'] and full['default']==source_sw['default']:
    fallback=full['block'];assert fallback not in self.bb and fallback not in self.used_new
    self.used_new.add(fallback)
    assert len(nonphis(self.ai[fallback]))==1, 'fallback repeats source effects or LOAD'
    assert term_skeleton(self.bi[source][-1])==term_skeleton(self.ai[fallback][-1]), 'full switch opcode/flags/case literals differ'
    load=self.ad.get(terminal['selector']);assert load and load['opcode']=='load' and load.get('simple') and load['block']==fresh
    assert state.selected_selector(self.ad,full['selector'],fresh,fallback)==load['operand'], 'not SAME actual terminal LOAD'
    assert terminal.get('same_block_load') and terminal.get('simple_load') and terminal['load']==load['instruction']
    assert 0<len(terminal['cases'])<=16 and len({c['value'] for c in terminal['cases']})==len(terminal['cases'])
    expected={c['value']:c['target'] for c in source_sw['cases']};loop_slots=0
    for c in terminal['cases']:
     assert c['value'] in expected, 'fast non-case candidate'
     # Avoid conditional-expression ambiguity: source root is fixed below.
     original_root=pairs[0][0] if pairs else source
     wanted=root if expected[c['value']]==original_root else expected[c['value']]
     assert c['target']==wanted, 'fast target is neither exact source target nor its checked root'
     loop_slots+=wanted==root
    pairs.append((source,fresh,None));break
   assert self.bi[source][-1]['opcode']==self.ai[fresh][-1]['opcode'] and len(old)==len(new)
   onward=[n for n,e in enumerate(new) if e['target'] not in self.bb]
   assert len(onward)==1, 'ambiguous copied continuation/side exit'
   slot=onward[0]
   for n,(a,z) in enumerate(zip(old,new)):
    if n!=slot:assert a['target']==z['target'], 'ordinary copied side exit changed'
   pairs.append((source,fresh,slot));source=old[slot]['target'];fresh=new[slot]['target']
  result=dict(seed=seed,slot=selected,root=pairs[0][0],entry=entry,clone_root=root,snapshot=snapshot,path=pairs,fallback=fallback,loop_slots=loop_slots,terminal_load=load['operand'])
  return result

 def install(self):
  for c in self.contexts:
   root_old,root_new,_=c['path'][0];root_phis=phis(self.bi[root_old]);actual_phis=phis(self.ai[root_new]);mapping={}
   internal={source for source,_,_ in c['path']}
   history=[]
   for phi in root_phis:
    v=incoming(phi,c['seed'])
    if v in self.bd and self.bd[v]['block'] in internal and v not in history:history.append(v)
   snap_phis=phis(self.ai[c['snapshot']]) if c['snapshot'] else []
   assert len(snap_phis)==len(history), 'historical snapshot count differs'
   historical=dict(zip(history,[p['operand'] for p in snap_phis]))
   for var,phi in zip(history,snap_phis):
    assert Counter(x['block'] for x in phi['incoming'])==Counter([c['seed']])
    self.snapshots.append((var,phi['incoming'][0]['value'],c['seed'],phi['operand']))
   if c['loop_slots']:
    assert len(actual_phis)==len(root_phis), 'unexpected/ambiguous cyclic root PHIs'
    for old,new in zip(root_phis,actual_phis):
     assert phi_type(old)==phi_type(new), 'root PHI type changed'
     mapping[old['operand']]=new['operand'];self.events[old['operand']][root_new].append((self.positions[new['operand']][1],new['operand']))
     self.phi_pairs.append((old,new,c))
   else:
    assert not actual_phis, 'unexpected straight clone root PHIs'
    for phi in root_phis:
     v=incoming(phi,c['seed']);mapped=historical.get(v,v);mapping[phi['operand']]=mapped
     self.events[phi['operand']][root_new].append((-1,mapped))
     if v not in historical:self.eliminated_phi_arms.append((v,mapped,c['seed']))
   predecessor=c['seed']
   for n,(source,fresh,slot) in enumerate(c['path']):
    if n:
     assert not phis(self.ai[fresh]), 'unexpected intermediate cloned PHI'
     for phi in phis(self.bi[source]):
      v=incoming(phi,predecessor);mapped=mapping.get(v,v);mapping[phi['operand']]=mapped
      self.events[phi['operand']][fresh].append((-1,mapped))
      self.eliminated_phi_arms.append((v,mapped,c['path'][n-1][1]))
    old=nonphis(self.bi[source])[:-1];new=nonphis(self.ai[fresh])[:-1]
    assert len(old)==len(new), 'source effects/definitions omitted or duplicated'
    for original,actual in zip(old,new):
     assert original['opcode']==actual['opcode']
     if original['operand'] in self.bd:
      mapping[original['operand']]=actual['operand']
      self.events[original['operand']][fresh].append((self.positions[actual['operand']][1],actual['operand']))
     self.body_pairs.append((original,actual,fresh))
    predecessor=source
   c['mapping']=mapping
  for var,blocks in self.events.items():
   for b,events in blocks.items():events.sort()
  self.memo={};self.visiting=set()

 def current(self,var,value,block,point=None):
  if block not in self.reachable:
   assert value==var, 'unreachable source operand changed; conservatively refused'
   return
  if var not in self.bd:
   assert value==var, 'constant/argument/global changed';return
  if point is None:point=len(self.ai[block])
  key=(var,value,block,point)
  if key in self.memo:return
  if key in self.visiting:return # coinduction; every actual predecessor is checked
  self.current_queries+=1;assert self.current_queries<=1000000, 'SSA proof work bound exceeded'
  self.visiting.add(key)
  try:
   events=[(n,v) for n,v in self.events[var].get(block,[]) if n<point]
   if events:
    assert events[-1][1]==value, 'value is stale or from the wrong source execution: '+repr(key)+' current='+repr(events[-1])
   else:
    definition=self.ad.get(value)
    if definition and definition['opcode']=='phi' and definition['block']==block and value not in self.bd:
     # An actual edge PHI transports a value, but cannot invent or recursively
     # turn an old observation into a fresh source definition.
     for arm in definition['incoming']:self.current(var,arm['value'],arm['block'])
    else:
     predecessors=[(p,n) for p,n in self.pred[block] if p in self.reachable]
     assert predecessors, 'source value unavailable on an actual entry path: '+repr(key)
     for predecessor,_ in predecessors:self.current(var,value,predecessor)
   self.memo[key]=True
  finally:self.visiting.remove(key)

 def instruction(self,old,new,block,point):
  assert old['opcode']==new['opcode'] and old.get('predicate')==new.get('predicate')
  assert len(old['operands'])==len(new['operands'])
  for a,z in zip(old['operands'],new['operands']):
   assert skeleton(a['typed'])==skeleton(z['typed']), 'operand type/literal changed'
   self.current(a['operand'],z['operand'],block,point)
  assert skeleton(rhs(old))==skeleton(rhs(new)), 'source opcode/flags/metadata/literals changed'
  # Full token pairing also binds non-operand SSA references (e.g. named types)
  # that may appear in the printed instruction, rather than erasing them.
  known={(a['operand'],z['operand']) for a,z in zip(old['operands'],new['operands'])}
  for a,z in zip(SSA.findall(rhs(old)),SSA.findall(rhs(new))):assert a==z or (a,z) in known, 'unbound printed SSA identity'
  self.calls+=old['opcode']=='call';self.stores+=old['opcode']=='store'

 def effects(self):
  for b,items in self.bi.items():
   old=nonphis(items);new=nonphis(self.ai[b]);assert len(old)==len(new), 'ordinary effects were inserted/removed'
   for a,z in zip(old[:-1],new[:-1]):
    assert a['operand']==z['operand'], 'ordinary definition identity changed'
    self.instruction(a,z,b,self.ai[b].index(z))
   # Original terminator retains kind/condition/case constants. Only independently
   # discovered selected successor slots may enter their matching copies.
   a,z=old[-1],new[-1];assert a['opcode']==z['opcode'] and term_skeleton(a)==term_skeleton(z), 'ordinary terminator opcode/flags/literals changed'
   ao=[o for o in a['operands'] if not o['typed'].startswith('label ')]
   zo=[o for o in z['operands'] if not o['typed'].startswith('label ')]
   assert len(ao)==len(zo)
   for x,y in zip(ao,zo):
    assert skeleton(x['typed'])==skeleton(y['typed']);self.current(x['operand'],y['operand'],b,len(self.ai[b])-1)
  for old,new,b in self.body_pairs:self.instruction(old,new,b,self.ai[b].index(new))
  for c in self.contexts:
   mapping=c['mapping']
   for source,fresh,slot in c['path'][:-1]:
    a,z=self.bi[source][-1],self.ai[fresh][-1]
    assert term_skeleton(a)==term_skeleton(z), 'copied terminator opcode/flags/literals changed'
    ao=[o for o in a['operands'] if not o['typed'].startswith('label ')];zo=[o for o in z['operands'] if not o['typed'].startswith('label ')]
    assert len(ao)==len(zo)
    for x,y in zip(ao,zo):
     assert skeleton(x['typed'])==skeleton(y['typed']);self.current(x['operand'],y['operand'],fresh,len(self.ai[fresh])-1)
   source,fresh,_=c['path'][-1];self.current(self.bs[source]['selector'],self.asw[fresh]['selector'],fresh)
  for var,value,seed,snapshot in self.snapshots:self.current(var,value,seed)
  for var,value,pred in self.eliminated_phi_arms:self.current(var,value,pred)

 def phi_values(self):
  # Original PHIs retain one value for every remaining/added actual original
  # edge. Edge-origin mapping comes only from the proved copied terminators.
  for b,items in self.bi.items():
   actual={p['operand']:p for p in phis(self.ai[b])}
   for phi in phis(items):
    assert phi['operand'] in actual
    original={v['block']:incoming(phi,v['block']) for v in phi['incoming']}
    for pred,slot in self.pred[b]:
     origin=self.edge_origins.get((pred,slot));assert origin, 'unexplained ordinary PHI predecessor edge'
     source,source_slot=origin;assert self.bb[source]['successors'][source_slot]['target']==b
     self.current(original[source],incoming(actual[phi['operand']],pred),pred)
  for old,new,c in self.phi_pairs:
   expected=Counter([c['snapshot'] or c['seed']]+[c['path'][-1][1]]*c['loop_slots'])
   assert Counter(v['block'] for v in new['incoming'])==expected, 'cyclic root PHI actual edge multiplicity differs'
   initial=incoming(new,c['snapshot'] or c['seed']);source_initial=incoming(old,c['seed'])
   if c['snapshot']:
    snapshot=self.ad.get(initial)
    if source_initial in self.bd and self.bd[source_initial]['block'] in {s for s,_,_ in c['path']}:
     assert snapshot and snapshot['block']==c['snapshot'] and snapshot['opcode']=='phi'
     self.current(source_initial,incoming(snapshot,c['seed']),c['seed'])
    else:self.current(source_initial,initial,c['seed'])
   else:self.current(source_initial,initial,c['seed'])
   self.current(incoming(old,c['path'][-1][0]),incoming(new,c['path'][-1][1]),c['path'][-1][1])

 def run(self):
  self.discover();self.install();self.effects();self.phi_values()
  return dict(schema='checked-context-structural-proof-v1',function=self.before['function'],checked_context_proofs=[{k:v for k,v in c.items() if k!='mapping'} for c in self.contexts],all_original_blocks_preserved=True,all_actual_ordinary_edges_and_phi_slots_preserved=True,all_copied_opcodes_flags_operands_proved=True,unreachable_blocks_preserved_with_exact_source_operands=sorted(set(self.ab)-self.reachable),same_actual_load_full_fallback_proved=True,historical_snapshot_and_fresh_backedges_proved=True,liveout_edge_value_proof_queries=self.current_queries,source_definition_events=sum(len(e) for b in self.events.values() for e in b.values()),original_and_cloned_calls_checked=self.calls,original_and_cloned_stores_checked=self.stores,passes_executed=0,native_executions=0,scope='Read-only graph and SSA epoch copy proof; no range/memory immutability fact and no observed iteration claim. Full fallback and ordinary CFG are proved before any conditional guard-success metric.')

def module_identity(before,after,function):
 def outside(text):
  lines=text.splitlines(True);start=[n for n,l in enumerate(lines) if l.startswith('define ') and re.search(r'@'+re.escape(function)+r'\(',l)]
  assert len(start)==1, 'ambiguous function identity'
  first=start[0];end=next(n for n in range(first+1,len(lines)) if lines[n].strip()=='}')
  return ''.join(lines[:first+1]+lines[end:])
 assert outside(before)==outside(after), 'module globals/declarations/attributes/metadata identity changed'
 return dict(full_outside_function_bytes_preserved=True,metadata_payloads_preserved=True)

def bind_frames(tool,runtime,runtime_sha,function,frames,out):
 """Bind full JSON inventories back to independently parsed/verified raw IR.

 Positive temporary products are removed; any failed importer/difference stays
 in its separate directory, with its actual command and loader/runtime pins.
 """
 tool=Path(tool).resolve();runtime=Path(runtime).resolve();out=Path(out)
 assert sha(runtime/'libdragon.so')==runtime_sha, 'inventory runtime hash differs'
 manifest=json.loads((runtime/'MANIFEST.json').read_text());assert manifest['files']['libdragon.so']['sha256']==runtime_sha
 env=dict(os.environ,LD_LIBRARY_PATH=str(runtime));rows=[]
 for phase,ll,inventory in frames:
  work=Path(tempfile.mkdtemp(prefix=out.name+'.'+phase+'-import-',dir=out.parent));expected=work/'inventory.json'
  command=[str(tool),str(ll),function,str(expected)]
  process=subprocess.run(command,capture_output=True,text=True,env=env)
  row=dict(phase=phase,command=command,exit=process.returncode,stdout=process.stdout,stderr=process.stderr,tool_sha256=sha(tool),runtime_sha256=runtime_sha,LD_LIBRARY_PATH=str(runtime),input_ll_sha256=sha(ll),actual_json_sha256=sha(inventory))
  row['exact_independent_parse_verify_inventory']=process.returncode==0 and expected.read_bytes()==Path(inventory).read_bytes()
  receipt=out.parent/(out.name+'.'+phase+'-IMPORT.json');assert not receipt.exists();receipt.write_text(json.dumps(row,indent=2)+'\n')
  if not row['exact_independent_parse_verify_inventory']:
   raise AssertionError('actual inventory differs from independently parsed raw IR; evidence '+str(work))
  expected.unlink();work.rmdir();rows.append(row)
 return rows

def main():
 p=argparse.ArgumentParser();p.add_argument('--before',required=True);p.add_argument('--after',required=True);p.add_argument('--source-anchor');p.add_argument('--before-ll',required=True);p.add_argument('--after-ll',required=True);p.add_argument('--inventory-tool',required=True);p.add_argument('--inventory-runtime',required=True);p.add_argument('--inventory-runtime-sha256',required=True);p.add_argument('--out',required=True);a=p.parse_args()
 sys.setrecursionlimit(20000)
 before=json.loads(Path(a.before).read_text());after=json.loads(Path(a.after).read_text());result=Proof(before,after).run()
 assert bool(a.before_ll)==bool(a.after_ll), 'both LL frames are required together'
 if a.before_ll:result['module_identity']=module_identity(Path(a.before_ll).read_text(),Path(a.after_ll).read_text(),before['function'])
 result['bindings']={n:{'path':getattr(a,n),'sha256':sha(getattr(a,n))} for n in ('before','after','before_ll','after_ll') if getattr(a,n)}
 if a.before_ll:result['actual_LL_JSON_frame_binding']=bind_frames(a.inventory_tool,a.inventory_runtime,a.inventory_runtime_sha256,before['function'],[('before',a.before_ll,a.before),('after',a.after_ll,a.after)],a.out)
 if a.source_anchor:result['source_anchor']={'path':a.source_anchor,'sha256':sha(a.source_anchor)}
 out=Path(a.out);assert not out.exists(), 'refuse to overwrite earlier proof';out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'proved_contexts':len(result['checked_context_proofs']),'all_actual_edges':True,'native_executions':0}))
if __name__=='__main__':main()
