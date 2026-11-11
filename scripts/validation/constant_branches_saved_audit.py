#!/usr/bin/env python3
"""Full saved-frame review for the unchanged business model and new literal shape."""
import argparse,gzip,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('output',type=Path);a=p.parse_args();r=a.root;out=r/a.label
mfile=r/'frozen/guarded_business_oracle.py';sfile=r/'source/constant_branches_shape_oracle.py'
assert hashlib.sha256(mfile.read_bytes()).hexdigest()=='19bb40a10559a6ea3c385f60c590ad3c809a6e69e7fcd5e6d108c468c12888bd'
assert hashlib.sha256(sfile.read_bytes()).hexdigest()=='b096f3133b3dcff1a8dd12fe83a3b8f5ae71e06ae568f568de476bb0b19920f6'
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
m=load('business',mfile);s=load('shape',sfile)
assert json.loads((r/'frozen/inputs.json').read_text())==[list(k) for k in m.INPUTS]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
pins=json.loads((out/'pins-before.json').read_text());assert pins==json.loads((out/'pins-after.json').read_text())
for path,value in pins.items():assert sha(path)==value,path
ldd=(out/'ldd.txt').read_text();assert all(str(Path(path).parent) in ldd for path in pins if path.endswith('libdragon.so'))
rows=json.loads((out/'summary.json').read_text());assert len(rows)==14
for row in rows:assert row['exit']==0 and row['report']['verified_output'] and row['report']['identity_preserved'] and row['report']['option_accepted']
assert (out/'pre/call_path.ll').read_bytes()==(out/'post/call_path.ll').read_bytes()
result={'label':a.label,'groups':{},'normal':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'frames':0,'streams':0,'native_executions_added':0}
for group in ('business','shape'):
 counts={'normal':0,'before':0,'after':0,'business_return':0,'checked_failure':0,'fault_observations':0,'specified_faults':0,'comparator_controls':0,'frames':0,'streams':0}
 for opt in (0,2):
  d=out/(f'native-O{opt}' if group=='business' else f'shape-native-O{opt}');report=json.loads((d/'report.json').read_text());assert report['errors']==[] and report['opt']==opt;guest=report['guest_address']
  specs=([(stage+'-'+mode,mode,None) for stage in ('before','after') for mode in m.MODES]+[('control-'+fault,mode,fault) for fault,mode in m.FAULTS.items()] if group=='business' else [('before','plain',None),('after','plain',None),('wrong_target','plain','wrong_target'),('drop_live_store','plain','drop_live_store')])
  assert {path.name for path in d.glob('*.snapshots.gz')}=={name+'.snapshots.gz' for name,_,_ in specs}
  normal=faultobs=cmp=specified=0;faults={}
  for name,mode,fault in specs:
   required=caught=0
   with gzip.open(d/(name+'.snapshots.gz'),'rb') as stream:
    for key in m.INPUTS:
     if key[0]!=mode:continue
     actual=stream.read(m.ARENA+16);assert len(actual)==m.ARENA+16
     expected=m.model(key,guest,fault) if group=='business' else s.model(m,key,guest,fault)
     m.same(actual,expected,(group,name,key));counts['frames']+=1
     if fault:
      faultobs+=1;ordinary=m.model(key,guest)
      if expected!=ordinary:
       required+=1
       try:m.same(actual,ordinary,('fault',group,name,key))
       except AssertionError:caught+=1
       else:raise AssertionError('saved fault escaped')
     else:
      normal+=1;counts['before' if name.startswith('before') else 'after']+=1
      kind=struct.unpack_from('<Q',actual,16+m.EXIT)[0];assert kind in (1,5);counts['business_return' if kind==1 else 'checked_failure']+=1
      memory_offset=512 if group=='business' else s.OFFSET
      for offset in (0,8,16+m.REGS,16+m.GUEST+memory_offset)+tuple(16+m.EXIT+i*8 for i in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:m.same(damaged,expected,'comparator')
       except AssertionError:cmp+=1
       else:raise AssertionError('saved comparator escaped')
    assert not stream.read(1)
   counts['streams']+=1
   if fault:assert required==caught and required>0;faults[fault]={'required':required,'caught':caught};specified+=required
  assert normal==report['normal'] and faultobs==report['fault_observations'] and cmp==report['comparator_controls'] and faults==report['faults']
  for key,value in [('normal',normal),('fault_observations',faultobs),('specified_faults',specified),('comparator_controls',cmp)]:counts[key]+=value
 result['groups'][group]=counts
 for key in ('normal','fault_observations','specified_faults','comparator_controls','frames','streams'):result[key]+=counts[key]
assert result['groups']['business']['normal']==3072 and result['groups']['shape']['normal']==512
assert result['frames']==6144 and result['streams']==48 and result['specified_faults']==1794
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
