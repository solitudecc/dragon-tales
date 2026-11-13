#!/usr/bin/env python3
"""Independent effectful seed/state-value oracle; concrete hints never change the model."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500;CONFIG=0x600;FAIL=0xbad1000000000000
MODES=('plain','table_before','table_after','freeze','flags','duplicate','tail_call')
TARGETS=(43,44,99,127,128,255,777,99999)
INPUTS=tuple((m,u,t,n,size,alias) for m in MODES for u in (0,2,50,51,64,MASK) for t in (43,777,99999) for n in (0,3) for size in (519,1024) for alias in (0,1))
INPUTS+=tuple((m,u,43,3,size,alias) for m in MODES for u,sizes in ((63,(1023,1024)),(50,(919,920))) for size in sizes for alias in (0,1))
INPUTS+=tuple((m,1<<63,43,0,65536,alias) for m in MODES for alias in (0,1))
FAULTS={'double_call':'plain','drop_seed_store':'plain','force_direction':'plain','stale_state':'plain','reload':'table_after','wrong_phi':'plain'}
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t business_calls;
static uint64_t rd(unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void wr(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
void seed_unknown(unsigned char*r,unsigned char*g,unsigned char*c,uint64_t state,uint64_t i){
 uint64_t target=rd(c);++business_calls;
 wr(r,state+i+1000);wr(r+16,state^53);wr(r+48,0x9999+i);
 wr(r+56,rd(r+56)+state+i+1);wr(g+80,state+i+0x66);
 if(state<64)wr(g+512+8*state,(i&1)?44:target);
}
'''
def regoff(k):return GUEST+32 if k[-1] else REGS
def put(a,o,v):struct.pack_into('<Q',a,o,v&MASK)
def get(a,o):return struct.unpack_from('<Q',a,o)[0]
def initial(k,guest):
 mode,u,target,n,size,alias=k;a=bytearray((i*23+17)&255 for i in range(ARENA));r=regoff(k)
 for j in range(64):put(a,GUEST+512+8*j,TARGETS[(j+TARGETS.index(target))%8])
 put(a,GUEST+128,0)
 for o,v in ((0,u),(16,u),(40,n),(48,0x1111),(56,0x1234)):put(a,r+o,v)
 put(a,DESC,guest);put(a,DESC+8,size);put(a,CONFIG,target)
 return a

def finish(label,kind,answer,observed,tag):
 s=label+':\n'
 for o,v in ((0,answer),(16,'%iteration'),(24,'%side'),(32,'%history'),(40,'%state'),(48,observed)):
  s+=f'  %{label}.r{o} = getelementptr i8, ptr %regs, i64 {o}\n  store i64 {v}, ptr %{label}.r{o}, align 1\n'
 for i,v in enumerate((str(kind),'0','49152',observed,'1' if kind==1 else '0','%x',tag)):
  s+=f'  %{label}.e{i} = getelementptr i8, ptr %exit, i64 {8*i}\n  store i64 {v}, ptr %{label}.e{i}, align 1\n'
 return s+f'  ret i64 {answer}\n'

def ir(mode,fault=None):
 assert mode in MODES
 s='''source_filename = "guarded-state-values-independent"
target datalayout = "e-p:64:64"
declare void @seed_unknown(ptr,ptr,ptr,i64,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %x = load i64, ptr %regs, align 1
  %statep = getelementptr i8, ptr %regs, i64 16
  %entry_state = load i64, ptr %statep, align 1
  %limitp = getelementptr i8, ptr %regs, i64 40
  %limit = load i64, ptr %limitp, align 1
  %sidep = getelementptr i8, ptr %regs, i64 48
  %seedp = getelementptr i8, ptr %guest, i64 32
  %branchp = getelementptr i8, ptr %guest, i64 160
  %accp = getelementptr i8, ptr %guest, i64 128
  %odd = and i64 %x, 1
  %isodd = icmp ne i64 %odd, 0
  %forced = select i1 %isodd, i64 777, i64 99999
  br label %header
header:
  %iteration = phi i64 [ 0, %entry ], [ %next, %latch ], [ %next, %latch ]
  %history = phi i64 [ %x, %entry ], [ %loaded, %latch ], [ %loaded, %latch ]
'''
 s+='  %raw = '+('add i64 %entry_state, 0' if fault=='stale_state' else 'load i64, ptr %statep, align 1')+'\n'
 s+='  %state = '+('freeze i64 %raw' if mode=='freeze' else 'add nuw nsw i64 %raw, 0' if mode=='flags' else 'add i64 %raw, 0')+'\n  br label %seed\nseed:\n'
 if fault!='drop_seed_store':s+='  store i64 %state, ptr %seedp, align 1\n'
 call='  call void @seed_unknown(ptr %regs, ptr %guest, ptr %config, i64 %state, i64 %iteration)\n'
 s+=call*(2 if fault=='double_call' else 1)
 s+='  %side = load i64, ptr %sidep, align 1\n'
 s+='  %cmp = '+('icmp eq i64 0, 1' if fault=='force_direction' else 'icmp ugt i64 %state, 50')+'\n'
 s+='  br i1 %cmp, label %'+('small' if mode=='duplicate' else 'large')+', label %small\n'
 if mode!='duplicate':s+='''large:
  %large_state = phi i64 [ %state, %seed ]
  %large_mark = add i64 %large_state, 1000
  store i64 %large_mark, ptr %branchp, align 1
  br label %join
'''
 s+='small:\n  %small_state = phi i64 [ %state, %seed ]'+(', [ %state, %seed ]' if mode=='duplicate' else '')+'\n  %small_mark = add i64 %small_state, 2000\n  store i64 %small_mark, ptr %branchp, align 1\n  br label %join\njoin:\n'
 s+='  %bias = phi i64 [ '+('4' if fault=='wrong_phi' else '3')+', %small ]'+(', [ 7, %large ]' if mode!='duplicate' else '')+'\n'
 s+='''  %state_ok = icmp ult i64 %state, 64
  br i1 %state_ok, label %index, label %state_failure
index:
  %scaled = shl i64 %state, 3
  %table_index = add i64 %scaled, 512
  %tableptr = getelementptr i8, ptr %guest, i64 %table_index
  br label %bounds
bounds:
  %end = add i64 %table_index, 8
  %room = icmp ule i64 %end, %size
  br i1 %room, label %dispatch, label %table_failure
dispatch:
'''
 if mode=='tail_call':s+='  %later = add i64 %iteration, 16\n  call void @seed_unknown(ptr %regs, ptr %guest, ptr %config, i64 %state, i64 %later)\n'
 if mode=='table_before':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 s+='  %loaded = load i64, ptr %tableptr, align 1\n'
 if mode=='table_after':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 if fault=='reload':s+='  %bad_reload = load i64, ptr %tableptr, align 1\n'
 s+='  switch i64 '+('%bad_reload' if fault=='reload' else '%loaded')+', label %good [ i64 43, label %good i64 44, label %good i64 99, label %case99 i64 127, label %case127 i64 128, label %case128 i64 255, label %case255 i64 777, label %central ]\n'
 for name,tag in (('case99',2),('case127',3),('case128',4),('case255',5),('central',6)):
  s+=name+':\n  br label %business\n'
 s+='''good:
  %selected = phi i64 [ 1, %dispatch ], [ 1, %dispatch ], [ 1, %dispatch ]
  br label %business
business:
  %tag = phi i64 [ %selected, %good ], [ 2, %case99 ], [ 3, %case127 ], [ 4, %case128 ], [ 5, %case255 ], [ 6, %central ]
  %oldacc = load i64, ptr %accp, align 1
  %v0 = add i64 %oldacc, %tag
  %v1 = add i64 %v0, %loaded
  %v2 = add i64 %v1, %bias
  store i64 %v2, ptr %accp, align 1
  %r0 = xor i64 %loaded, %side
  %r1 = xor i64 %r0, %history
  %r2 = xor i64 %r1, %bias
  %r3 = xor i64 %r2, %tag
  %answer = xor i64 %r3, %x
  %done = icmp uge i64 %iteration, %limit
  br i1 %done, label %success, label %latch
latch:
  %next = add i64 %iteration, 1
  %parity = and i64 %next, 1
  %again = icmp ne i64 %parity, 0
  br i1 %again, label %header, label %header
'''
 s+=finish('success',1,'%answer','%loaded','%tag')
 s+='state_failure:\n  %state_bad = xor i64 %state, '+str(FAIL)+'\n  br label %state_finish\n'+finish('state_finish',5,'%state_bad','%state','9')
 s+='table_failure:\n  %table_bad = xor i64 %table_index, '+str(FAIL)+'\n  br label %table_finish\n'+finish('table_finish',5,'%table_bad','%table_index','10')
 return s+'}\n'

def model(k,guest,fault=None):
 mode,u,target,n,size,alias=k;a=initial(k,guest);r=regoff(k);iteration=0;history=u;calls=0
 def external(state,i):
  nonlocal calls
  calls+=1;put(a,r,state+i+1000);put(a,r+16,state^53);put(a,r+48,0x9999+i)
  put(a,r+56,get(a,r+56)+state+i+1);put(a,GUEST+80,state+i+0x66)
  if state<64:put(a,GUEST+512+8*state,44 if i&1 else target)
 while True:
  state=u if fault=='stale_state' else get(a,r+16)
  if fault!='drop_seed_store':put(a,GUEST+32,state)
  external(state,iteration)
  if fault=='double_call':external(state,iteration)
  side=get(a,r+48);large=state>50 and mode!='duplicate' and fault!='force_direction'
  put(a,GUEST+160,state+(1000 if large else 2000));bias=7 if large else 4 if fault=='wrong_phi' else 3
  if state>=64:kind=5;answer=state^FAIL;observed=state;tag=9;break
  address=512+8*state
  if address+8>size:kind=5;answer=address^FAIL;observed=address;tag=10;break
  if mode=='tail_call':external(state,iteration+16)
  forced=777 if u&1 else 99999
  if mode=='table_before':put(a,GUEST+address,forced)
  loaded=get(a,GUEST+address)
  if mode=='table_after':put(a,GUEST+address,forced)
  selected=get(a,GUEST+address) if fault=='reload' else loaded
  tag={43:1,44:1,99:2,127:3,128:4,255:5,777:6}.get(selected,1)
  put(a,GUEST+128,get(a,GUEST+128)+tag+loaded+bias)
  answer=loaded^side^history^bias^tag^u;observed=loaded
  if iteration>=n:kind=1;break
  iteration+=1;history=loaded
 for o,v in ((0,answer),(16,iteration),(24,side),(32,history),(40,state),(48,observed)):put(a,r+o,v)
 for i,v in enumerate((kind,0,0xc000,observed,int(kind==1),u,tag)):put(a,EXIT+8*i,v)
 return struct.pack('<QQ',answer,calls)+a

def same(a,b,label):
 if a!=b:raise AssertionError((label,next((i for i,(x,y) in enumerate(zip(a,b)) if x!=y),None),len(a),len(b)))
def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'helper.c').write_text(HELPER);(directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
def run(before,after,wrongvalue,wrongtarget,out,opt):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();address=ctypes.addressof(arena);guest=address+GUEST
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,k):
  ctypes.memmove(address,bytes(initial(k,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'business_calls');calls.value=0
  result=fn(address+regoff(k),address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory,modes in (('before',before,MODES),('after',after,MODES),('wrongvalue',wrongvalue,('plain',)),('wrongtarget',wrongtarget,('plain',))):
   for mode in modes:
    lib,fn=compile_ir(directory/(mode+'.ll'),stage+'-'+mode)
    with gzip.open(out/(stage+'-'+mode+'.snapshots.gz'),'wb') as stream:
     for k in INPUTS:
      if k[0]!=mode:continue
      actual=invoke(lib,fn,k);stream.write(actual);expected=model(k,guest);same(actual,expected,(stage,k));report['normal']+=1
      for offset in (0,8,16+regoff(k),16+GUEST+32)+tuple(16+EXIT+i*8 for i in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:same(damaged,expected,'comparator')
       except AssertionError:report['comparator_controls']+=1
       else:raise AssertionError('comparator escaped')
  for fault,mode in FAULTS.items():
   lib,fn=compile_ir(before/('control_'+fault+'.ll'),'control-'+fault);required=caught=0
   with gzip.open(out/('control-'+fault+'.snapshots.gz'),'wb') as stream:
    for k in INPUTS:
     if k[0]!=mode:continue
     actual=invoke(lib,fn,k);stream.write(actual);normal=model(k,guest);damaged=model(k,guest,fault);same(actual,damaged,('fault model',fault,k));report['fault_observations']+=1
     if damaged!=normal:
      required+=1
      try:same(actual,normal,('fault',fault,k))
      except AssertionError:caught+=1
      else:raise AssertionError('fault escaped')
   assert required and required==caught;report['faults'][fault]={'required':required,'caught':caught}
 except Exception as error:report['errors'].append(repr(error));raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
def main():
 p=argparse.ArgumentParser();s=p.add_subparsers(dest='action',required=True)
 e=s.add_parser('emit');e.add_argument('directory',type=Path)
 r=s.add_parser('run')
 for name in ('before','after','wrongvalue','wrongtarget','output'):r.add_argument(name,type=Path)
 r.add_argument('--opt',type=int,choices=(0,2),required=True);a=p.parse_args()
 if a.action=='emit':emit(a.directory)
 else:run(a.before,a.after,a.wrongvalue,a.wrongtarget,a.output,a.opt)
if __name__=='__main__':main()
