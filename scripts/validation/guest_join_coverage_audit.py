#!/usr/bin/env python3
"""Additional pins/structure audit for the frozen nine-mode native evidence."""
import argparse,hashlib,importlib.util,json,re
from pathlib import Path
SOURCE='4f9faffaf3e58ae95bf827adc87b839a0f2077f64313df5c109ac15b646bec30'
DRIVER='1805db7cd7276985a42358ccf0a1d5cbd2b1fda4afa88a69428c237bc49c0868'
RUNTIMES={'old-main':'34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f',
'old-noz3':'e34bd492f93badcadfd441f51d089eeb06d069e1e5da9faac779b748194df3dd',
'main-v1':'d6cfa1bfc4898abcd2ab771a368010c247b78eb98b27f93f9908b42e9ddd3c2f',
'noz3-v1':'c566a1e43fe067ff43d4b1fb13f8308c3db0b68d56d086d19cb75747429c7ab6'}
def sha(data):return hashlib.sha256(data).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();r=a.root
 report={'native_executions_added':0,'groups':{},'errors':[]}
 try:
  source=r/'source/guest_join_coverage_oracle.py';assert sha(source.read_bytes())==SOURCE
  assert sha((r/'source/guest_join_coverage_driver.cpp').read_bytes())==DRIVER
  spec=importlib.util.spec_from_file_location('frozen_join',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
  assert json.loads((r/'before/inputs.json').read_text())==[list(x) for x in m.INPUTS]
  for mode in m.MODES:assert (r/'before'/(mode+'.ll')).read_text()==m.ir(mode)
  for fault,mode in m.FAULTS.items():assert (r/'before'/('control_'+fault+'.ll')).read_text()==m.ir(mode,fault)
  assert (r/'before/helper.c').read_text()==m.HELPER
  previous=None
  for group,backend in RUNTIMES.items():
   g=r/group;old=group.startswith('old');pins=json.loads((g/'pins-before.json').read_text());assert pins==json.loads((g/'pins-after.json').read_text())
   back=[(k,v) for k,v in pins.items() if k.endswith('/libdragon.so')];assert len(back)==1 and back[0][1]==backend
   assert str(Path(back[0][0]).parent) in (g/'driver-ldd.txt').read_text()
   for name,digest in pins.items():
    # Retired old libraries are bound by before/after hashes and the previous
    # phase's verified gzip archive. Never substitute a current build library.
    if '/runtime-' not in name:assert sha(Path(name).read_bytes())==digest,name
    elif not old:assert sha(Path(name).read_bytes())==digest,name
   results=json.loads((g/'adapter-report.json').read_text());assert set(results)==set(m.MODES)
   for mode,item in results.items():
    invalid=old and mode!='ordinary';stats=item['stats'];assert stats['input_verified'] and stats['saved']
    assert item['exit']==(4 if invalid else 0) and stats['output_verified']==(not invalid)
    assert (g/'after'/(mode+'.ll')).exists()==(not invalid)
    assert (g/'after'/(mode+'.ll.invalid.ll')).exists()==invalid
    if invalid:assert 'PHINode should have one entry for each predecessor' in stats['verify_error']
    else:assert not stats['verify_error']
   if old:
    assert not list(g.glob('results-*'));report['groups'][group]={'verifier_failures':8,'valid_control':1,'native_executions':0};continue
   for mode in ('ordinary','duplicate_join','duplicate_backedge'):
    assert results[mode]['stats']['loadsForwarded']>=1 and results[mode]['stats']['phisInserted']>=1
   ordinary=(g/'after/ordinary.ll').read_text();duplicate=(g/'after/duplicate_join.ll').read_text();loop=(g/'after/duplicate_backedge.ll').read_text()
   def phi_line(text,name):return next(line for line in text.splitlines() if name+' = phi ' in line)
   q=phi_line(ordinary,'%guest.mem.phi');assert q.count('%left ]')==q.count('%right ]')==1
   q=phi_line(duplicate,'%guest.mem.phi');assert q.count('%left ]')==2 and q.count('%right ]')==1
   q=phi_line(loop,'%guest.mem.loop.phi');assert q.count('%entry ]')==1 and q.count('%body ]')==2
   for mode in ('stable','changed','self','nested','unknown_call'):
    assert results[mode]['stats']['joinBarriers']>=1 and results[mode]['stats']['phisInserted']==0
    assert 'guest.mem.phi = phi ' not in (g/'after'/(mode+'.ll')).read_text()
   assert results['unreachable_pred']['stats']['joinBarriers']>=1
   assert (g/'after/unknown_call.ll').read_text().count('call void @join_unknown(')==1
   assert (g/'after/self.ll').read_text().count('store i64 %updated, ptr %slot')==1
   assert (g/'after/nested.ll').read_text().count('store i64 %inner_new, ptr %slot')==1
   saved=json.loads((g/'saved-audit.json').read_text());assert saved=={'streams':48,'frames':1536,'native_executions_added':0,'errors':[]}
   totals={'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0}
   for opt in (0,2):
    stats=json.loads((g/f'results-O{opt}/report.json').read_text());assert not stats['errors']
    assert (g/f'native-O{opt}.exit').read_text().strip()=='0'
    for key in ('normal','fault_observations','comparator_controls'):totals[key]+=stats[key]
    totals['specified_faults']+=sum(x['required'] for x in stats['faults'].values())
   assert totals=={'normal':1152,'fault_observations':384,'specified_faults':288,'comparator_controls':12672}
   report['groups'][group]=totals
   product={p.name:p.read_bytes() for p in (g/'after').glob('*.ll')}
   if previous is not None:assert product==previous
   previous=product
  report['main_noz3_after_ir_byte_equal']=True
 except Exception as error:report['errors'].append(repr(error));raise
 finally:(r/'final-audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
