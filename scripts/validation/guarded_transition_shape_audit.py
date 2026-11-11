#!/usr/bin/env python3
"""Verify frozen fixture-specific guard/same-load/fallback and reentry structure."""
import argparse,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('output',type=Path);a=p.parse_args();records=[]
for api in ('core','auto'):
 for hint in (43,99):
  for mode in ('plain','store_before','store_after','side_reentry','seed_call','duplicate_seed'):
   path=a.directory/f'{api}-hint{hint}'/(mode+'.ll');s=path.read_text()
   items=re.split(r'^([A-Za-z0-9_.]+):[^\n]*\n',s,flags=re.M)
   blocks=dict(zip(items[1::2],items[2::2]));dispatch=blocks['dispatch.transition'];fallback=blocks['transition.fallback']
   assert dispatch.count('load i64')==1
   assert f'%transition.matches = icmp eq i64 %loaded.transition, {hint}' in dispatch
   target='good' if hint==43 else 'alt'
   assert f'br i1 %transition.matches, label %{target}, label %transition.fallback' in dispatch
   assert re.search(r'switch i64 %loaded\.transition, label %miss \[\s+i64 43, label %good\s+i64 44, label %good\s+i64 99, label %alt\s+\]',fallback)
   assert 'load ' not in fallback and 'store ' not in fallback
   assert 'switch i64 %loaded, label %miss [' in blocks['dispatch']
   if mode=='store_after':
    assert dispatch.index('%loaded.transition = load')<dispatch.index('store i64 %forced')<dispatch.index('%transition.matches')
   if mode=='seed_call':assert s.count('call void @transition_unknown(')==1 and 'call void @transition_unknown(' in blocks['seed']
   if api=='core':
    head=blocks['head.transition'];assert '%rawfresh.transition = load i64, ptr %freshptr' in head
    assert '%fresh.transition = freeze i64 %freshbits.transition' in head
    assert '%used_history.transition = add i64 %transition.old, 0' in head
    assert 'label %check.transition, label %side' in head
    assert 'label %write.transition, label %failure' in blocks['check.transition']
    assert 'br label %head' in blocks['side']
    assert '%transition.old = phi i64' in blocks['transition.inputs'] and '%transition.old1 = phi i64' in blocks['transition.inputs']
    if mode=='store_before':assert 'store i64 %forced, ptr %tableptr.transition' in blocks['write.transition']
    if mode=='duplicate_seed':assert 'label %transition.inputs, label %head' in blocks['seed']
   else:
    assert 'head.transition' not in blocks and 'transition.inputs' not in blocks
    if mode=='store_before':assert 'store i64 %forced, ptr %tableptr' in blocks['write']
   records.append({'api':api,'mode':mode,'hint':hint,'actual_load_guard':True,'same_load_fallback':True,'original_switch_retained':True})
a.output.write_text(json.dumps({'records':records,'checked':len(records),'native_executions_added':0,'full_cff':False},indent=2)+'\n');print('24 shape audits passed')
