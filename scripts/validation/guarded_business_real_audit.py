#!/usr/bin/env python3
"""Independent saved-IR checks for the unchanged full-state business B replay."""
import copy,json,sys
from pathlib import Path
import guarded_business_closure as closure
ROOT=Path(sys.argv[1])
WITNESSES=json.loads((ROOT/'business-store-witness-recheck.json').read_text())['rows']

def validate(before,after,manifest,stats):
 switches={s['block']:s for s in after['switches']};guards=[g for g in after['same_value_guards'] if g['generated']];blocks={b['name']:b for b in after['blocks']}
 assert len(guards)==stats['rewritten_edges']==stats['fallback_switches_created']
 assert len(after['blocks'])-len(before['blocks'])==stats['cloned_blocks']
 candidates=set();ranges=[]
 for row in manifest['memory']:
  data=bytes.fromhex(row['bytes']);ranges.append((int(row['offset'],16),int(row['offset'],16)+len(data)))
  candidates.update(int.from_bytes(data[n:n+8],'little') for n in range(0,len(data)-7,8))
 eligible=[s for s in before['switches'] if s.get('same_block_load') and 'address_decimal' in s and any(a<=int(s['address_decimal']) and int(s['address_decimal'])+8<=b for a,b in ranges)]
 for old in eligible:assert old['block'] in switches and switches[old['block']]['cases']==old['cases'] and switches[old['block']]['default']==old['default']
 for g in guards:
  assert g['same_value_full_switch'] and g['fallback_has_candidate_case'] and g['fallback_candidate_matches_success']
  value=closure.int_value(g['candidate']);assert value in candidates
  fallback=switches[g['fallback']];assert fallback['load_block']==g['block']==g['load_block']
  assert any(c['target']==g['success'] and closure.int_value(c['value'])==value for c in fallback['cases'])
  assert any(s['cases']==fallback['cases'] and s['default']==fallback['default'] for s in before['switches'])
 # Every original constant selector opportunity has a preserved matching real guard.
 for old in eligible:
  assert any(switches[g['fallback']].get('address_decimal')==old['address_decimal'] and switches[g['fallback']]['cases']==old['cases'] and switches[g['fallback']]['default']==old['default'] for g in guards)
 return blocks,guards,eligible

rows=[];controls=0
for case in ('f-cff','f-combo'):
 for level in ('O0','O2'):
  tail=ROOT/'candidate/guarded-transition-tail'/case/level;report=json.loads((tail/'report.json').read_text());manifest=json.loads((ROOT/'candidate/guarded-views'/case/level/'manifest.json').read_text());data=ROOT/'structural-review'/f'{case}-{level}'
  before=json.loads((data/'prepared.json').read_text());after=json.loads((data/'after.json').read_text());stats=report['controller_report']['runs'][0]['counters']
  assert report['total_pass_runs']==31 and report['additional_pass_runs']==6 and report['inherited_pass_runs']==25
  assert report['controller_options']==dict(max_candidates=64,max_rewrites=64,max_blocks=512,max_instructions=16384,business_corridors=1)
  blocks,guards,eligible=validate(before,after,manifest,stats)
  for corruption in ('candidate','fallback'):
   bad=copy.deepcopy(after);g=next(g for g in bad['same_value_guards'] if g['generated'])
   if corruption=='candidate':g['candidate']='i64 -123'
   else:next(s for s in bad['switches'] if s['block']==g['fallback'])['cases']=[]
   try:validate(before,bad,manifest,stats)
   except AssertionError:controls+=1
   else:raise AssertionError('malformed guard/fallback accepted')
  witnesses=[]
  if (case,level)==('f-cff','O0'):
   old={b['name']:b for b in before['blocks']};guardmap={g['block']:g for g in guards}
   for w in WITNESSES:
    assert old[w['seed']]['successors'][w['slot']]['target']==w['path'][0]
    assert any(s['value']==f"i64 {w['state']}" for s in old[w['seed']]['stores'])
    target=blocks[w['seed']]['successors'][w['slot']]['target'];assert target!=w['path'][0]
    found=[];seen=set();queue=[target]
    while queue:
     n=queue.pop()
     if n in seen or not blocks[n]['generated']:continue
     seen.add(n)
     if n in guardmap:found.append(guardmap[n]);continue
     queue.extend(e['target'] for e in blocks[n]['successors'])
    assert len(found)==1
    content=bytes.fromhex(manifest['memory'][0]['bytes']);expected=int.from_bytes(content[w['state']*8:(w['state']+1)*8],'little');assert closure.int_value(found[0]['candidate'])==expected
    witnesses.append(dict(seed=w['seed'],slot=w['slot'],stored_value=w['state'],original_path=w['path'],new_target=target,actual_guard=found[0]))
   assert len(witnesses)==25 and stats['rewritten_edges']==49 and stats['cloned_blocks']==290
  c=closure.evaluate(before,after,manifest)
  rows.append(dict(case=case,level=level,counters=stats,original_terminal_opportunities=len(eligible),actual_guards=guards,original_store_edges=witnesses,closure=c))
result=dict(rows=rows,structural_fault_controls=controls,graph_control_selftest=closure.self_test(),native_executions=0,limits='Saved IR audit does not add native executions. Conditional closure failure stays recorded; all ordinary branches and natural business decoded cases remain.')
(ROOT/'structural-review/REVIEW.json').write_text(json.dumps(result,indent=2)+'\n')
print([(x['case'],x['level'],len(x['actual_guards']),len(x['original_store_edges']),x['closure']['guard_success_entry_closure']) for x in rows]);print('structural controls',controls)
