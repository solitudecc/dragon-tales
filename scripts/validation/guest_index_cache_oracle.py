#!/usr/bin/env python3
"""Independent native state model for per-candidate historical proof reuse."""
import argparse,ctypes,gzip,hashlib,json,re,struct,subprocess
from pathlib import Path
MODES=('plain','frozen_leaves','call_before','call_after','different_offset','fresh_load','fresh_freeze')
VALUES=(0,1,1<<63,(1<<64)-1)
INPUTS=tuple((m,x,path,bound,frame) for m in MODES for x in VALUES for path in (0,1) for bound in (0,1,2) for frame in (256,512))
FAULTS={'drop_store':'plain','wrong_state':'plain','drop_call':'call_after','bounds':'plain','unknown_constant':'plain','wrong_exit':'plain'}
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500;CONFIG=0x600
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t cache_calls;
void cache_unknown(unsigned char *regs,unsigned char *guest,uint64_t index,uint64_t x){
 uint64_t value=x^UINT64_C(0xabcdef),side,r;++cache_calls;
 memcpy(guest+index,&value,8);memcpy(&side,guest+128,8);++side;memcpy(guest+128,&side,8);
 memcpy(&r,regs+56,8);r^=x;memcpy(regs+56,&r,8);
}
'''
def ir(mode,fault=None):
 s='''source_filename = "guest-index-cache-independent"
