#!/usr/bin/env python3
"""Independently replay every saved J normal/fault frame and unchanged strict API gate."""
import argparse,gzip,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args();r=a.root;out=r/a.label
source=r/'source/guarded_partial_corridor_oracle.py'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(source)=='1fef932509527967a8ff460c211930384dab1b6ff85e8d78c32806f6b15cdb33'
spec=importlib.util.spec_from_file_location('reference',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert json.loads((r/'before/inputs.json').read_text())==[list(k) for k in m.INPUTS] and len(m.INPUTS)==3840
assert (r/f'{a.label}-prepare.exit').read_text().strip()=='0'
pins=json.loads((out/'pins-before.json').read_text());assert pins==json.loads((out/'pins-after.json').read_text())
for path,value in pins.items():assert sha(Path(path))==value,path
ldd=(out/'ldd.txt').read_text();assert all(str(Path(path).parent) in ldd for path in pins if path.endswith('libdragon.so'))
rows=json.loads((out/'summary.json').read_text());assert len(rows)==13
for row in rows:
 d=row['report'];assert row['exit']==0 and d['option_accepted'] and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved']
 cs=[x['runs'][0]['counters'] for x in d['rounds']];counts=[x.get('rewritten_edges',0) for x in cs]
 expected=row['recipe'] in ('normal','wrong','resumed') and row['mode']!='call_inside'
 assert sum(counts)==int(expected) and d['unchanged']!=expected
 if expected:
  assert cs[0]['partial_candidate_edges']==cs[0]['partial_attempted_edges']==cs[0]['partial_rewritten_edges']==1
  assert cs[0]['partial_unresolved_edges']==cs[0]['complete']==cs[0]['budget_exhausted']==0
  assert d['rounds'][0]['runs'][0]['status']==2
 if row['recipe']=='resumed':assert counts==[1,0,0] and (out/'resumed/plain.ll').read_bytes()==(out/'normal/plain.ll').read_bytes()
result={'label':a.label,'frames':0,'streams':0,'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'before':0,'after_transformed':0,'after_unchanged':0,'wrong_hint_transformed':0,'business_return':0,'checked_failure':0,'native_executions_added':0,'skipped':0,'excluded':0}
for opt in (0,2):
 assert (r/f'{a.label}-O{opt}.exit').read_text().strip()=='0'
 directory=out/f'native-O{opt}';report=json.loads((directory/'report.json').read_text());assert report['errors']==[] and report['opt']==opt
 guest=report['guest_address'];normal=faultobs=specified=cmp=0;fault_counts={}
 specs=[(stage+'-'+mode,mode,None) for stage in ('before','after') for mode in m.MODES]+[('wrong-plain','plain',None)]+[('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()]
 assert {f.name for f in directory.glob('*.snapshots.gz')}=={name+'.snapshots.gz' for name,_,_ in specs}
 for name,mode,fault in specs:
  required=caught=0
  with gzip.open(directory/(name+'.snapshots.gz'),'rb') as stream:
   for key in m.INPUTS:
    if key[0]!=mode:continue
    actual=stream.read(m.ARENA+16);assert len(actual)==m.ARENA+16,(name,key,'short')
    expected=m.model(key,guest,fault);m.same(actual,expected,(name,key));result['frames']+=1
    if fault:
     faultobs+=1;original=m.model(key,guest)
     if expected!=original:
      required+=1
      try:m.same(actual,original,('specified fault',name,key))
      except AssertionError:caught+=1
      else:raise AssertionError('saved fault escaped')
    else:
     normal+=1
     category='before' if name.startswith('before-') else 'wrong_hint_transformed' if name.startswith('wrong-') else 'after_unchanged' if mode=='call_inside' else 'after_transformed'
     result[category]+=1;kind=struct.unpack_from('<Q',actual,16+m.EXIT)[0];assert kind in (1,5)
     result['business_return' if kind==1 else 'checked_failure']+=1
     for offset in (0,8,16+m.regoff(key),16+m.GUEST+512)+tuple(16+m.EXIT+i*8 for i in range(7)):
      damaged=bytearray(actual);damaged[offset]^=1
      try:m.same(damaged,expected,('comparator',name,key))
      except AssertionError:cmp+=1
      else:raise AssertionError('saved comparator escaped')
   assert not stream.read(1),(name,'extra bytes')
  result['streams']+=1
  if fault:assert required==caught and required>0;fault_counts[fault]={'required':required,'caught':caught};specified+=required
 assert normal==report['normal']==8640 and faultobs==report['fault_observations']==4800
 assert cmp==report['comparator_controls']==95040 and fault_counts==report['faults']
 for key,value in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:result[key]+=value
assert result['frames']==26880 and result['streams']==28
assert (result['before'],result['after_transformed'],result['after_unchanged'],result['wrong_hint_transformed'])==(7680,5760,1920,1920)
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
