#!/usr/bin/env python3
"""Replay every saved finite-bitwise frame through the frozen reference."""
import argparse,gzip,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args()
source=a.root/'source/guarded_finite_bitwise_oracle.py';assert hashlib.sha256(source.read_bytes()).hexdigest()=='deedf98e78db63843ad4a028241d0364a6e6d6f2adae8df3ac8276ef48136310'
spec=importlib.util.spec_from_file_location('reference',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert json.loads((a.root/'before/inputs.json').read_text())==[list(k) for k in m.INPUTS]
out=a.root/a.label;result={'label':a.label,'frames':0,'streams':0,'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'before':0,'after_transformed':0,'after_unchanged_dense':0,'business_return':0,'checked_failure':0,'native_executions_added':0}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
pins=json.loads((out/'pins-before.json').read_text());assert pins==json.loads((out/'pins-after.json').read_text())
for path,value in pins.items():assert sha(Path(path))==value,path
ldd=(out/'ldd.txt').read_text();assert all(str(Path(path).parent) in ldd for path in pins if path.endswith('libdragon.so'))
rows=json.loads((out/'summary.json').read_text());assert len(rows)==11
for row in rows:
 r=row['report'];assert row['exit']==0 and r['verified_output'] and r['identity_preserved'] and r['option_accepted']
 expected=row['recipe']=='normal' and row['mode']!='dense'
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in r['rounds']]
 assert bool(sum(counts))==expected and r['unchanged']!=expected
 if expected:assert sum(counts)==(3 if row['mode']=='combo_phi' else 1)
for opt in (0,2):
 directory=out/f'native-O{opt}';report=json.loads((directory/'report.json').read_text());assert report['errors']==[] and report['opt']==opt
 guest=report['guest_address'];normal=faultobs=specified=cmp=0;fault_counts={}
 specs=[(stage+'-'+mode,mode,None) for stage in ('before','after') for mode in m.MODES]+[('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()]
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
     normal+=1;result['before' if name.startswith('before-') else 'after_unchanged_dense' if mode=='dense' else 'after_transformed']+=1
     kind=struct.unpack_from('<Q',actual,16+m.EXIT)[0];assert kind in (1,5);result['business_return' if kind==1 else 'checked_failure']+=1
     for offset in (0,8,16+m.REGS,16+m.GUEST+512)+tuple(16+m.EXIT+i*8 for i in range(7)):
      damaged=bytearray(actual);damaged[offset]^=1
      try:m.same(damaged,expected,('comparator',name,key))
      except AssertionError:cmp+=1
      else:raise AssertionError('saved comparator escaped')
   assert not stream.read(1),(name,'extra stream bytes')
  result['streams']+=1
  if fault:assert required==caught and required>0;fault_counts[fault]={'required':required,'caught':caught};specified+=required
 assert normal==report['normal']==4096 and faultobs==report['fault_observations']==1792 and cmp==report['comparator_controls']==45056
 assert fault_counts==report['faults']
 for key,value in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:result[key]+=value
assert result['frames']==11776 and result['streams']==46
assert result['before']==4096 and result['after_transformed']==3584 and result['after_unchanged_dense']==512
assert result['business_return']==3840 and result['checked_failure']==4352
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
