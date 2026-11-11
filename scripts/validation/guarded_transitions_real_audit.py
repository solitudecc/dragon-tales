from pathlib import Path
from collections import deque
import copy,json,re,sys
ROOT=Path(sys.argv[1])
def integer(s):return int(s.split()[1])
def candidates(before,manifest):
 memory={}
 for row in manifest['memory']:
  start=int(row['offset'],16)
  for n,b in enumerate(bytes.fromhex(row['bytes'])):memory[start+n]=b
 eligible=[]; rejected=[]
 for sw in before['switches']:
  reason=None;hint=None
  if not sw.get('same_block_load') or not sw.get('simple_load'):reason='not same-block simple LOAD'
  elif 'address_decimal' not in sw:reason='dynamic index'
  else:
   width=int(re.search(r'load i(\d+)',sw['load'])[1]);a=int(sw['address_decimal']);data=[memory.get(a+n) for n in range(width//8)]
   if any(x is None for x in data):reason='no complete hint bytes'
   else:
    hint=int.from_bytes(bytes(data),'little')
    if hint not in [integer(c['value']) for c in sw['cases']]:reason='hint not an original case'
  if reason:rejected.append({'block':sw['block'],'reason':reason,'hint':hint});continue
  eligible.append({'switch':sw,'hint':hint,'edges':[dict(e,target=sw['block']) for e in sw['incoming']]})
 return eligible,rejected
def validate(before,after,manifest,report):
 eligible,rejected=candidates(before,manifest); guards=[g for g in after['same_value_guards'] if g['generated']]
 counters=report['runs'][0]['counters'];assert counters['candidate_edges']==sum(len(x['edges']) for x in eligible)
 assert counters['rewritten_edges']==len(guards)==counters['fallback_switches_created']
 switches={x['block']:x for x in after['switches']};old={x['switch']['block']:x for x in eligible}
 for name,x in old.items():
  assert name in switches and switches[name]['cases']==x['switch']['cases'] and switches[name]['default']==x['switch']['default']
 assert counters['original_switches_retained']==len(old)
 records=[]
 for g in guards:
  fallback=switches[g['fallback']]
  matches=[x for x in eligible if x['switch'].get('address_decimal')==fallback.get('address_decimal') and x['hint']==integer(g['candidate']) and x['switch']['cases']==fallback['cases'] and x['switch']['default']==fallback['default']]
  assert matches and g['same_value_full_switch'] and g['fallback_has_candidate_case'] and g['fallback_candidate_matches_success']
  assert g['block']==g['load_block'] and fallback['load_block']==g['block']
  assert any(c['target']==g['success'] and integer(c['value'])==integer(g['candidate']) for c in fallback['cases'])
  records.append({'guard':g['block'],'candidate':integer(g['candidate']),'success':g['success'],'fallback':g['fallback'],'address':int(fallback['address_decimal']),'possible_original_switch_blocks':[x['switch']['block'] for x in matches]})
 return eligible,rejected,records

def frontiers(after,guards):
 blocks={b['name']:b for b in after['blocks']};sw={s['block']:s for s in after['switches']};guardmap={g['block']:g for g in after['same_value_guards']};fallbacks={g['fallback'] for g in after['same_value_guards']}
 rows=[]
 for g in guards:
  q=deque([(g['success'],[g['success']])]);visited=set();found=[]
  while q:
   name,path=q.popleft()
   if name in visited:continue
   visited.add(name);b=blocks[name]
   if name in fallbacks:continue
   if name in sw:
    found.append({'kind':'original_dynamic_table_switch' if 'address_decimal' not in sw[name] else 'original_constant_table_switch','block':name,'path':path});continue
   if b['calls']:
    found.append({'kind':'call_boundary','block':name,'path':path});continue
   if not b['successors']:
    found.append({'kind':'exit','block':name,'path':path});continue
   successors=[guardmap[name]['success']] if name in guardmap else [x['target'] for x in b['successors']]
   for child in successors:q.append((child,path+[child]))
  rows.append({'guard':g['guard'],'business_target':g['success'],'frontiers':found,'visited_blocks':len(visited)})
 return rows
out=[];controls=0
for case in ('f-cff','f-combo'):
 for level in ('O0','O2'):
  base=ROOT/'candidate/guarded-transition-tail'/case/level
  report=json.loads((base/'report.json').read_text());manifest=json.loads((ROOT/'candidate/guarded-views'/case/level/'manifest.json').read_text())
  r=ROOT/'structural-review'/f'{case}-{level}'
  before=json.loads((r/'prepared.json').read_text());after=json.loads((r/'after.json').read_text())
  assert report['state']=='finished' and report['total_pass_runs']==31 and report['additional_pass_runs']==6 and report['inherited_pass_runs']==25
  assert report['cleanup_report']['iterations']==1 and [x['name'] for x in report['cleanup_report']['runs']]==['mem2reg','instcombine','gvn','control-flow','dce']
  assert report['controller_report']['iterations']==1 and len(report['controller_report']['runs'])==1
  eligible,rejected,guards=validate(before,after,manifest,report['controller_report'])
  if case=='f-cff':assert guards
  for corruption in ('candidate','fallback'):
   if not guards:continue
   bad=copy.deepcopy(after);first=next(g for g in bad['same_value_guards'] if g['generated'])
   if corruption=='candidate':first['candidate']='i64 -123'
   else:next(x for x in bad['switches'] if x['block']==first['fallback'])['cases']=[]
   try:validate(before,bad,manifest,report['controller_report'])
   except AssertionError:controls+=1
   else:raise AssertionError('faulty inventory accepted')
  out.append(dict(case=case,level=level,counters=report['controller_report']['runs'][0]['counters'],eligible_original_switches=[dict(block=x['switch']['block'],address=x['switch']['address_decimal'],hint=x['hint'],edges=x['edges'],case_targets=x['switch']['cases']) for x in eligible],noneligible_switches=rejected,actual_guarded_edges=guards,all_success_structural_frontiers=frontiers(after,guards),distinct_addresses=sorted({g['address'] for g in guards}),distinct_targets=sorted({g['candidate'] for g in guards})))
result=dict(rows=out,structural_fault_controls=controls,native_executions=0,limits='This reviewer only inspects final IR already used by the separate native oracle. CFG frontier paths are not proven feasible; context counts are not complete loop coverage.')
(ROOT/'structural-review/REVIEW.json').write_text(json.dumps(result,indent=2)+'\n')
print([(x['case'],x['level'],len(x['actual_guarded_edges']),len(x['distinct_addresses'])) for x in out]);print('structural controls',controls)
