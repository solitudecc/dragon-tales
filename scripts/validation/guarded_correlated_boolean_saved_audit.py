#!/usr/bin/env python3
"""Recheck every retained I frame, including the separately frozen unknown-scenario supplement."""
import argparse,gzip,hashlib,importlib.util,json,struct,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);p.add_argument('--supplement',action='store_true');a=p.parse_args()
modelname='guarded_correlated_unknown_scenario' if a.supplement else 'guarded_correlated_boolean_oracle'
source=a.root/'source'/(modelname+'.py');expectedsha='e03c41cbe6bfd55a29aa1c08a8fe5263ca3d3aafcb6c3319a34c0b6e8588ce38' if a.supplement else '43846283d7b522d9ee5bae110507e365f52f3d14eaf5a89c6abff90f43defa4e'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(source)==expectedsha;sys.path.insert(0,str(a.root/'source'))
spec=importlib.util.spec_from_file_location('reference',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert json.loads((a.root/('supplement-before' if a.supplement else 'before')/'inputs.json').read_text())==[list(k) for k in m.INPUTS]
out=a.root/a.label;result={'label':a.label,'supplement':a.supplement,'frames':0,'streams':0,'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'before':0,'after_transformed':0,'after_unchanged':0,'business_return':0,'checked_failure':0,'native_executions_added':0}
pins=json.loads((out/'pins-before.json').read_text());assert pins==json.loads((out/'pins-after.json').read_text())
for path,value in pins.items():assert sha(Path(path))==value,path
ldd=(out/'ldd.txt').read_text();assert all(str(Path(path).parent) in ldd for path in pins if path.endswith('libdragon.so'))
rows=json.loads((out/'summary.json').read_text());assert len(rows)==(1 if a.supplement else 13)
for row in rows:
 r=row['report'];assert row['exit']==0 and r['verified_output'] and r['identity_preserved'] and r['metadata_preserved'] and r['option_accepted']
 expected=row['recipe'] in ('normal','partial','resumed') and row['mode'] not in ('four','unknown_arm')
 counts=[x['runs'][0]['counters'].get('rewritten_edges',0) for x in r['rounds']]
 assert bool(sum(counts))==expected and r['unchanged']!=expected
 if expected:assert sum(counts)==(1 if row['recipe']=='partial' else 3 if row['mode']=='history' else 1)
 if row['recipe']=='resumed':assert counts==[1,1,1,0]
for opt in (0,2):
 directory=out/f'native-O{opt}';report=json.loads((directory/'report.json').read_text());assert report['errors']==[] and report['opt']==opt
 guest=report['guest_address'];normal=faultobs=specified=cmp=0;fault_counts={}
 specs=[(stage+'-'+mode,mode,None) for stage in ('before','after') for mode in m.MODES]
 specs += [('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()]
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
     normal+=1;result['before' if name.startswith('before-') else 'after_unchanged' if mode in ('four','unknown_arm') else 'after_transformed']+=1
     kind=struct.unpack_from('<Q',actual,16+m.EXIT)[0];assert kind in (1,5);result['business_return' if kind==1 else 'checked_failure']+=1
     for offset in (0,8,16+m.REGS,16+m.GUEST+512)+tuple(16+m.EXIT+i*8 for i in range(7)):
      damaged=bytearray(actual);damaged[offset]^=1
      try:m.same(damaged,expected,('comparator',name,key))
      except AssertionError:cmp+=1
      else:raise AssertionError('saved comparator escaped')
   assert not stream.read(1),(name,'extra stream bytes')
  result['streams']+=1
  if fault:assert required==caught and required>0;fault_counts[fault]={'required':required,'caught':caught};specified+=required
 assert normal==report['normal']==(512 if a.supplement else 4096)
 assert faultobs==report['fault_observations']==(256 if a.supplement else 1792)
 assert cmp==report['comparator_controls']==normal*11
 assert fault_counts==report['faults']
 for key,value in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:result[key]+=value
assert result['frames']==(1536 if a.supplement else 11776) and result['streams']==(6 if a.supplement else 46)
assert result['before']==(512 if a.supplement else 4096)
assert result['after_transformed']==(0 if a.supplement else 3072) and result['after_unchanged']==(512 if a.supplement else 1024)
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
