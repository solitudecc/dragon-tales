#!/usr/bin/env python3
"""Conditional fast-CFG closure audit. This does not prove path feasibility."""
import json,sys
from collections import deque
from pathlib import Path

def int_value(s):return int(s.split()[1])
def signature(sw):return (tuple(sorted(int_value(c['value']) for c in sw['cases'])),sw['default'])
def graph_with_success_edges(inventory):
 blocks={b['name']:b for b in inventory['blocks']};graph={n:[e['target'] for e in b['successors']] for n,b in blocks.items()};kept=[];invalid=[]
 for g in inventory['same_value_guards']:
  n=g['block'];edges=blocks[n]['successors']
  if not (g['same_value_full_switch'] and g['fallback_has_candidate_case'] and g['fallback_candidate_matches_success'] and len(edges)==2 and edges[0]['target']==g['success'] and edges[1]['target']==g['fallback']):
   invalid.append(n);continue
  graph[n]=[g['success']];kept.append(g)
 return blocks,graph,kept,invalid

def walk(graph,start):
 paths={start:[start]};queue=deque([start])
 while queue:
  n=queue.popleft()
  for child in graph[n]:
   if child not in paths:paths[child]=paths[n]+[child];queue.append(child)
 return paths

def components(graph,nodes):
 # Iterative Kosaraju; no Python recursion-dependent coverage limit.
 visited=set();order=[]
 for start in nodes:
  if start in visited:continue
  visited.add(start);stack=[(start,0)]
  while stack:
   n,i=stack[-1];successors=graph[n]
   if i==len(successors):order.append(n);stack.pop();continue
   stack[-1]=(n,i+1);child=successors[i]
   if child in nodes and child not in visited:visited.add(child);stack.append((child,0))
 reverse={n:[] for n in nodes}
 for n in nodes:
  for child in graph[n]:
   if child in nodes:reverse[child].append(n)
 visited=set();result=[]
 for start in reversed(order):
  if start in visited:continue
  group=[];stack=[start];visited.add(start)
  while stack:
   n=stack.pop();group.append(n)
   for child in reverse[n]:
    if child not in visited:visited.add(child);stack.append(child)
  result.append(group)
 return result

def classify(prepared,manifest):
 ranges=[(int(row['offset'],16),int(row['offset'],16)+len(bytes.fromhex(row['bytes']))) for row in manifest['memory']]
 groups={signature(sw) for sw in prepared['switches'] if sw.get('same_block_load') and 'address_decimal' in sw and any(lo<=int(sw['address_decimal']) and int(sw['address_decimal'])+8<=hi for lo,hi in ranges)}
 if len(groups)!=1:raise ValueError('CFF case/default identity not uniquely witnessed by actual manifest-covered selector LOADs')
 group=next(iter(groups));originals=[sw for sw in prepared['switches'] if signature(sw)==group]
 return group,originals

def admission(inventory,manifest):
 rows=sorted(manifest['memory'],key=lambda x:int(x['offset'],16));last=rows[-1];data=bytes.fromhex(last['bytes']);assert len(data)%8==0
 address=int(last['offset'],16)+len(data)-8;constant=int.from_bytes(data[-8:],'little');blocks={b['name']:b for b in inventory['blocks']};matches=[]
 for check in inventory['equality_checks']:
  if not(check.get('canonical_guest_base') and check.get('address_decimal')==str(address) and int_value(check['constant'])==constant):continue
  failure=blocks[check['failure']]
  if failure['call_targets']==[manifest['root_symbol']]:matches.append(check)
 if len(matches)!=1:raise ValueError('last actual entry table equality/root fallback not unique')
 return matches[0]

