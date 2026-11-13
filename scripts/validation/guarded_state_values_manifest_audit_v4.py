#!/usr/bin/env python3
"""Instruction-based state guard audit; preserve the prior target-only metric."""
import copy,json,re,sys
from collections import Counter,deque
from pathlib import Path
import guarded_loop_family_audit as family

def tables(inv):
 blocks={b['name']:b for b in inv['blocks']}
 rows={b['block']:b['instructions'] for b in inv['state_value_structure']}
 assert len(blocks)==len(inv['blocks']) and len(rows)==len(inv['state_value_structure'])
 assert set(blocks)==set(rows), 'instruction/CFG block coverage differs'
 defs={}
 for b,items in rows.items():
  for i in items:
   if i['opcode'] in ('store','br','switch','ret','unreachable') or not i['operand'].startswith('%'):continue
   assert i['operand'] not in defs, 'duplicate SSA definition'
   defs[i['operand']]=dict(i,block=b)
 return blocks,rows,defs

def block_operand(value):
 assert value['typed'].startswith('label '), 'not an actual block operand'
 name=value['operand'];assert name.startswith('%')
 return name[1:]

def validate_inventory(inv):
 """Bind every CFG slot to the instruction inventory before any projection."""
 blocks,rows,defs=tables(inv);switches={s['block']:s for s in inv['switches']}
 assert len(switches)==len(inv['switches'])
 incoming={n:Counter() for n in blocks}
 for name,b in blocks.items():
  items=rows[name];assert items
  term=items[-1];assert term['instruction']==b['terminator']
  operands=term['operands'];op=term['opcode']
  if op=='br':
   if 'condition' in term:
    assert len(operands)==3 and term['condition']==operands[0]['operand']
    expected=[block_operand(operands[2]),block_operand(operands[1])]
   else:assert len(operands)==1;expected=[block_operand(operands[0])]
  elif op=='switch':
   assert len(operands)>=2 and len(operands)%2==0
   expected=[block_operand(operands[1])]+[block_operand(operands[n]) for n in range(3,len(operands),2)]
   sw=switches[name];assert sw['selector']==operands[0]['operand'] and sw['default']==expected[0]
   assert len(sw['cases'])==len(expected)-1
   for n,c in enumerate(sw['cases']):assert c['value']==operands[2+2*n]['typed'] and c['target']==expected[n+1]
  else:assert op in ('ret','unreachable'), 'unsupported terminator';expected=[]
  assert [e['slot'] for e in b['successors']]==list(range(len(expected))), 'omitted or repeated CFG slot'
  assert [e['target'] for e in b['successors']]==expected, 'CFG edge differs from actual terminator'
  assert (op=='switch')==(name in switches)
  for child in expected:assert child in blocks;incoming[child][name]+=1
  assert b['calls']==[i['instruction'] for i in items if i['opcode']=='call']
  assert [v['instruction'] for v in b['stores']]==[i['instruction'] for i in items if i['opcode']=='store']
 for name,items in rows.items():
  for i in items:
   if i['opcode']=='phi':assert Counter(v['block'] for v in i['incoming'])==incoming[name], 'incomplete actual PHI edge coverage'
 # Recompute every scalar projection witness from actual SSA, rather than
 # accepting a generated marker or cached "same_value" booleans as proof.
 for g in inv['same_value_guards']:
  term=rows[g['block']][-1];assert term['opcode']=='br' and 'condition' in term
  eq=defs[term['condition']];assert eq['opcode']=='icmp' and eq['predicate']=='eq' and len(eq['operands'])==2
  load=defs[eq['operands'][0]['operand']];assert load['opcode']=='load' and load['instruction']==g['load'] and load['block']==g['load_block']
  assert eq['operands'][1]['typed']==g['candidate']
  edges=blocks[g['block']]['successors'];assert [e['target'] for e in edges]==[g['success'],g['fallback']]
  fallback=switches[g['fallback']];assert fallback['selector']==load['operand']
  candidates=[c for c in fallback['cases'] if c['value']==g['candidate']]
  assert len(candidates)==1 and candidates[0]['target']==g['success']
  assert all(g[k] for k in ('same_value_full_switch','fallback_has_candidate_case','fallback_candidate_matches_success'))
 return blocks,rows,defs

def selected_selector(definitions,value,pred,fallback_block=None):
 """One edge PHI in this fallback, never a historical predecessor traversal."""
 definition=definitions.get(value)
 if definition and definition['opcode']=='phi':
  assert fallback_block is not None and definition['block']==fallback_block, 'selector PHI is outside actual fallback'
  incoming=[v['value'] for v in definition['incoming'] if v['block']==pred]
  assert incoming and len(set(incoming))==1, 'ambiguous edge selector PHI'
  value=incoming[0]
  assert not (value in definitions and definitions[value]['opcode']=='phi'), 'historical or nested selector PHI refused'
 return value