target datalayout = "e-p:64:64"
declare void @cache_unknown(ptr,ptr,i64,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %path = load i64, ptr %config, align 1
  %unknownp = getelementptr i8, ptr %config, i64 8
  %unknownframe = load i64, ptr %unknownp, align 1
'''
 level=[]
 for i in range(64):
  s+=f'  %rp{i} = getelementptr i8, ptr %regs, i64 {i*8}\n  %leaf{i} = load i64, ptr %rp{i}, align 1\n'
  if mode=='frozen_leaves':s+=f'  %frozen{i} = freeze i64 %leaf{i}\n';level.append(f'%frozen{i}')
  else:level.append(f'%leaf{i}')
 count=0
 while len(level)>1:
  nextlevel=[]
  for i in range(0,len(level),2):
   name='%tree'+str(count);count+=1;s+=f'  {name} = add i64 {level[i]}, {level[i+1]}\n';nextlevel.append(name)
  level=nextlevel
 frame=level[0]
 s+=f'  %storedindex = add i64 {frame}, {24 if mode=="different_offset" else 16}\n'
 s+='  %storedptr = getelementptr i8, ptr %guest, i64 %storedindex\n  %choose = icmp eq i64 %path, 0\n  br i1 %choose, label %known, label %unknown\nknown:\n'
 if mode=='call_before' and fault!='drop_call':s+='  call void @cache_unknown(ptr %regs,ptr %guest,i64 %storedindex,i64 %leaf0)\n'
 if fault!='drop_store':s+=f'  store i64 {44 if fault=="wrong_state" else 43}, ptr %storedptr, align 1\n'
 if mode=='call_after' and fault!='drop_call':s+='  call void @cache_unknown(ptr %regs,ptr %guest,i64 %storedindex,i64 %leaf0)\n'
 s+='  br label %join\nunknown:\n  br label %join\njoin:\n'
 s+=f'  %selected = phi i64 [ {frame}, %known ], [ %unknownframe, %unknown ]\n'
 selected='%selected'
 if mode=='fresh_load':s+='  %freshframe = load i64, ptr %rp2, align 1\n';selected='%freshframe'
 if mode=='fresh_freeze':s+='  %freshframe = freeze i64 %selected\n';selected='%freshframe'
 s+=f'  %index = add i64 {selected}, 16\n  %room = icmp uge i64 %size, 8\n  %lastaddress = sub i64 %size, 8\n  %inside = icmp ule i64 %index, %lastaddress\n  %valid = and i1 %room, %inside\n'
 s+='  br i1 '+('true' if fault=='bounds' else '%valid')+', label %read, label %failure\nread:\n'
 s+='  %targetptr = getelementptr i8, ptr %guest, i64 %index\n'
 if fault=='unknown_constant':s+='  %state = add i64 43, 0\n'
 else:s+='  %state = load i64, ptr %targetptr, align 1\n'
 s+='  %good = xor i64 %state, %leaf0\n  br label %done\nfailure:\n  %bad = xor i64 %index, -4985484787499139072\n  br label %done\ndone:\n'
 s+='  %answer = phi i64 [ %good, %read ], [ %bad, %failure ]\n  %observed = phi i64 [ %state, %read ], [ 0, %failure ]\n  %kind = phi i64 [ 1, %read ], [ 5, %failure ]\n  %ok = phi i64 [ 1, %read ], [ 0, %failure ]\n'
 for offset,value in ((0,'%answer'),(16,'%observed'),(24,'%index'),(32,'%selected')):s+=f'  %out{offset} = getelementptr i8, ptr %regs, i64 {offset}\n  store i64 {value}, ptr %out{offset}, align 1\n'
 fields=('%kind','0',str(0x7000+MODES.index(mode)+(fault=='wrong_exit')),'%index','%ok','%leaf0','%observed')
 for i,value in enumerate(fields):s+=f'  %e{i} = getelementptr i8, ptr %exit, i64 {i*8}\n  store i64 {value}, ptr %e{i}, align 1\n'
 return s+'  ret i64 %answer\n}\n'
def put(a,offset,value):struct.pack_into('<Q',a,offset,value&MASK)
def get(a,offset):return struct.unpack_from('<Q',a,offset)[0]
def parameters(key):
 mode,x,path,bound,frame=key;selected=frame+(32 if path else 0);index=(frame if mode=='fresh_load' else selected)+16
 return selected,index,(65536,index+7,0)[bound]
def initial(key,guest_address):
 mode,x,path,bound,frame=key;a=bytearray((i*37+11)&255 for i in range(ARENA))
 for i in range(64):put(a,REGS+i*8,0)
 put(a,REGS,x);put(a,REGS+8,-x);put(a,REGS+16,frame)
 selected,index,size=parameters(key);put(a,DESC,guest_address);put(a,DESC+8,size);put(a,CONFIG,path);put(a,CONFIG+8,frame+32)
 return a
def model(key,guest_address,fault=None):
 mode,x,path,bound,frame=key;a=initial(key,guest_address);calls=0;storedindex=frame+(24 if mode=='different_offset' else 16)
 def call():
  nonlocal calls
  calls+=1;put(a,GUEST+storedindex,x^0xabcdef);put(a,GUEST+128,get(a,GUEST+128)+1);put(a,REGS+56,get(a,REGS+56)^x)
 if path==0:
  if mode=='call_before' and fault!='drop_call':call()
  if fault!='drop_store':put(a,GUEST+storedindex,44 if fault=='wrong_state' else 43)
  if mode=='call_after' and fault!='drop_call':call()
 selected,index,size=parameters(key);valid=size>=8 and index<=size-8
 if valid or fault=='bounds':
  observed=43 if fault=='unknown_constant' else get(a,GUEST+index);answer=observed^x;kind=1;ok=1
 else:observed=0;answer=index^0xbad0000000000000;kind=5;ok=0
 for offset,value in ((0,answer),(16,observed),(24,index),(32,selected)):put(a,REGS+offset,value)
 for i,value in enumerate((kind,0,0x7000+MODES.index(mode)+(fault=='wrong_exit'),index,ok,x,observed)):put(a,EXIT+i*8,value)
 return struct.pack('<QQ',answer,calls)+a
def same(actual,expected,label):
 if actual!=expected:
  first=next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)
  raise AssertionError((label,first,len(actual),len(expected)))
def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'helper.c').write_text(HELPER);(directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
 (directory/'CONTRACT.json').write_text(json.dumps({'modes':MODES,'inputs':len(INPUTS),'faults':FAULTS,'snapshot_bytes':16+ARENA,
 'new_gain_required':['plain','frozen_leaves','call_before'],'no_gain_required':['call_after','different_offset','fresh_load','fresh_freeze'],
 'old_expected':'valid/no copy under same index-proof budget; no old semantic failure claimed','exclusions':[]},indent=2)+'\n')
def run(before,after,out,opt):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();address=ctypes.addressof(arena);guest=address+GUEST
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  # The production adapter only publishes .ll when verify succeeded. Verify
  # again before compilation to ensure an old invalid artifact cannot execute.
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,key):
  ctypes.memmove(address,bytes(initial(key,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'cache_calls');calls.value=0
  result=fn(address+REGS,address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory in (('before',before),('after',after)):
   for mode in MODES:
    lib,fn=compile_ir(directory/(mode+'.ll'),stage+'-'+mode)
    with gzip.open(out/(stage+'-'+mode+'.snapshots.gz'),'wb') as stream:
     for key in INPUTS:
      if key[0]!=mode:continue
      actual=invoke(lib,fn,key);stream.write(actual);expected=model(key,guest);same(actual,expected,(stage,key));report['normal']+=1
      for offset in (0,8,16+REGS,16+GUEST+64)+tuple(16+EXIT+i*8 for i in range(7)):
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
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 x=p.parse_args()
 if x.action=='emit':emit(x.directory)
 else:run(x.before,x.after,x.output,x.opt)
if __name__=='__main__':main()
