#!/usr/bin/env python3
"""Read-only complete saved-stream audit; adds zero native executions."""
import argparse, gzip, hashlib, importlib.util, json, re
from pathlib import Path

PINS = {
 'main': ('34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f','953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84'),
 'noz3': ('e34bd492f93badcadfd441f51d089eeb06d069e1e5da9faac779b748194df3dd','061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0')}
SOURCE = 'e28032a57b2d3f8afd5a2a2bae4cf0c56b45a4426fd198525246771410e40aaa'
DRIVER = 'c3de98f01ba23cd2bd68145df2ac4a86e289ebe68f4ac41a6900b5d9551cd941'
BINARY = '6e579299a7c769cf5ff3afa81f92f94843fe55c7048384e93f30d22b9ef35889'
def sha(data): return hashlib.sha256(data).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--flavor',choices=PINS,required=True);a=p.parse_args();r=a.root
 report={'flavor':a.flavor,'native_executions_added':0,'frames':0,'streams':0,'errors':[]}
 try:
  source=r/'entry_prefix_liveout_oracle.py';assert sha(source.read_bytes())==SOURCE
  spec=importlib.util.spec_from_file_location('frozen_oracle',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
  pintext=(r/'source-runtime-before.sha256').read_text();assert pintext==(r/'source-runtime-after.sha256').read_text()
  pins={name:digest for digest,name in (line.split(None,1) for line in pintext.splitlines())}
  assert len(pins)==5
  assert pins['entry_prefix_liveout_oracle.py']==SOURCE and pins['entry_prefix_production_driver.cpp']==DRIVER and pins['prefix-production']==BINARY
  for name,digest in pins.items():assert sha((r/name).read_bytes())==digest,name
  runtime='../runtime-'+a.flavor+'-v1/'
  assert pins[runtime+'libdragon.so']==PINS[a.flavor][0] and pins[runtime+'libdragon_c.so']==PINS[a.flavor][1]
  assert str((r/runtime).resolve()) in (r/'driver-ldd.txt').read_text()
  if a.flavor=='noz3': assert 'DRAGON_USE_Z3:BOOL=OFF' in (r/'build-noz3-option.txt').read_text()
  assert json.loads((r/'before/inputs.json').read_text())==[list(x) for x in m.INPUTS]
  assert (r/'before/helper.c').read_text()==m.HELPER
  for mode in m.MODES:assert (r/'before'/f'{mode}.ll').read_text()==m.ir(mode)
  for fault,mode in m.FAULTS.items():assert (r/'before'/f'control_{fault}.ll').read_text()==m.ir(mode,fault)
  for name,text in m.structural().items():assert (r/'before'/f'{name}.ll').read_text()==text
  for opt in (0,2):
   out=r/f'results-O{opt}';stats=json.loads((out/'report.json').read_text())
   assert not stats['failures'] and stats['normal']==768 and stats['fault_observations']==448 and stats['comparator_controls']==8448
   expected_streams=set()
   for stage in ('before','after'):
    for mode in m.MODES:
     path=out/f'{stage}-{mode}.snapshots.gz';expected_streams.add(path.name)
     with gzip.open(path,'rb') as stream:
      for key in m.INPUTS:
       if key[0]!=mode:continue
       m.same(stream.read(16+m.ARENA),m.model(key),(opt,stage,key));report['frames']+=1
      assert stream.read(1)==b''
     report['streams']+=1
   for fault,mode in m.FAULTS.items():
    path=out/f'control-{fault}.snapshots.gz';expected_streams.add(path.name);required=caught=0
    with gzip.open(path,'rb') as stream:
     for key in m.INPUTS:
      if key[0]!=mode:continue
      actual=stream.read(16+m.ARENA);expected=m.model(key);damaged=m.model(key,fault)
      m.same(actual,damaged,(opt,fault,key));report['frames']+=1
      if expected!=damaged:
       required+=1
       try:m.same(actual,expected,'saved faulty IR')
       except AssertionError:caught+=1
       else:raise AssertionError('saved fault escaped')
     assert stream.read(1)==b''
    report['streams']+=1
    assert stats['faults'][fault]=={'required':required,'caught':caught} and required==caught>0
   assert set(p.name for p in out.glob('*.snapshots.gz'))==expected_streams
   assert sum(x['required'] for x in stats['faults'].values())==238
  assert report['frames']==2432 and report['streams']==38
  identity=('moduleIdentity','globalIdentity','argumentIdentity','ordinaryUnchanged','metadataUnchanged','verified')
  for name in (*m.MODES,*m.structural()):
   stats=json.loads((r/'logs'/f'{name}.stderr').read_text());rejected=name.startswith('reject_')
   assert all(stats[x] for x in identity) and stats['available']
   assert stats['changed']==(not rejected) and stats['complete']==(not rejected)
   assert int((r/'logs'/f'{name}.exit').read_text())==(3 if rejected else 0)
   folder='after' if name in m.MODES else 'structural-after'
   assert (r/folder/f'{name}.ll').exists()==(not rejected)
   if rejected:assert stats['rejectedUnchanged']
   else:assert stats['peeledEdges']==1
  for mode in m.MODES:
   text=(r/'after'/f'{mode}.ll').read_text()
   assert re.search(r'(?m)^guarded\.accept:[^\n]*\n\s*br label %definition\.entry\.prefix\b',text)
   out=re.search(r'(?ms)^out:[^\n]*\n(.*?)(?=^[A-Za-z_.][A-Za-z_0-9.]*:|^})',text).group(1)
   assert '= phi i64 ' in out and not re.search(r'^out\.entry\.prefix:',text,re.M)
   if mode=='duplicate_exit':assert any(line.count('%definition ]')==3 and line.count('%definition.entry.prefix ]')==3 for line in out.splitlines() if '= phi i64 ' in line)
   if mode=='unknown_call':assert text.count('call void @entry_liveout_unknown(')==out.count('call void @entry_liveout_unknown(')==1
   if mode=='volatile':assert len(re.findall(r'\b(?:load|store) volatile\b',text))==len(re.findall(r'\b(?:load|store) volatile\b',out))==4
   if mode=='loop_local':
    assert re.search(r'(?m)^latch:[^\n]*\n\s*store i64 %stored, ptr %local_address, align 1\n\s*br label %definition\b',text)
    assert '[ %history_next, %latch ]' in text
  text=(r/'structural-after/struct_freeze_undef.ll').read_text()
  assert text.count('freeze i64 undef')==2
  assert '[ %fresh, %definition ]' in text and '[ %fresh.entry.prefix, %definition.entry.prefix ]' in text
  text=(r/'structural-after/struct_poison_flag.ll').read_text()
  assert text.count('add nsw i64')==2 and '[ poison, %fallback ]' in text
  compat=r/'compat-prior13';c=json.loads((compat/'report.json').read_text())
  assert not c['failures'] and c['native_executions_added']==0 and len(c['compatible'])==13
  for item in c['compatible']:
   name=item['name'];actual=(compat/f'{name}.ll').read_bytes();expected=(compat/'references'/f'{name}-expected-after.ll').read_bytes()
   m.same(actual,expected,name);assert sha(actual)==item['actual_sha256']==item['expected_sha256']
   assert sha((compat/'references'/f'{name}-input.ll').read_bytes())==item['source_sha256']
  assert len(c['controls'])==4
  for item in c['controls']:
   changed=item['name']=='old-direct-liveout-now-positive';assert item['expected_changed']==changed
   stats=json.loads((compat/(item['name']+'.stderr')).read_text());assert stats==item['stats']
   assert stats['changed']==changed and stats['verified'] and item['exit']==(0 if changed else 3)
   assert (compat/(item['name']+'.ll')).exists()==changed
   if not changed:assert stats['rejectedUnchanged'] and not stats['complete']
  report.update({'compatibility_byte_equal':13,'previous_native_recounted':0,'normal_observations':1536,'fault_observations':896,'specified_faults_caught':476,'comparator_controls_recorded':16896,'source_sha256':SOURCE,'runtime_pins':PINS[a.flavor]})
 except Exception as error:
  report['errors'].append(repr(error));raise
 finally:
  (r/'saved-audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
