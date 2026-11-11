#!/usr/bin/env python3
"""Independent partial-hint LLVM/C oracle, including overlapping caller mappings."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
MODES=('plain','table_before','table_after','call_inside')
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500;CONFIG=0x600;FAILURE=0xbad0000000000000
TARGETS=(43,44,99,127,128,255,777,99999)
INPUTS=tuple((m,u,t,c,n,size,alias) for m in MODES for u in (0,1,49,50,51,MASK) for t in (43,777,99999) for c in (0,1) for n in (0,2,3) for size in (519,919,920,65536) for alias in (0,1))
INPUTS+=tuple((m,u,43,c,n,size,alias) for m in MODES for u in (63,64,1<<63) for c in (0,1) for n in (0,3) for size in (519,1023,1024,65536) for alias in (0,1))
FAULTS={'bypass_call':'plain','stale_register':'plain','force_known':'plain','reload':'table_after','drop_fallback':'plain'}
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t business_calls;
static uint64_t rd(unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void wr(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
void business_unknown(unsigned char*r,unsigned char*g,unsigned char*c,uint64_t i){
 uint64_t u=rd(c),cond=(rd(c+8)^i)&1,t=rd(c+16),state=u^(i&1);++business_calls;
 wr(r,u+i);wr(r+8,cond);wr(r+16,state);wr(r+32,~i);wr(r+40,i+100);
 wr(r+48,0x8888+i);wr(r+56,rd(r+56)+u+i+1);
 wr(g+80,u+i+0x55);wr(g+912,t);
 if(state<64)wr(g+512+8*state,(i&1)?44:t);
}
'''
def regoff(key):return GUEST+32 if key[-1] else REGS
def put(a,o,v):struct.pack_into('<Q',a,o,v&MASK)
def get(a,o):return struct.unpack_from('<Q',a,o)[0]
def initial(key,guest):
 mode,u,target,cond,n,size,alias=key;a=bytearray((i*37+11)&255 for i in range(ARENA));r=regoff(key)
 for state in range(64):put(a,GUEST+512+8*state,TARGETS[(state+TARGETS.index(target))%8])
 put(a,GUEST+128,0)
 for o,v in ((0,u),(8,cond^1),(16,7),(32,0x8888),(40,n),(48,0x3333),(56,0x1234)):put(a,r+o,v)
 put(a,DESC,guest);put(a,DESC+8,size);put(a,CONFIG,u);put(a,CONFIG+8,cond);put(a,CONFIG+16,target)
 return a

def finish(label,kind,answer,observed,tag):
 s=label+':\n'
 for o,v in ((0,answer),(16,'%iteration'),(24,'%side'),(32,'%history'),(40,'%state'),(48,observed)):
  s+=f'  %{label}.r{o} = getelementptr i8, ptr %regs, i64 {o}\n  store i64 {v}, ptr %{label}.r{o}, align 1\n'
 for i,v in enumerate((str(kind),'0','45056',observed,'1' if kind==1 else '0','%x',tag)):
  s+=f'  %{label}.e{i} = getelementptr i8, ptr %exit, i64 {8*i}\n  store i64 {v}, ptr %{label}.e{i}, align 1\n'
 return s+f'  ret i64 {answer}\n'

def ir(mode,fault=None):
 assert mode in MODES
 s='''source_filename = "guarded-partial-independent"
target datalayout = "e-p:64:64"
declare void @business_unknown(ptr,ptr,ptr,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %x = load i64, ptr %regs, align 1
  %limitp = getelementptr i8, ptr %regs, i64 40
  %limit = load i64, ptr %limitp, align 1
  %statep = getelementptr i8, ptr %regs, i64 16
  %precall_state = load i64, ptr %statep, align 1
  %condp = getelementptr i8, ptr %regs, i64 8
  %sidep = getelementptr i8, ptr %regs, i64 48
  %accp = getelementptr i8, ptr %guest, i64 128
  %odd = and i64 %x, 1
  %isodd = icmp ne i64 %odd, 0
  %forced = select i1 %isodd, i64 777, i64 99999
  br label %loop
loop:
  %iteration = phi i64 [ 0, %entry ], [ %next, %seed ], [ %next, %seed ]
  %history = phi i64 [ %x, %entry ], [ %loaded, %seed ], [ %loaded, %seed ]
'''
 if fault!='bypass_call':s+='  call void @business_unknown(ptr %regs, ptr %guest, ptr %config, i64 %iteration)\n'
 s+='''  br label %postcall
postcall:
  %poststate = load i64, ptr %statep, align 1
  %condword = load i64, ptr %condp, align 1
  %side = load i64, ptr %sidep, align 1
  %lowbit = and i64 %condword, 1
  %choose = icmp ne i64 %lowbit, 0
'''
 s+='  %state = '+('add i64 50, 0' if fault=='force_known' else 'select i1 %choose, i64 50, i64 '+('%precall_state' if fault=='stale_register' else '%poststate'))+'\n'
 s+='''  br label %state_check
state_check:
  %state_ok = icmp ult i64 %state, 64
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
 if mode=='call_inside':s+='  %inner_iteration = add i64 %iteration, 16\n  call void @business_unknown(ptr %regs, ptr %guest, ptr %config, i64 %inner_iteration)\n'
 if mode=='table_before':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 s+='  %loaded = load i64, ptr %tableptr, align 1\n'
 if mode=='table_after':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 if fault=='reload':s+='  %reloaded = load i64, ptr %tableptr, align 1\n'
 s+='  switch i64 '+('%reloaded' if fault=='reload' else '%loaded')+', label %good [\n'
 for value,target in ((43,'good'),(44,'good'),(99,'alt'),(127,'c127'),(128,'c128'),(255,'c255'),(777,'central')):
  if fault=='drop_fallback' and value==777:continue
  s+=f'    i64 {value}, label %{target}\n'
 s+='  ]\ngood:\n  %selected = phi i64 [ %loaded, %dispatch ], [ %loaded, %dispatch ], [ %loaded, %dispatch ]\n  br label %step\n'
 for name in ('alt','c127','c128','c255','central'):s+=name+':\n  br label %step\n'
 s+='''step:
  %tag = phi i64 [ 1, %good ], [ 2, %alt ], [ 3, %c127 ], [ 4, %c128 ], [ 5, %c255 ], [ 6, %central ]
  %routed = phi i64 [ %selected, %good ], [ %loaded, %alt ], [ %loaded, %c127 ], [ %loaded, %c128 ], [ %loaded, %c255 ], [ %loaded, %central ]
  %acc = load i64, ptr %accp, align 1
  %accplus = add i64 %acc, %tag
  %accnext = add i64 %accplus, %routed
  store i64 %accnext, ptr %accp, align 1
  %r0 = xor i64 %routed, %side
  %r1 = xor i64 %r0, %history
  %r2 = xor i64 %r1, %tag
  %result = xor i64 %r2, %x
  %again = icmp ult i64 %iteration, %limit
  br i1 %again, label %seed, label %finish
seed:
  %next = add i64 %iteration, 1
  br i1 %choose, label %loop, label %loop
'''
 s+=finish('finish',1,'%result','%routed','%tag')
 s+='state_failure:\n  %state_bad = xor i64 %state, '+str(FAILURE)+'\n  br label %state_finish\n'+finish('state_finish',5,'%state_bad','%state','9')
 s+='table_failure:\n  %table_bad = xor i64 %table_index, '+str(FAILURE)+'\n  br label %table_finish\n'+finish('table_finish',5,'%table_bad','%table_index','10')
 return s+'}\n'

def model(key,guest,fault=None):
 mode,u,target,cond,n,size,alias=key;a=initial(key,guest);r=regoff(key);iteration=0;history=u;calls=0;prestate=get(a,r+16)
 def external(i):
  nonlocal calls
  calls+=1;post=u^(i&1)
  for o,v in ((0,u+i),(8,(cond^i)&1),(16,post),(32,~i),(40,i+100),(48,0x8888+i)):put(a,r+o,v)
  put(a,r+56,get(a,r+56)+u+i+1);put(a,GUEST+80,u+i+0x55);put(a,GUEST+912,target)
  if post<64:put(a,GUEST+512+8*post,44 if i&1 else target)
 while True:
  if fault!='bypass_call':external(iteration)
  post=get(a,r+16);choose=get(a,r+8)&1;side=get(a,r+48)
  state=50 if fault=='force_known' else 50 if choose else prestate if fault=='stale_register' else post
  if state>=64:kind=5;answer=state^FAILURE;observed=state;tag=9;break
  address=512+state*8
  if address+8>size:kind=5;answer=address^FAILURE;observed=address;tag=10;break
  if mode=='call_inside':external(iteration+16)
  forced=777 if u&1 else 99999
  if mode=='table_before':put(a,GUEST+address,forced)
  loaded=get(a,GUEST+address)
  if mode=='table_after':put(a,GUEST+address,forced)
  selected=get(a,GUEST+address) if fault=='reload' else loaded
  tag={43:1,44:1,99:2,127:3,128:4,255:5,777:6}.get(selected,1)
  if fault=='drop_fallback' and selected==777:tag=1
  put(a,GUEST+128,get(a,GUEST+128)+tag+loaded);answer=loaded^side^history^tag^u;observed=loaded
  if iteration>=n:kind=1;break
  iteration+=1;history=loaded
 for o,v in ((0,answer),(16,iteration),(24,side),(32,history),(40,state),(48,observed)):put(a,r+o,v)
 for i,v in enumerate((kind,0,0xb000,observed,int(kind==1),u,tag)):put(a,EXIT+8*i,v)
 return struct.pack('<QQ',answer,calls)+a

def same(a,b,label):
 if a!=b:raise AssertionError((label,next((i for i,(x,y) in enumerate(zip(a,b)) if x!=y),None),len(a),len(b)))

def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'helper.c').write_text(HELPER);(directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
def run(before,after,wrong,out,opt):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();address=ctypes.addressof(arena);guest=address+GUEST
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,key):
  ctypes.memmove(address,bytes(initial(key,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'business_calls');calls.value=0
  result=fn(address+regoff(key),address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory,modes in (('before',before,MODES),('after',after,MODES),('wrong',wrong,('plain',))):
   for mode in modes:
    lib,fn=compile_ir(directory/(mode+'.ll'),stage+'-'+mode)
    with gzip.open(out/(stage+'-'+mode+'.snapshots.gz'),'wb') as stream:
     for key in INPUTS:
      if key[0]!=mode:continue
      actual=invoke(lib,fn,key);stream.write(actual);expected=model(key,guest);same(actual,expected,(stage,key));report['normal']+=1
      for offset in (0,8,16+regoff(key),16+GUEST+512)+tuple(16+EXIT+i*8 for i in range(7)):
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
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('wrong',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=p.parse_args()
 if a.action=='emit':emit(a.directory)
 else:run(a.before,a.after,a.wrong,a.output,a.opt)
if __name__=='__main__':main()
