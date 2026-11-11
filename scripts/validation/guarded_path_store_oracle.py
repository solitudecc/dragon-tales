#!/usr/bin/env python3
"""Separate H path-STORE oracle; original H source and matrix stay frozen."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
import guarded_store_corridor_oracle as frozen
assert hashlib.sha256(Path(frozen.__file__).read_bytes()).hexdigest()=='10d50a43f6441f32c5df1da803df69edf4eb61c39d727a26868682b12e1d6b6c'
MODES=('mixed','mixed_table_after')
VALUES=frozen.VALUES;TARGETS=frozen.TARGETS
INPUTS=tuple((m,x,n,t,size) for m in MODES for x in VALUES for n in range(4) for t in (43,44,777,99999) for size in (263,295,583,65536))
FAULTS={'wrong_selected':'mixed','wrong_initial':'mixed','drop_middle':'mixed','reload':'mixed_table_after'}
MASK=frozen.MASK;ARENA=frozen.ARENA;REGS=frozen.REGS;GUEST=frozen.GUEST;EXIT=frozen.EXIT;DESC=frozen.DESC;CONFIG=frozen.CONFIG;FAILURE=frozen.FAILURE
HELPER=frozen.HELPER;put=frozen.put;get=frozen.get;same=frozen.same

def ir(mode,fault=None):
 assert mode in MODES
 s=frozen.ir('table_after' if mode=='mixed_table_after' else 'plain')
 s=s.replace('source_filename = "guarded-store-corridor-independent"','source_filename = "guarded-path-store-independent"')
 s=s.replace('  %limit = and i64 %n0, 3','  %entry_stateptr = getelementptr i8, ptr %regs, i64 80\n  %entry_state = load i64, ptr %entry_stateptr, align 1\n  %limit = and i64 %n0, 3')
 selected='8' if fault=='wrong_selected' else '50'
 first='50' if fault=='wrong_initial' else '%entry_state'
 s=s.replace('head:\n','head:\n  %path_state = phi i64 [ '+first+', %initial_write ], [ '+selected+', %seed ], [ '+selected+', %seed ]\n')
 if fault!='drop_middle':s=s.replace('  %state_a = load i64','  store i64 %path_state, ptr %aptr, align 1\n  %state_a = load i64')
 old='  store i64 %next_state, ptr %aptr, align 1';assert s.count(old)==1
 s=s.replace(old,'  store i32 8, ptr %unrelatedptr, align 1')
 if mode=='mixed_table_after':
  s=s.replace('  store i64 %forced, ptr %tableptr, align 1','  %external_writeptr = getelementptr i8, ptr %regs, i64 88\n  %external_write = load i64, ptr %external_writeptr, align 1\n  store i64 %external_write, ptr %tableptr, align 1')
 if fault=='reload':
  assert mode=='mixed_table_after'
  s=s.replace('  switch i64 %loaded, label %good','  %reloaded = load i64, ptr %tableptr, align 1\n  switch i64 %reloaded, label %good')
 return s

def initial(key,guest):
 a=frozen.initial(key,guest);put(a,REGS+80,key[1]&63);put(a,REGS+88,777 if key[1]&1 else 99999);return a

def model(key,guest,fault=None):
 mode,x,n,target,size=key;a=initial(key,guest);iteration=0;history=oldtarget=x
 if size<264:
  answer=256^FAILURE;put(a,REGS,answer)
  for i,v in enumerate((5,0,0x9000,256,0,x,1)):put(a,EXIT+i*8,v)
  return struct.pack('<QQ',answer,0)+a
 put(a,GUEST+256,x&63)
 while True:
  fresh=get(a,CONFIG)^iteration;used=history
  if size<296:answer=256^FAILURE;kind=5;routed=256;tag=3;break
  put(a,GUEST+288,used)
  state=(50 if fault=='wrong_initial' else get(a,REGS+80)) if iteration==0 else (8 if fault=='wrong_selected' else 50)
  if fault!='drop_middle':put(a,GUEST+256,state)
  put(a,GUEST+264,get(a,GUEST+256));state=get(a,GUEST+264)
  if state>=64:answer=256^FAILURE;kind=5;routed=256;tag=3;break
  address=512+8*state
  if address+8>size:answer=address^FAILURE;kind=5;routed=address;tag=3;break
  loaded=get(a,GUEST+address)
  if mode=='mixed_table_after':put(a,GUEST+address,get(a,REGS+88))
  selected=get(a,GUEST+address) if fault=='reload' else loaded
  tag={43:1,44:1,99:2,127:3,128:4,255:5,777:6}.get(selected,1);routed=loaded
  put(a,GUEST+128,get(a,GUEST+128)+tag+routed);answer=routed^fresh^used^oldtarget^tag
  if iteration>=n:kind=1;break
  next_iteration=iteration+1;put(a,CONFIG,x^(next_iteration+85));struct.pack_into('<I',a,GUEST+280,8)
  iteration=next_iteration;history=fresh;oldtarget=loaded
 for o,v in ((0,answer),(16,iteration),(24,fresh),(32,used),(40,oldtarget),(48,routed)):put(a,REGS+o,v)
 for i,v in enumerate((kind,0,0x9000,routed,int(kind==1),x,tag)):put(a,EXIT+i*8,v)
 return struct.pack('<QQ',answer,0)+a

def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'helper.c').write_text(HELPER);(directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
def run(before,after,partial,out,opt):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();address=ctypes.addressof(arena);guest=address+GUEST
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,key):
  ctypes.memmove(address,bytes(initial(key,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'business_calls');calls.value=0
  result=fn(address+REGS,address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory,modes in (('before',before,MODES),('after',after,MODES)):
   for mode in modes:
    lib,fn=compile_ir(directory/(mode+'.ll'),stage+'-'+mode)
    with gzip.open(out/(stage+'-'+mode+'.snapshots.gz'),'wb') as stream:
     for key in INPUTS:
      if key[0]!=mode:continue
      actual=invoke(lib,fn,key);stream.write(actual);expected=model(key,guest);same(actual,expected,(stage,key));report['normal']+=1
      for offset in (0,8,16+REGS,16+GUEST+512)+tuple(16+EXIT+i*8 for i in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:same(damaged,expected,'comparator control')
       except AssertionError:report['comparator_controls']+=1
       else:raise AssertionError('comparator escaped')
  for fault,mode in FAULTS.items():
   lib,fn=compile_ir(before/('control_'+fault+'.ll'),'control-'+fault);required=caught=0
   with gzip.open(out/('control-'+fault+'.snapshots.gz'),'wb') as stream:
    for key in INPUTS:
     if key[0]!=mode:continue
     actual=invoke(lib,fn,key);stream.write(actual);expected=model(key,guest);damaged=model(key,guest,fault)
     same(actual,damaged,('fault model',fault,key));report['fault_observations']+=1
     if damaged!=expected:
      required+=1
      try:same(actual,expected,('fault',fault,key))
      except AssertionError:caught+=1
      else:raise AssertionError('fault escaped')
   assert required and caught==required;report['faults'][fault]={'required':required,'caught':caught}
 except Exception as error:report['errors'].append(repr(error));raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
def main():
 p=argparse.ArgumentParser();s=p.add_subparsers(dest='action',required=True)
 e=s.add_parser('emit');e.add_argument('directory',type=Path)
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('partial',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=p.parse_args()
 if a.action=='emit':emit(a.directory)
 else:run(a.before,a.after,a.partial,a.output,a.opt)
if __name__=='__main__':main()
