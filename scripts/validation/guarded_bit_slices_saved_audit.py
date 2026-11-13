#!/usr/bin/env python3
"""Audit every retained bit-domain C/LLVM native frame against the frozen model."""
import argparse,gzip,hashlib,importlib.util,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('--source',type=Path,required=True);p.add_argument('--before-only',action='store_true');a=p.parse_args()
spec=importlib.util.spec_from_file_location('independent',a.source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
directory=a.root/a.label;report=json.loads((directory/'report.json').read_text());assert not report['errors'];guest=report['guest_address'];frame=16+m.ARENA
rows=[('reference-'+x,x,None,'reference') for x in m.MODES]+[('before-'+x,x,None,'normal') for x in m.MODES]
if not a.before_only:rows += [('after-'+x,x,None,'normal') for x in m.MODES]+[('wrong-l64t1','l64t1',None,'normal')]
rows += [('control-'+fault,mode,fault,'fault') for fault,mode in m.FAULTS.items()]
assert {name+'.snapshots.gz' for name,_,_,_ in rows}=={x.name for x in directory.glob('*.snapshots.gz')},'unexpected/missing native stream'
summary=dict(label=a.label,streams=len(rows),frames=0,normal=0,c_reference=0,fault_observations=0,specified_fault_detections=0,comparator_controls=0,business=0,checked_failure=0,faults={},files={},skips=0,exclusions=0,normalization=0)
for name,mode,fault,kind in rows:
 path=directory/(name+'.snapshots.gz');keys=[k for k in m.INPUTS if k[0]==mode];required=caught=0
 with gzip.open(path,'rb') as stream:
  for k in keys:
   actual=stream.read(frame);assert len(actual)==frame,(name,k,'truncated');expected=m.model(k,guest,fault);m.same(actual,expected,(name,k));summary['frames']+=1
   if kind=='reference':summary['c_reference']+=1
   elif kind=='normal':
    summary['normal']+=1;summary['business' if m.get(actual,16+m.EXIT)==1 else 'checked_failure']+=1
    for offset in (0,8,16+m.regoff(k),16+m.GUEST+96)+tuple(16+m.EXIT+j*8 for j in range(7)):
     bad=bytearray(actual);bad[offset]^=1
     try:m.same(bad,expected,'saved comparator control')
     except AssertionError:summary['comparator_controls']+=1
     else:raise AssertionError('saved comparator control escaped')
   else:
    summary['fault_observations']+=1;normal=m.model(k,guest)
    if expected!=normal:
     required+=1
     try:m.same(actual,normal,(fault,k))
     except AssertionError:caught+=1
     else:raise AssertionError('saved fault escaped')
  assert stream.read(1)==b'',(name,'trailing bytes')
 if fault:
  assert required==caught and required>0;assert report['faults'][fault]==dict(required=required,caught=caught)
  summary['faults'][fault]=dict(required=required,caught=caught);summary['specified_fault_detections']+=caught
 summary['files'][str(path)]=dict(size=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),frames=len(keys))
for key in ('normal','c_reference','fault_observations','comparator_controls'):assert summary[key]==report[key]
out=directory/'saved-audit.json';assert not out.exists();out.write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='files'}))