def predicate(name,a,b,width):
 mask=(1<<width)-1;a&=mask;b&=mask
 signed=lambda n:n-(1<<width) if n&(1<<(width-1)) else n
 if name in ('sgt','sge','slt','sle'):a,b=signed(a),signed(b)
 return {'eq':a==b,'ne':a!=b,'ugt':a>b,'uge':a>=b,'ult':a<b,'ule':a<=b,'sgt':a>b,'sge':a>=b,'slt':a<b,'sle':a<=b}[name]

def literal(o):
 m=re.fullmatch(r'i([1-9][0-9]*) (-?[0-9]+|true|false)',o['typed']);assert m, 'nonliteral operand'
 return int(m[1]),int({'true':'1','false':'0'}.get(m[2],m[2]))

def bare(i):
 return {k:v for k,v in i.items() if k not in ('block',)}

def rhs(instruction):
 return re.sub(r'^\s*%(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+) = ', '', instruction).strip()

SSA=re.compile(r'%(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)')

def remap(text,mapping):
 return SSA.sub(lambda m:mapping.get(m[0],m[0]),text)

def clone_path(before,after,seed,selected,gate,guard):
 """Prove actual cloned operations and every selected/unselected edge slot.

 This deliberately refuses snapshot/bridge shapes rather than deriving their
 historical meaning from names. All admitted roots/PHIs observe actual edges.
 """
 bb,bi,bd=tables(before);ab,ai,ad=tables(after);old_names=set(bb)
 source=bb[seed]['successors'][selected]['target'];fresh=ab[gate]['successors'][0]['target'];pairs=[];seen=set()
 while True:
  assert source in bb and fresh in ab and fresh not in old_names and fresh not in seen
  seen.add(fresh);assert len(seen)<=32
  if fresh==guard:
   assert bi[source][-1]['opcode']=='switch';pairs.append((source,fresh,None));break
  assert bi[source][-1]['opcode']==ai[fresh][-1]['opcode'] and ai[fresh][-1]['opcode'] in ('br','switch')
  old_edges=bb[source]['successors'];new_edges=ab[fresh]['successors'];assert len(old_edges)==len(new_edges)
  forwarded=[n for n,e in enumerate(new_edges) if e['target'] not in old_names]
  assert len(forwarded)==1, 'ambiguous clone continuation or historical snapshot'
  slot=forwarded[0]
  for n,(old_edge,new_edge) in enumerate(zip(old_edges,new_edges)):
   assert old_edge['slot']==new_edge['slot']==n
   if n!=slot:assert old_edge['target']==new_edge['target'], 'unselected original edge changed'
  pairs.append((source,fresh,slot));source=old_edges[slot]['target'];fresh=new_edges[slot]['target']
 assert len({s for s,_,_ in pairs})==len(pairs), 'historical loop path refused'
 source_names={s for s,_,_ in pairs};mapping={};predecessor=seed
 for source,fresh,slot in pairs:
  old=bi[source];new=ai[fresh]
  for phi in (i for i in old if i['opcode']=='phi'):
   values=[v['value'] for v in phi['incoming'] if v['block']==predecessor]
   assert values and len(set(values))==1, 'missing or inconsistent actual PHI edge'
   value=values[0]
   if predecessor==seed and value in bd:assert bd[value]['block'] not in source_names, 'historical root snapshot refused'
   mapping[phi['operand']]=mapping.get(value,value)
  assert all(i['opcode']!='phi' for i in new), 'unexpected clone PHI/snapshot'
  old_body=[i for i in old[:-1] if i['opcode']!='phi'];new_body=new[:-2] if fresh==guard else new[:-1]
  assert len(old_body)==len(new_body), 'clone effects or definitions omitted/duplicated'
  for original,copy in zip(old_body,new_body):
   assert original['opcode']==copy['opcode'] and original.get('predicate')==copy.get('predicate')
   assert remap(rhs(original['instruction']),mapping)==rhs(copy['instruction']), 'cloned opcode/flags/operands differ'
   if original['operand'].startswith('%'):mapping[original['operand']]=copy['operand']
  if fresh!=guard:
   original_term,copy_term=old[-1],new[-1]
   if original_term['opcode']=='br':assert original_term.get('condition') is None or mapping.get(original_term['condition'],original_term['condition'])==copy_term.get('condition')
   else:
    original_switch=next(s for s in before['switches'] if s['block']==source)
    copy_switch=next(s for s in after['switches'] if s['block']==fresh)
    assert mapping.get(original_switch['selector'],original_switch['selector'])==copy_switch['selector']
    assert [c['value'] for c in original_switch['cases']]==[c['value'] for c in copy_switch['cases']]
  predecessor=source
 # Every added side-exit PHI input carries this execution's mapped value.
 # Original incoming pairs are checked separately after all seed guards pass.
 def exits(original,copy,targets):
  for target,count in Counter(targets).items():
   actual={i['operand']:i for i in ai[target] if i['opcode']=='phi'}
   for phi in (i for i in bi[target] if i['opcode']=='phi'):
    values=[v['value'] for v in phi['incoming'] if v['block']==original]
    assert values and len(set(values))==1
    expected=mapping.get(values[0],values[0]);observed=[v['value'] for v in actual[phi['operand']]['incoming'] if v['block']==copy]
    assert observed==[expected]*count, 'new exit PHI value/slot differs from selected execution'
 for source,fresh,slot in pairs[:-1]:
  exits(source,fresh,[e['target'] for n,e in enumerate(bb[source]['successors']) if n!=slot])
 source,fresh,_=pairs[-1];good,fallback=[e['target'] for e in ab[fresh]['successors']]
 exits(source,fresh,[good]);exits(source,fallback,[e['target'] for e in bb[source]['successors']])
 return pairs,mapping

