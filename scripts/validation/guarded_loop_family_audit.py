"""Independent saved-IR classifier: source Exit PC, not generated default names."""
import copy,re
import guarded_target_sets_real_audit as audit

def pc_map(inventory):
 blocks={b['name']:b for b in inventory['blocks']};result={}
 for sw in inventory['switches']:
  stores=blocks[sw['default']]['stores']
  pcs=[audit.closure.int_value(s['value']) for s in stores if re.search(r'getelementptr i8, ptr %exit, i64 16$',s['pointer']) and re.fullmatch(r'i64 [0-9]+',s['value'])]
  if pcs:
   assert len(pcs)==1
   result[sw['default']]=pcs[0]
 return result

def evaluate(before,after,manifest):
 maps=pc_map(before)
 for name,pc in pc_map(after).items():
  assert name not in maps or maps[name]==pc
  maps[name]=pc
 oldsignature=audit.closure.signature;oldclassify=audit.closure.classify
 def signature(sw):return (tuple(sorted(audit.closure.int_value(c['value']) for c in sw['cases'])),('exit_pc',maps[sw['default']]) if sw['default'] in maps else ('default',sw['default']))
 try:
  audit.closure.signature=signature;audit.closure.classify=audit.classify_guarded
  group,original=audit.classify_guarded(before,manifest);assert group[1][0]=='exit_pc'
  bs={b['name']:b for b in after['blocks']};sw={s['block']:s for s in after['switches']};sets=[]
  for fast in after['switches']:
   fallback=sw.get(fast['default'])
   if not fallback or not bs[fast['block']]['generated']:continue
   assert fast.get('simple_load') and fast.get('same_block_load') and fast['selector']==fallback['selector'] and fast['load']==fallback['load'] and fast['load_block']==fallback['load_block']==fast['block']
   full={audit.closure.int_value(c['value']):c['target'] for c in fallback['cases']};assert 0<len(fast['cases'])<=16 and all(full.get(audit.closure.int_value(c['value']))==c['target'] for c in fast['cases'])
   assert any(signature(s)==signature(fallback) for s in before['switches'])
   sets.append(dict(block=fast['block'],fallback=fallback['block'],cases=fast['cases'],selector=fast['selector']))
  projected=copy.deepcopy(after);lookup={b['name']:b for b in projected['blocks']}
  for s in sets:
   b=lookup[s['block']];assert b['successors'][0]['target']==s['fallback'];b['successors']=b['successors'][1:]
  result=audit.closure.evaluate(before,projected,manifest)
  central=[s['block'] for s in after['switches'] if signature(s)==group]
  # Any full selector with exactly the central case values needs an identified
  # source PC; an unrecognized exit is an audit failure, never silently ignored.
  full=[s for s in after['switches'] if signature(s)[0]==group[0]]
  assert all(s['default'] in maps for s in full)
  original_default={s['default'] for s in original}
  return dict(closure=result,actual_sets=sets,central_blocks=central,central_source_pc=group[1][1],source_pc_groups=[dict(block=s['block'],default=s['default'],exit_pc=maps[s['default']],is_central=s['block'] in central) for s in full],default_pc_evidence=maps)
 finally:audit.closure.signature=oldsignature;audit.closure.classify=oldclassify

def assert_central_coverage(actual, declared):
 assert len(declared)==len(set(declared)) and set(actual)==set(declared), 'central coverage omission or duplicate'

def selftest():
 inventory={'blocks':[{'name':'old','stores':[{'pointer':'%p = getelementptr i8, ptr %exit, i64 16','value':'i64 123'}]},{'name':'clone','stores':[{'pointer':'%q = getelementptr i8, ptr %exit, i64 16','value':'i64 123'}]},{'name':'business','stores':[{'pointer':'%r = getelementptr i8, ptr %exit, i64 16','value':'i64 456'}]}],'switches':[{'default':'old'},{'default':'clone'},{'default':'business'}]}
 m=pc_map(inventory);assert m['old']==m['clone']!=m['business'];bad=copy.deepcopy(inventory);bad['blocks'][1]['stores'][0]['value']='i64 999';assert pc_map(bad)['clone']!=m['old'];bad=copy.deepcopy(inventory);bad['blocks'][1]['stores'][0]['pointer']='%q = getelementptr i8, ptr %other, i64 16';assert 'clone' not in pc_map(bad)
 actual=['original','prefix_clone'];assert_central_coverage(actual,list(actual))
 try:assert_central_coverage(actual,['original'])
 except AssertionError:pass
 else:raise AssertionError('missing prefix central accepted')
 return {'omitted_prefix_count_rejected':True,'renamed_default_family_detected':True,'different_business_pc_separate':True,'wrong_pc_changes_family':True,'non_exit_pointer_rejected':True}
