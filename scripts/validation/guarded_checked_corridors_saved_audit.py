#!/usr/bin/env python3
"""Recheck every saved native frame and its complete independent arena model."""
import argparse,gzip,hashlib,importlib.util,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('--before-only',action='store_true');a=p.parse_args();root=a.root;directory=root/a.label
spec=importlib.util.spec_from_file_location('independent',root/'source/guarded_checked_corridors_oracle.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
report=json.loads((directory/'report.json').read_text());assert not report['errors'];guest=report['guest_address'];frame=16+m.ARENA
rows=[('before-'+x,x,None) for x in m.MODES]
if not a.before_only:rows += [('after-'+x,x,None) for x in m.MODES]+[('wrong-loop','loop',None)]
rows += [('control-'+x,y,x) for x,y in m.FAULTS.items()]
expectedfiles={name+'.snapshots.gz' for name,_,_ in rows};actualfiles={x.name for x in directory.glob('*.snapshots.gz')};assert actualfiles==expectedfiles,(actualfiles-expectedfiles,expectedfiles-actualfiles)
summary={'label':a.label,'streams':len(rows),'frames':0,'normal':0,'fault_observations':0,'specified_fault_detections':0,'comparator_controls':0,'business':0,'checked_failure':0,'faults':{},'files':{}}
for name,mode,fault in rows:
 path=directory/(name+'.snapshots.gz');selected=[k for k in m.INPUTS if k[0]==mode];required=caught=0
 with gzip.open(path,'rb') as stream:
  for k in selected:
   actual=stream.read(frame);assert len(actual)==frame,(name,k,'truncated');expected=m.model(k,guest,fault);m.same(actual,expected,(name,k));summary['frames']+=1
   if fault is None:
    summary['normal']+=1;summary['business' if m.get(actual,16+m.EXIT)==1 else 'checked_failure']+=1
    for offset in (0,8,16+m.regoff(k),16+m.GUEST+32)+tuple(16+m.EXIT+i*8 for i in range(7)):
     damaged=bytearray(actual);damaged[offset]^=1
     try:m.same(damaged,expected,'saved-comparator-control')
     except AssertionError:summary['comparator_controls']+=1
     else:raise AssertionError('saved comparator missed control')
   else:
    summary['fault_observations']+=1;normal=m.model(k,guest)
    if expected!=normal:
     required+=1
     try:m.same(actual,normal,(fault,k))
     except AssertionError:caught+=1
     else:raise AssertionError('saved fault escaped')
  assert stream.read(1)==b'',(name,'trailing bytes')
 if fault is not None:
  assert required==caught and required>0;assert report['faults'][fault]=={'required':required,'caught':caught};summary['faults'][fault]={'required':required,'caught':caught};summary['specified_fault_detections']+=caught
 summary['files'][str(path)]={'size':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'frames':len(selected)}
assert summary['normal']==report['normal'] and summary['fault_observations']==report['fault_observations'] and summary['comparator_controls']==report['comparator_controls'];dest=directory/'saved-audit.json';assert not dest.exists();dest.write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='files'}))