def guard_witness(before,after,hint,strict_effects=True,seed_fallback_map=None,check_phis=True):
 bb,bi,bd=tables(before);ab,ai,ad=tables(after);seed=hint['block'];old=bi[seed][-1]
 assert old['opcode']=='br' and 'condition' in old
 cmp=bd[old['condition']];assert cmp['opcode']=='icmp' and len(cmp['operands'])==2
 ops=cmp['operands'];constants=[]
 for n,o in enumerate(ops):
  try:w,k=literal(o);constants.append((n,w,k))
  except AssertionError:pass
 assert len(constants)==1;constant_operand,width,k=constants[0];state=ops[1-constant_operand]['operand'];prediction=int(hint['state'],16)
 assert 0<=prediction<(1<<width)
 selected=0 if predicate(cmp['predicate'],k if constant_operand==0 else prediction,prediction if constant_operand==0 else k,width) else 1
 branch=ai[seed][-1];assert branch['opcode']=='br' and 'condition' in branch
 frozen=ad[branch['condition']];assert frozen['opcode']=='freeze' and len(frozen['operands'])==1 and frozen['operands'][0]['typed'].startswith('i1 '), 'new branch must use one frozen i1 equality'
 eq=ad[frozen['operands'][0]['operand']];assert eq['opcode']=='icmp' and eq['predicate']=='eq' and len(eq['operands'])==2
 assert eq['operands'][0]['operand']==state and literal(eq['operands'][1])==(width,prediction), 'not the original state SSA equality'
 assert state in ad and bare(ad[state])==bare(bd[state]), 'state definition changed or reread'
 assert old['condition'] in ad and bare(ad[old['condition']])==bare(cmp), 'original icmp changed'
 success,fallback=[e['target'] for e in ab[seed]['successors']]
 fallbranch=ai[fallback][-1];assert fallbranch['opcode']=='br' and fallbranch.get('condition')==old['condition']
 assert [e['target'] for e in ab[fallback]['successors']]==[e['target'] for e in bb[seed]['successors']], 'original conditional direction or slots changed'
 assert [i for i in ai[fallback] if i['opcode']!='phi']==[fallbranch], 'fallback unexpectedly repeats effects'
 assert eq['block']==frozen['block']==seed and ai[seed][-3]['operand']==eq['operand'] and ai[seed][-2]['operand']==frozen['operand']
 if strict_effects:assert ai[seed][:-3]==bi[seed][:-1], 'seed effects/definitions were duplicated or removed'
 # Original successor PHIs retain every original edge, including duplicates.
 # Core-generated extra edges may be appended, but no original pair may vanish.
 phi_checked=0
 for target in (set(e['target'] for e in bb[seed]['successors']) if check_phis else []):
  source_phis=[i for i in bi[target] if i['opcode']=='phi'];actual={i['operand']:i for i in ai[target] if i['opcode']=='phi'}
  for phi in source_phis:
   assert phi['operand'] in actual
   renamed=dict(seed_fallback_map or {});renamed[seed]=fallback
   expected=Counter((renamed.get(v['block'],v['block']),v['value']) for v in phi['incoming'])
   present=Counter((v['block'],v['value']) for v in actual[phi['operand']]['incoming'])
   assert all(present[x]>=n for x,n in expected.items()), 'original PHI slot/value omitted'
   phi_checked+=1
 # The new gate is real unconditional flow into a copied selected-direction tail.
 assert ai[success][-1]['opcode']=='br' and 'condition' not in ai[success][-1]
 assert len(ab[success]['successors'])==1
 # Find an actual fresh LOAD equality reachable from the true state edge without
 # traversing original blocks, old-state fallback, or trusting generated tags.
 old_blocks=set(bb);queue=deque([success]);seen=set();target_guards=[]
 while queue:
  name=queue.popleft()
  if name in seen or name in old_blocks:continue
  seen.add(name);assert len(seen)<=4096
  term=ai[name][-1]
  if term['opcode']=='br' and 'condition' in term:
   c=ad.get(term['condition'])
   if c and c['opcode']=='icmp' and c['predicate']=='eq':
    load=ad.get(c['operands'][0]['operand'])
    if load and load['opcode']=='load' and load['block']==name and load.get('simple'):
     try:_,target=literal(c['operands'][1])
     except AssertionError:target=None
     if target==int(hint['target'],16):target_guards.append((name,load))
  for edge in ab[name]['successors']:queue.append(edge['target'])
 assert len(target_guards)==1, 'missing or ambiguous fresh target guard'
 guard,load=target_guards[0];good,bad=[e['target'] for e in ab[guard]['successors']]
 switches={s['block']:s for s in after['switches']};full=switches[bad]
 assert selected_selector(ad,full['selector'],guard,bad)==load['operand']
 assert any(int(c['value'].split()[-1])==int(hint['target'],16) and c['target']==good for c in full['cases'])
 # Recursively retain the existing small-switch/default-to-full-switch chain.
 chain=[];previous=guard;current=bad;selected_value=load['operand'];seen_chain=set()
 old_switches={s['block']:s for s in before['switches']}
 while current in switches:
  assert current not in seen_chain;seen_chain.add(current);sw=switches[current]
  selector=sw['selector']
  selector=selected_selector(ad,selector,previous,current)
  assert selector==selected_value, 'recursive fallback selects a different read'
  matches=[s for s in old_switches.values() if s['cases']==sw['cases'] and s['default']==sw['default']]
  assert matches, 'fallback cases/default differ from the frozen original switch'
  assert not ab[current]['calls'] and not ab[current]['stores']
  chain.append(dict(block=current,cases=len(sw['cases']),default=sw['default'],same_selected_load=True))
  if sw['default'] not in switches:break
  previous,current=current,sw['default']
 assert chain and chain[-1]['cases']>=chain[0]['cases']
 path,mapping=clone_path(before,after,seed,selected,success,guard)
 source_end=next(s for s in before['switches'] if s['block']==path[-1][0])
 assert mapping.get(source_end['selector'],source_end['selector'])==load['operand']
 assert source_end['cases']==full['cases'] and source_end['default']==full['default'], 'fallback differs from this exact selected source switch'
 return dict(seed=seed,state=state,width=width,prediction=prediction,original_predicate=cmp['instruction'],selected_original_slot=selected,selected_original_target=bb[seed]['successors'][selected]['target'],state_success=success,state_fallback=fallback,checked_original_phis=phi_checked,target_guard=guard,target_load=load['instruction'],target_success=good,target_fallback_chain=chain,verified_new_blocks=sorted(seen),selected_clone_path=[dict(original=s,clone=c,selected_slot=n) for s,c,n in path],frozen_i1=frozen['operand'])

def projected(before,after,hints):
 validate_inventory(before);validate_inventory(after)
 # Every renamed original predecessor must itself be validated as a real state
 # equality guard by the corresponding independent witness below. The map only
 # accounts for interactions of separate original seed rewrites in one run.
 assert before['function']==after['function']
 assert all(h['function']==before['function'] for h in hints)
 assert len({h['block'] for h in hints})==len(hints), 'duplicate state hint'
 # Establish every original state EQ, its single i1 freeze, original direction,
 # actual copied path, and exact same-read fallback before building the map.
 validated=[guard_witness(before,after,h,check_phis=False) for h in hints]
 renamed={w['seed']:w['state_fallback'] for w in validated}
 witnesses=[guard_witness(before,after,h,seed_fallback_map=renamed) for h in hints]
 result=copy.deepcopy(after);blocks={b['name']:b for b in result['blocks']}
 for w in witnesses:
  b=blocks[w['seed']];assert b['successors'][0]['target']==w['state_success'];b['successors']=b['successors'][:1]
 return result,witnesses