def evaluate(prepared,final,manifest):
 group,originals=classify(prepared,manifest);entry=admission(final,manifest)
 blocks,graph,guards,invalid=graph_with_success_edges(final)
 if invalid:raise ValueError('unverified claimed transition guards: '+str(invalid))
 central={sw['block'] for sw in final['switches'] if signature(sw)==group}
 reachable=walk(graph,entry['success']);remaining=sorted(central&reachable.keys())
 guard_nodes={g['block'] for g in guards};sccs=components(graph,set(reachable));cycles=[c for c in sccs if (len(c)>1 or c[0] in graph[c[0]]) and guard_nodes.intersection(c)]
 dynamic=[s for s in originals if 'address_decimal' not in s]
 # Case-value signatures are equal; prefer the original dynamic representative.
 representative=dynamic[0] if dynamic else originals[0]
 coverage=[]
 for case in representative['cases']:
  paths=walk(graph,case['target']);hits=sorted(central&paths.keys())
  coverage.append({'case':case['value'],'target':case['target'],'central_reentry':hits,'witnesses':[paths[h] for h in hits],'calls':[n for n in paths if blocks[n]['calls']]})
 targets={int_value(c['value']):c['target'] for c in representative['cases']};table=[]
 for row in manifest['memory']:
  data=bytes.fromhex(row['bytes']);start=int(row['offset'],16)
  for off in range(0,len(data)-7,8):
   value=int.from_bytes(data[off:off+8],'little');target=targets.get(value);hits=[]
   if target is not None:hits=sorted(central&walk(graph,target).keys())
   table.append({'address':start+off,'hint':value,'decoded_case_target':target,'central_reentry':hits,'reachability_claim':False})
 return {'admission_check':entry,'actual_guard_count':len(guards),'guard_success_entry_closure':not remaining and bool(cycles),'no_central_reentry':not remaining,'fast_cycle_sccs':cycles,'reachable_original_or_fallback_CFF_switches':remaining,'central_witness_paths':[reachable[h] for h in remaining],'reachable_calls':[n for n in reachable if blocks[n]['calls']],'business_switches_retained':[sw for sw in final['switches'] if signature(sw)!=group],'decoded_CFF_case_coverage':coverage,'table_hint_address_coverage':table,'limits':'Only actual proved comparison-failure edges were removed. All ordinary condition edges and natural business switch cases/default remain. Calls are traversed and listed, not treated as exits. This is conditional structural closure, not path-feasibility proof, reachability of every hint, or evidence every runtime guard succeeds.'}

def self_test():
 def block(n,edges):return {'name':n,'successors':[{'slot':i,'target':x} for i,x in enumerate(edges)]}
 inv={'blocks':[block('g',['body','fallback']),block('body',['g','central']),block('fallback',['central']),block('central',[])],'same_value_guards':[{'block':'g','success':'body','fallback':'fallback','same_value_full_switch':True,'fallback_has_candidate_case':True,'fallback_candidate_matches_success':True}]}
 _,graph,_,_=graph_with_success_edges(inv)
 assert graph['g']==['body'] and graph['body']==['g','central'];assert 'central' in walk(graph,'g')
 inv['blocks'][1]['successors']=[{'slot':0,'target':'g'}];_,graph,_,_=graph_with_success_edges(inv);assert 'central' not in walk(graph,'g')
 inv['same_value_guards'][0]['fallback_candidate_matches_success']=False;_,graph,_,bad=graph_with_success_edges(inv);assert bad and 'central' in walk(graph,'g')
 # A natural business switch has no valid guard record: all its cases stay.
 inv['same_value_guards'][0]['fallback_candidate_matches_success']=True;inv['blocks'][1]['successors']=[{'slot':0,'target':'g'},{'slot':1,'target':'central'}];_,graph,_,_=graph_with_success_edges(inv);assert 'central' in walk(graph,'g')
 cases=[{'value':'i64 7','target':'body'}]
 prepared={'switches':[{'block':'constant','cases':cases,'default':'cff-exit','same_block_load':True,'address_decimal':'16'},{'block':'central','cases':cases,'default':'cff-exit','same_block_load':True},{'block':'business','cases':cases,'default':'business-exit','same_block_load':True}]}
 _,originals=classify(prepared,{'memory':[{'offset':'0x10','bytes':'0700000000000000'}]});assert [x['block'] for x in originals]==['constant','central']
 return {'synthetic_graph_controls':5,'native_executions':0}
if __name__=='__main__':
 if sys.argv[1:]==['--self-test']:print(json.dumps(self_test()));raise SystemExit(0)
 if len(sys.argv)!=5:raise SystemExit('prepared.json final.json manifest.json output.json')
 prepared,final,manifest=[json.loads(Path(p).read_text()) for p in sys.argv[1:4]]
 Path(sys.argv[4]).write_text(json.dumps(evaluate(prepared,final,manifest),indent=2)+'\n')
