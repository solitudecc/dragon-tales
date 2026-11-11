#!/usr/bin/env python3
"""Independent full-state model for dynamically guarded transition copies."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
MODES=('plain','store_before','store_after','side_reentry','seed_call','duplicate_seed')
VALUES=(0,1,1<<63,(1<<64)-1)
INPUTS=tuple((m,x,n,target,short) for m in MODES for x in VALUES for n in range(4) for target in (43,44,99,777) for short in (False,True))
FAULTS={'trust_hint':'plain','wrong_hit':'plain','reload':'store_after','drop_store':'store_before','stale_fresh':'plain','fresh_history':'plain','wrong_phi':'plain','wrong_exit':'plain'}
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500;CONFIG=0x600;FAILURE=0xbad0000000000000
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t transition_calls;
void transition_unknown(unsigned char *regs,unsigned char *guest,uint64_t x){
 uint64_t v,r; ++transition_calls;v=(x&1)?99:43;memcpy(guest+512,&v,8);
 memcpy(&v,guest+1024,8);v^=x;memcpy(guest+1024,&v,8);
 memcpy(&r,regs+56,8);r+=1;memcpy(regs+56,&r,8);
}
'''
def finish(label,kind,answer,observed,tag,fault):
 s=label+':\n'
 fields=((0,answer),(16,'%iteration'),(24,'%fresh'),(32,'%used_history'),(40,'%oldtarget'),(48,observed))
 for offset,value in fields:s+=f'  %{label}.r{offset} = getelementptr i8, ptr %regs, i64 {offset}\n  store i64 {value}, ptr %{label}.r{offset}, align 1\n'
 ex=(str(kind),'0',str(0x9000+(fault=='wrong_exit')),observed,'1' if kind==1 else '0','%x',tag)
 for i,value in enumerate(ex):s+=f'  %{label}.e{i} = getelementptr i8, ptr %exit, i64 {8*i}\n  store i64 {value}, ptr %{label}.e{i}, align 1\n'
 return s+f'  ret i64 {answer}\n'
def ir(mode,fault=None,call_in_path=False):
 s='''source_filename = "guarded-transition-independent"
target datalayout = "e-p:64:64"
declare void @transition_unknown(ptr,ptr,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %xp = getelementptr i8, ptr %regs, i64 8
  %x = load i64, ptr %xp, align 1
  %np = getelementptr i8, ptr %regs, i64 16
  %n0 = load i64, ptr %np, align 1
  %limit = and i64 %n0, 3
  %oddvalue = and i64 %x, 1
  %odd = icmp ne i64 %oddvalue, 0
  %forced = select i1 %odd, i64 99, i64 43
  %freshptr = getelementptr i8, ptr %guest, i64 1024
  %traceptr = getelementptr i8, ptr %guest, i64 256
  %accptr = getelementptr i8, ptr %guest, i64 128
  %sideptr = getelementptr i8, ptr %guest, i64 136
  br label %head
head:
'''
 dup=2 if mode=='duplicate_seed' else 1
 s+='  %iteration = phi i64 [ 0, %entry ]'+''.join(', [ %next, %seed ]' for _ in range(dup))+', [ %side_next, %side ]\n'
 s+='  %history = phi i64 [ %x, %entry ]'+''.join(', [ %fresh, %seed ]' for _ in range(dup))+', [ %fresh, %side ]\n'
 s+='  %oldtarget = phi i64 [ %x, %entry ]'+''.join(', [ %loaded, %seed ]' for _ in range(dup))+', [ %oldtarget, %side ]\n'
 s+='  %rawfresh = load i64, ptr %freshptr, align 1\n  %freshbits = xor i64 %rawfresh, %iteration\n'
 s+='  %fresh = freeze i64 '+('%history' if fault=='stale_fresh' else '%freshbits')+'\n'
 s+='  %used_history = add i64 '+('%fresh' if fault=='fresh_history' else '%history')+', 0\n  store i64 %used_history, ptr %traceptr, align 1\n'
 s+='  %atone = icmp eq i64 %iteration, 1\n  %sidewanted = and i1 %atone, %odd\n'
 s+='  %takeSide = and i1 %sidewanted, '+('true' if mode=='side_reentry' else 'false')+'\n'
 s+='  %continuepath = xor i1 %takeSide, true\n  br i1 %continuepath, label %check, label %side\ncheck:\n'
 s+='  %room = icmp uge i64 %size, 520\n  br i1 %room, label %write, label %failure\nwrite:\n'
 if call_in_path:s+='  call void @transition_unknown(ptr %regs,ptr %guest,i64 %x)\n'
 s+='  %tableptr = getelementptr i8, ptr %guest, i64 512\n'
 if mode=='store_before' and fault!='drop_store':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 s+='  br label %dispatch\ndispatch:\n  %loaded = load i64, ptr %tableptr, align 1\n'
 # The controller's constant-byte-GEP contract requires the terminal LOAD's
 # pointer to be a GEP. Its definition may be in the previous copied block.
 if mode=='store_after':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 selector='%loaded'
 if fault=='trust_hint':selector='43'
 if fault=='reload':s+='  %reloaded = load i64, ptr %tableptr, align 1\n';selector='%reloaded'
 if fault=='wrong_phi':s+='  %wrong_routed = add i64 %loaded, 1\n'
 s+='  switch i64 '+selector+', label %miss [\n    i64 43, label %'+('alt' if fault=='wrong_hit' else 'good')+'\n    i64 44, label %good\n    i64 99, label %alt\n  ]\ngood:\n'
 routed='%wrong_routed' if fault=='wrong_phi' else '%loaded'
 count=1 if fault=='wrong_hit' else 2
 s+='  %selected = phi i64 '+', '.join(f'[ {routed}, %dispatch ]' for _ in range(count))+'\n  br label %step\nalt:\n  br label %step\nmiss:\n  br label %step\nstep:\n'
 s+='  %tag = phi i64 [ 1, %good ], [ 2, %alt ], [ 3, %miss ]\n  %routed = phi i64 [ %selected, %good ], [ %loaded, %alt ], [ %loaded, %miss ]\n'
 s+='  %acc = load i64, ptr %accptr, align 1\n  %accplus = add i64 %acc, %tag\n  %accnext = add i64 %accplus, %routed\n  store i64 %accnext, ptr %accptr, align 1\n'
 s+='  %r0 = xor i64 %routed, %fresh\n  %r1 = xor i64 %r0, %used_history\n  %r2 = xor i64 %r1, %oldtarget\n  %result = xor i64 %r2, %tag\n'
 s+='  %again = icmp ult i64 %iteration, %limit\n  br i1 %again, label %seed, label %finish\nseed:\n'
 s+='  %next = add i64 %iteration, 1\n  %newbits = add i64 %next, 85\n  %newfresh = xor i64 %x, %newbits\n  store i64 %newfresh, ptr %freshptr, align 1\n'
 s+='  %choice0 = add i64 %next, %x\n  %choice = and i64 %choice0, 3\n  %is0 = icmp eq i64 %choice, 0\n  %is1 = icmp eq i64 %choice, 1\n  %is2 = icmp eq i64 %choice, 2\n  %v2 = select i1 %is2, i64 99, i64 777\n  %v1 = select i1 %is1, i64 44, i64 %v2\n  %nexttarget = select i1 %is0, i64 43, i64 %v1\n'
 s+='  %seedptr = getelementptr i8, ptr %guest, i64 512\n  store i64 %nexttarget, ptr %seedptr, align 1\n'
 if mode=='seed_call':s+='  call void @transition_unknown(ptr %regs,ptr %guest,i64 %x)\n'
 s+=('  br i1 %odd, label %head, label %head\n' if mode=='duplicate_seed' else '  br label %head\n')
 s+='side:\n  %side_next = add i64 %iteration, 1\n  %sidecount = load i64, ptr %sideptr, align 1\n  %sideinc = add i64 %sidecount, 1\n  store i64 %sideinc, ptr %sideptr, align 1\n  %sidefresh = xor i64 %rawfresh, 40503\n  store i64 %sidefresh, ptr %freshptr, align 1\n  br label %head\n'
 s+=finish('finish',1,'%result','%routed','%tag',fault)
 s+='failure:\n  %bad = xor i64 512, '+str(FAILURE)+'\n  br label %badfinish\n'+finish('badfinish',5,'%bad','512','3',fault)
 return s+'}\n'
def put(a,o,v):struct.pack_into('<Q',a,o,v&MASK)
def get(a,o):return struct.unpack_from('<Q',a,o)[0]
def initial(key,guest):
 mode,x,n,target,short=key;a=bytearray((i*37+11)&255 for i in range(ARENA))
 put(a,REGS+8,x);put(a,REGS+16,n);put(a,DESC,guest);put(a,DESC+8,511 if short else 65536)
 put(a,GUEST+512,target);put(a,GUEST+1024,x^0x1122334455667788);put(a,GUEST+128,0);put(a,GUEST+136,0)
 return a
def model(key,guest,fault=None):
 mode,x,n,target,short=key;a=initial(key,guest);iteration=0;history=oldtarget=x;calls=0
 while True:
  raw=get(a,GUEST+1024);fresh=history if fault=='stale_fresh' else raw^iteration
  used=fresh if fault=='fresh_history' else history;put(a,GUEST+256,used)
  if mode=='side_reentry' and iteration==1 and x&1:
   put(a,GUEST+136,get(a,GUEST+136)+1);put(a,GUEST+1024,raw^40503);iteration+=1;history=fresh;continue
  if short:answer=512^FAILURE;kind=5;routed=512;tag=3;break
  forced=99 if x&1 else 43
  if mode=='store_before' and fault!='drop_store':put(a,GUEST+512,forced)
  loaded=get(a,GUEST+512)
  if mode=='store_after':put(a,GUEST+512,forced)
  selected=43 if fault=='trust_hint' else get(a,GUEST+512) if fault=='reload' else loaded
  tag=1 if selected in (43,44) else 2 if selected==99 else 3
  if fault=='wrong_hit' and selected==43:tag=2
  routed=(loaded+1)&MASK if fault=='wrong_phi' and tag==1 else loaded
  put(a,GUEST+128,get(a,GUEST+128)+tag+routed)
  answer=routed^fresh^used^oldtarget^tag
  if iteration>=n:kind=1;break
  next_iteration=iteration+1;put(a,GUEST+1024,x^(next_iteration+85));put(a,GUEST+512,(43,44,99,777)[(next_iteration+x)&3])
  if mode=='seed_call':
   calls+=1;put(a,GUEST+512,forced);put(a,GUEST+1024,get(a,GUEST+1024)^x);put(a,REGS+56,get(a,REGS+56)+1)
  iteration=next_iteration;history=fresh;oldtarget=loaded
 for o,v in ((0,answer),(16,iteration),(24,fresh),(32,used),(40,oldtarget),(48,routed)):put(a,REGS+o,v)
 for i,v in enumerate((kind,0,0x9000+(fault=='wrong_exit'),routed,int(kind==1),x,tag)):put(a,EXIT+i*8,v)
 return struct.pack('<QQ',answer,calls)+a
def same(a,b,label):
 if a!=b:raise AssertionError((label,next((i for i,(x,y) in enumerate(zip(a,b)) if x!=y),None),len(a),len(b)))
def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'reject_call.ll').write_text(ir('plain',call_in_path=True))
 (directory/'helper.c').write_text(HELPER);(directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
def run(before,after43,after99,out,opt):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();address=ctypes.addressof(arena);guest=address+GUEST
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,key):
  ctypes.memmove(address,bytes(initial(key,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'transition_calls');calls.value=0
  result=fn(address+REGS,address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory in (('before',before),('hint43',after43),('hint99',after99)):
   for mode in MODES:
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
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after43',type=Path);r.add_argument('after99',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=p.parse_args()
 if a.action=='emit':emit(a.directory)
 else:run(a.before,a.after43,a.after99,a.output,a.opt)
if __name__=='__main__':main()
