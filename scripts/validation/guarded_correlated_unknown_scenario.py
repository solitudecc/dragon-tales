#!/usr/bin/env python3
"""Extra actual-scenario unknown-arm rejection; frozen original matrix is unchanged."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
import guarded_correlated_boolean_oracle as base
assert hashlib.sha256(Path(base.__file__).read_bytes()).hexdigest()=='43846283d7b522d9ee5bae110507e365f52f3d14eaf5a89c6abff90f43defa4e'
MODES=('unknown_arm',);INPUTS=tuple(k for k in base.INPUTS if k[0]=='unknown_arm');FAULTS={'wrong_zero':'unknown_arm'}
MASK=base.MASK;ARENA=base.ARENA;REGS=base.REGS;GUEST=base.GUEST;EXIT=base.EXIT;DESC=base.DESC;CONFIG=base.CONFIG;FAILURE=base.FAILURE
HELPER=base.HELPER;put=base.put;get=base.get;same=base.same;initial=base.initial

def ir(mode,fault=None):
 assert mode=='unknown_arm'
 s=base.ir(mode)
 old='  %dispatch_state = select i1 %c0, i64 8, i64 %unknown_state'
 assert s.count(old)==1
 extra='  %arm_state = select i1 %c0, i64 8, i64 %unknown_state\n  %force_visit_bool = zext i1 %c0 to i64\n'
 extra+='  %neutral = add i64 0, 1\n' if fault=='wrong_zero' else '  %neutral = xor i64 %force_visit_bool, %force_visit_bool\n'
 extra+='  %dispatch_state = or i64 %arm_state, %neutral'
 return s.replace(old,extra).replace('guarded-correlated-boolean-independent','guarded-correlated-unknown-scenario')

def model(key,guest,fault=None):
 mode,x,n,target,size=key;a=initial(key,guest);iteration=0;history=oldtarget=x;prior=a[CONFIG+8]
 if size<264:
  answer=256^FAILURE;put(a,REGS,answer)
  for i,v in enumerate((5,0,0x9000,256,0,x,1)):put(a,EXIT+i*8,v)
  return struct.pack('<QQ',answer,0)+a
 put(a,GUEST+256,x&63)
 while True:
  count=4 if mode=='four' else 3 if mode=='three' else 1
  bits=[a[CONFIG+8+i] for i in range(count)]
  if mode in ('three','four'):state=sum((0 if fault=='drop_third' and i==2 else b)<<i for i,b in enumerate(bits))
  elif mode=='unknown_arm':state=(8 if bits[0] else get(a,REGS+72)) | (1 if fault=='wrong_zero' else 0)
  else:
   cond=prior if mode=='history' else bits[0];bit=prior if fault=='stale_bool' else (1-bits[0]) if fault=='wrong_correlated' else bits[0]
   state=(0 if cond else 8)|bit
  fresh=get(a,CONFIG)^iteration;used=history
  if size<296:answer=256^FAILURE;kind=5;routed=256;tag=3;break
  put(a,GUEST+288,used);put(a,GUEST+264,get(a,GUEST+256))
  if state>=64:answer=256^FAILURE;kind=5;routed=256;tag=3;break
  address=512+8*state
  if address+8>size:answer=address^FAILURE;kind=5;routed=address;tag=3;break
  forced=777 if x&1 else 99999
  if mode=='table_before' and fault!='drop_store':put(a,GUEST+address,forced)
  loaded=get(a,GUEST+address)
  if mode=='table_after':put(a,GUEST+address,forced)
  selected=43 if fault=='trust_hint' else get(a,GUEST+address) if fault=='reload' else loaded
  tag={43:1,44:1,99:2,127:3,128:4,255:5,777:6}.get(selected,1)
  routed=(loaded+1)&MASK if fault=='wrong_phi' and tag==1 else loaded
  put(a,GUEST+128,get(a,GUEST+128)+tag+routed);answer=routed^fresh^used^oldtarget^tag
  if iteration>=n:kind=1;break
  next_iteration=iteration+1;put(a,CONFIG,x^(next_iteration+85));put(a,GUEST+256,8 if x&1 else 38)
  for i,b in enumerate(bits):a[CONFIG+8+i]=1-b
  prior=bits[0];iteration=next_iteration;history=fresh;oldtarget=loaded
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
