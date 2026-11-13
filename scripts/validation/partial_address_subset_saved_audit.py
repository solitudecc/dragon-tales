#!/usr/bin/env python3
"""Read-only independent replay of all saved new full-state streams and API gates."""
import argparse,gzip,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args();r=a.root;o=r/a.label
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
src=r/'source/partial_address_subset_oracle.py';assert sha(src)=='85bc64f185b1456be3b07796e25183b5bb204ebf6f8d68128236dee90a9ed889';spec=importlib.util.spec_from_file_location('m',src);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
pre=json.loads((r/'PREDECLARED.json').read_text());assert len(m.INPUTS)==len(set(m.INPUTS))==384 and json.loads((r/'before/inputs.json').read_text())==[list(k) for k in m.INPUTS]
for name,digest in pre['files'].items():assert sha(r/name)==digest,name
pins=json.loads((o/'pins-before.json').read_text());afterpins=json.loads((o/'pins-after.json').read_text());assert all(afterpins.values());assert set(pins)==set(afterpins)
for name,digest in pins.items():assert sha(Path(name))==digest,name
assert str(r/'driver') in pins and str(Path(next(name for name in pins if name.endswith('libdragon.so'))).parent) in (o/'ldd.txt').read_text();assert 'libLLVM.so' not in (o/'ldd.txt').read_text()
gate=json.loads((o/'API-GATES.json').read_text());assert gate['rows']==62 and gate['common_errors']==[] and gate['precision_pass'] and len(gate['precision'])==15 and all(v['passed'] for v in gate['precision'])
result=dict(label=a.label,normal=0,fault_observations=0,specified_faults=0,comparator_controls=0,streams=0,frames=0,business=0,checked_failure=0,native_executions_added=0,excluded=0,skipped=0)
for opt in (0,2):
 assert (r/f'{a.label}-O{opt}.exit').read_text().strip()=='0';d=o/f'native-O{opt}';report=json.loads((d/'report.json').read_text());assert report['errors']==[] and report['opt']==opt;guest=report['guest_address'];normal=faultobs=cmp=specified=0;faults={}
 specs=[('before-'+mode,mode,None) for mode in m.MODES]+[('after-'+profile+'-'+mode,mode,None) for profile in m.PROFILES for mode in m.MODES]+[('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()]
 assert {p.name for p in d.glob('*.snapshots.gz')}=={name+'.snapshots.gz' for name,_,_ in specs}
 for name,mode,fault in specs:
  required=caught=0
  with gzip.open(d/(name+'.snapshots.gz'),'rb') as stream:
   for key in m.INPUTS:
    if key[0]!=mode:continue
    actual=stream.read(m.ARENA+16);assert len(actual)==m.ARENA+16,(name,key,'short');expected=m.model(key,guest,fault);m.same(actual,expected,(name,key));result['frames']+=1
    if fault:
     faultobs+=1;original=m.model(key,guest)
     if expected!=original:
      required+=1
      try:m.same(actual,original,(name,key,'fault comparator'))
      except AssertionError:caught+=1
      else:raise AssertionError('fault escaped')
    else:
     normal+=1;kind=struct.unpack_from('<Q',actual,16+m.EXIT)[0];assert kind in (1,5);result['business' if kind==1 else 'checked_failure']+=1
     for offset in (0,8,16+m.regoff(key),16+m.GUEST+512)+tuple(16+m.EXIT+i*8 for i in range(7)):
      damaged=bytearray(actual);damaged[offset]^=1
      try:m.same(damaged,expected,('saved comparator',name,key))
      except AssertionError:cmp+=1
      else:raise AssertionError('comparator escaped')
   assert not stream.read(1),(name,'extra bytes')
  result['streams']+=1
  if fault:assert required==caught and required>0;specified+=required;faults[fault]=dict(required=required,caught=caught)
 assert normal==report['normal']==3456 and faultobs==report['fault_observations']==672 and cmp==report['comparator_controls']==38016 and faults==report['faults']
 for k,v in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:result[k]+=v
assert result['normal']==pre['normal_per_build']==6912 and result['fault_observations']==pre['fault_observations_per_build']==1344 and result['streams']==pre['streams_per_build']==86 and result['frames']==pre['frames_per_build']==8256
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
