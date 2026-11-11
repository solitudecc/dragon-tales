#!/usr/bin/env python3
"""Recheck every preserved native frame; this does not execute native code."""
import argparse,gzip,hashlib,importlib.util,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args()
source=a.root/'source/guarded_transition_oracle.py';assert hashlib.sha256(source.read_bytes()).hexdigest()=='8929beea4d9341baf8063dd2a35a5811e7e08cb992c8a9c733509c315fbe762e'
spec=importlib.util.spec_from_file_location('reference',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert json.loads((a.root/'before/inputs.json').read_text())==[list(k) for k in m.INPUTS]
out=a.root/a.label;result={'label':a.label,'frames':0,'streams':0,'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'api_groups':{},'native_executions_added':0}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins=json.loads((out/'pins-before.json').read_text());assert pins==json.loads((out/'pins-after.json').read_text())
for path,value in pins.items():assert sha(Path(path))==value,path
assert sum('libdragon.so' in path for path in pins)==1
ldd=(out/'ldd.txt').read_text();assert all(str(Path(path).parent) in ldd for path in pins if path.endswith('libdragon.so'))
positive=json.loads((out/'positive-summary.json').read_text());assert len(positive)==24
for item in positive:
 r=item['report'];assert item['exit']==0 and all(r[k] for k in ('changed','verified_output','identity_preserved','input_verified'))
 if item['api']=='core':assert r['snapshot_phis']==2 and r['cloned_blocks']==6
 else:assert r['rewritten_edges']==1 and r['fallback_switches']==1 and r['original_switches_retained']==1
for path in (out/'reject').glob('*.json'):
 r=json.loads(path.read_text());assert r['unchanged_on_rejection'] and r['verified_output'] and r['identity_preserved']
 assert int(path.with_suffix('.exit').read_text())==3 and not path.with_suffix('.ll').exists()
for api in ('core','auto'):
 group={'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0}
 for opt in (0,2):
  directory=out/f'{api}-O{opt}';report=json.loads((directory/'report.json').read_text());assert report['errors']==[] and report['opt']==opt
  guest=report['guest_address'];normal=faultobs=specified=cmp=0;fault_counts={}
  specs=[(stage+'-'+mode,mode,None) for stage in ('before','hint43','hint99') for mode in m.MODES]+[('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()]
  assert {f.name for f in directory.glob('*.snapshots.gz')}=={name+'.snapshots.gz' for name,_,_ in specs}
  for name,mode,fault in specs:
   required=caught=0
   with gzip.open(directory/(name+'.snapshots.gz'),'rb') as stream:
    for key in m.INPUTS:
     if key[0]!=mode:continue
     actual=stream.read(m.ARENA+16);assert len(actual)==m.ARENA+16,(name,key,'short stream')
     expected=m.model(key,guest,fault);m.same(actual,expected,(name,key));result['frames']+=1
     if fault:
      faultobs+=1;original=m.model(key,guest)
      if expected!=original:
       required+=1
       try:m.same(actual,original,('fault rejection',name,key))
       except AssertionError:caught+=1
       else:raise AssertionError('saved fault escaped')
     else:
      normal+=1
      for offset in (0,8,16+m.REGS,16+m.GUEST+512)+tuple(16+m.EXIT+i*8 for i in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:m.same(damaged,expected,('comparator',name,key))
       except AssertionError:cmp+=1
       else:raise AssertionError('saved comparator escaped')
    assert not stream.read(1),(name,'extra stream bytes')
   result['streams']+=1
   if fault:assert required==caught and required>0;fault_counts[fault]={'required':required,'caught':caught};specified+=required
  assert normal==report['normal']==2304 and faultobs==report['fault_observations']==1024 and cmp==report['comparator_controls']==25344
  assert fault_counts==report['faults']
  for key,value in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:group[key]+=value;result[key]+=value
 result['api_groups'][api]=group
assert result['frames']==13312 and result['streams']==104
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
