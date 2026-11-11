#!/usr/bin/env python3
"""Independent extended target-set fixture; prior frozen model files remain unchanged."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
MODES=('plain','table_before','table_after','call_before','call_inside')
VALUES=(0,1,1<<63,(1<<64)-1);TARGETS=(43,44,99,127,128,255,777,99999)
INPUTS=tuple((m,x,n,t,short) for m in MODES for x in VALUES for n in range(4) for t in TARGETS for short in (False,True))
FAULTS={'trust_hint':'plain','wrong_hit':'plain','reload':'table_after','drop_store':'table_before','wrong_phi':'plain'}
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500;CONFIG=0x600;FAILURE=0xbad0000000000000
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t business_calls;
void business_unknown(unsigned char *regs,unsigned char *guest,uint64_t x){
 uint64_t v,r; ++business_calls;v=x&1;memcpy(guest+264,&v,8);
 v=(x&1)?99:43;memcpy(guest+512,&v,8);memcpy(guest+520,&v,8);
 memcpy(&v,guest+1024,8);v^=x;memcpy(guest+1024,&v,8);
 memcpy(&r,regs+56,8);r+=1;memcpy(regs+56,&r,8);
}
'''
def finish(label,kind,answer,observed,tag,fault):
 s=label+':\n'
 for offset,value in ((0,answer),(16,'%iteration'),(24,'%fresh'),(32,'%used_history'),(40,'%oldtarget'),(48,observed)):
  s+=f'  %{label}.r{offset} = getelementptr i8, ptr %regs, i64 {offset}\n  store i64 {value}, ptr %{label}.r{offset}, align 1\n'
 for i,value in enumerate((str(kind),'0',str(0x9000+(fault=='wrong_exit')),observed,'1' if kind==1 else '0','%x',tag)):
  s+=f'  %{label}.e{i} = getelementptr i8, ptr %exit, i64 {8*i}\n  store i64 {value}, ptr %{label}.e{i}, align 1\n'
 return s+f'  ret i64 {answer}\n'
def _original_shape(mode,fault=None):
 s='''source_filename = "guarded-business-independent"
target datalayout = "e-p:64:64"
declare void @business_unknown(ptr,ptr,i64)
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
  %forced_state = xor i64 %oddvalue, 1
  %freshptr = getelementptr i8, ptr %guest, i64 1024
  %traceptr = getelementptr i8, ptr %guest, i64 288
  %accptr = getelementptr i8, ptr %guest, i64 128
  %aptr = getelementptr i8, ptr %guest, i64 256
  %bptr = getelementptr i8, ptr %guest, i64 264
  %unrelatedptr = getelementptr i8, ptr %guest, i64 280
  %t0ptr = getelementptr i8, ptr %guest, i64 512
  %t1ptr = getelementptr i8, ptr %guest, i64 520
  store i64 %oddvalue, ptr %aptr, align 1
  br label %head
head:
  %iteration = phi i64 [ 0, %entry ], [ %next, %seed ]
  %history = phi i64 [ %x, %entry ], [ %fresh, %seed ]
  %oldtarget = phi i64 [ %x, %entry ], [ %loaded, %seed ]
  %rawfresh = load i64, ptr %freshptr, align 1
  %freshbits = xor i64 %rawfresh, %iteration
'''
 s+='  %fresh = freeze i64 '+('%history' if fault=='stale_fresh' else '%freshbits')+'\n'
 s+='  %used_history = add i64 '+('%fresh' if fault=='fresh_history' else '%history')+', 0\n  store i64 %used_history, ptr %traceptr, align 1\n  br label %state_check\n'
 s+='state_check:\n  %state_room = icmp uge i64 %size, 272\n  br i1 %state_room, label %copy, label %state_failure\ncopy:\n  %state_a = load i64, ptr %aptr, align 1\n'
 if fault!='drop_copy':s+='  store i64 %state_a, ptr %bptr, align 1\n'
 s+='  br label %mutate\nmutate:\n'
 if mode=='state_after':s+='  store i64 %forced_state, ptr %bptr, align 1\n'
 if mode=='call_path':s+='  call void @business_unknown(ptr %regs, ptr %guest, i64 %x)\n'
 s+='  br label %state_load\nstate_load:\n  %state_b = load i64, ptr %bptr, align 1\n  %state_valid = icmp ult i64 %state_b, 3\n  br i1 %state_valid, label %index, label %state_failure\n'
 s+='index:\n  %scaled = shl i64 %state_b, 3\n  %table_index = add i64 %scaled, 512\n  %tableptr = getelementptr i8, ptr %guest, i64 %table_index\n  br label %bounds\nbounds:\n  %end = add i64 %table_index, 8\n  %table_room = icmp ule i64 %end, %size\n  br i1 %table_room, label %dispatch, label %table_failure\ndispatch:\n'
 if mode=='table_before':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 s+='  %loaded = load i64, ptr %tableptr, align 1\n'
 if mode=='table_after':s+='  store i64 %forced, ptr %tableptr, align 1\n'
 selector='%loaded'
 if fault=='trust_hint':selector='44'
 if fault=='reload':s+='  %reloaded = load i64, ptr %tableptr, align 1\n';selector='%reloaded'
 if fault=='wrong_phi':s+='  %wrong_routed = add i64 %loaded, 1\n'
 s+='  switch i64 '+selector+', label %miss [\n    i64 43, label %good\n    i64 44, label %'+('alt' if fault=='wrong_hit' else 'good')+'\n    i64 99, label %alt\n  ]\ngood:\n'
 routed='%wrong_routed' if fault=='wrong_phi' else '%loaded';count=1 if fault=='wrong_hit' else 2
 s+='  %selected = phi i64 '+', '.join(f'[ {routed}, %dispatch ]' for _ in range(count))+'\n  br label %step\nalt:\n  br label %step\nmiss:\n  br label %step\nstep:\n'
 s+='  %tag = phi i64 [ 1, %good ], [ 2, %alt ], [ 3, %miss ]\n  %routed = phi i64 [ %selected, %good ], [ %loaded, %alt ], [ %loaded, %miss ]\n'
 s+='  %acc = load i64, ptr %accptr, align 1\n  %accplus = add i64 %acc, %tag\n  %accnext = add i64 %accplus, %routed\n  store i64 %accnext, ptr %accptr, align 1\n'
 s+='  %r0 = xor i64 %routed, %fresh\n  %r1 = xor i64 %r0, %used_history\n  %r2 = xor i64 %r1, %oldtarget\n  %result = xor i64 %r2, %tag\n  %again = icmp ult i64 %iteration, %limit\n  br i1 %again, label %seed, label %finish\nseed:\n'
 s+='  %next = add i64 %iteration, 1\n  %newbits = add i64 %next, 85\n  %newfresh = xor i64 %x, %newbits\n  store i64 %newfresh, ptr %freshptr, align 1\n'
 s+='  %choice0 = add i64 %next, %x\n  %choice = and i64 %choice0, 3\n  %is0 = icmp eq i64 %choice, 0\n  %is1 = icmp eq i64 %choice, 1\n  %is2 = icmp eq i64 %choice, 2\n  %v2 = select i1 %is2, i64 99, i64 777\n  %v1 = select i1 %is1, i64 44, i64 %v2\n  %nexttarget = select i1 %is0, i64 43, i64 %v1\n  store i64 %nexttarget, ptr %t1ptr, align 1\n  store i64 1, ptr %aptr, align 1\n'
 if mode=='unrelated':s+='  store i64 2, ptr %unrelatedptr, align 1\n'
 s+='  br label %head\n'
 s+=finish('finish',1,'%result','%routed','%tag',fault)
 s+='state_failure:\n  %state_bad = xor i64 256, '+str(FAILURE)+'\n  br label %state_badfinish\n'+finish('state_badfinish',5,'%state_bad','256','3',fault)
 s+='table_failure:\n  %table_bad = xor i64 %table_index, '+str(FAILURE)+'\n  br label %table_badfinish\n'+finish('table_badfinish',5,'%table_bad','%table_index','3',fault)
 return s+'}\n'
# Explicitly extended helper semantics: writes can return central/unknown values.
HELPER=HELPER.replace('v=(x&1)?99:43','v=(x&1)?777:99999')
def ir(mode,fault=None):
 assert mode in MODES
 source_mode='call_path' if mode=='call_before' else 'plain' if mode=='call_inside' else mode
 s=_original_shape(source_mode)
 s=s.replace('%forced = select i1 %odd, i64 99, i64 43','%forced = select i1 %odd, i64 777, i64 99999')
 if mode=='call_inside':s=s.replace('dispatch:\n','dispatch:\n  call void @business_unknown(ptr %regs, ptr %guest, i64 %x)\n')
 begin=s.index('  switch i64 %loaded, label %miss [');end=s.index('  %acc = load i64, ptr %accptr',begin)
 selector='43' if fault=='trust_hint' else '%reloaded' if fault=='reload' else '%loaded'
 extra=''
 if fault=='reload':extra+='  %reloaded = load i64, ptr %tableptr, align 1\n'
 if fault=='wrong_phi':extra+='  %wrong_routed = add i64 %loaded, 1\n'
 extra+='  switch i64 '+selector+', label %good [\n'
 for value,target in ((43,'alt' if fault=='wrong_hit' else 'good'),(44,'good'),(99,'alt'),(127,'c127'),(128,'c128'),(255,'c255'),(777,'central')):
  extra+=f'    i64 {value}, label %{target}\n'
 extra+='  ]\ngood:\n'
 incoming='%wrong_routed' if fault=='wrong_phi' else '%loaded'
 extra+='  %selected = phi i64 '+', '.join(f'[ {incoming}, %dispatch ]' for _ in range(2 if fault=='wrong_hit' else 3))+'\n  br label %step\n'
 for label in ('alt','c127','c128','c255','central'):extra+=label+':\n  br label %step\n'
 extra+='step:\n  %tag = phi i64 [ 1, %good ], [ 2, %alt ], [ 3, %c127 ], [ 4, %c128 ], [ 5, %c255 ], [ 6, %central ]\n'
 extra+='  %routed = phi i64 [ %selected, %good ], [ %loaded, %alt ], [ %loaded, %c127 ], [ %loaded, %c128 ], [ %loaded, %c255 ], [ %loaded, %central ]\n'
 s=s[:begin]+extra+s[end:]
 if fault=='drop_store':
  assert mode=='table_before';old='  store i64 %forced, ptr %tableptr, align 1\n';assert s.count(old)==1;s=s.replace(old,'')
 begin=s.index('  %choice0 = add i64 %next, %x');end=s.index('  store i64 %nexttarget, ptr %t1ptr',begin)
 choose='  %choice0 = add i64 %next, %x\n  %choice = and i64 %choice0, 7\n'
 for i in range(7):choose+=f'  %is{i} = icmp eq i64 %choice, {i}\n'
 for i in range(6,-1,-1):
  name='%nexttarget' if i==0 else f'%v{i}';other=str(TARGETS[7]) if i==6 else f'%v{i+1}'
  choose+=f'  {name} = select i1 %is{i}, i64 {TARGETS[i]}, i64 {other}\n'
 s=s[:begin]+choose+s[end:]
 return s

def put(a,o,v):struct.pack_into('<Q',a,o,v&MASK)
def get(a,o):return struct.unpack_from('<Q',a,o)[0]
def initial(key,guest):
 mode,x,n,target,short=key;a=bytearray((i*37+11)&255 for i in range(ARENA))
 put(a,REGS+8,x);put(a,REGS+16,n);put(a,DESC,guest);put(a,DESC+8,511 if short else 65536)
 put(a,GUEST+512,target);put(a,GUEST+520,TARGETS[(TARGETS.index(target)+1)%8]);put(a,GUEST+528,99)
 put(a,GUEST+264,2);put(a,GUEST+1024,x^0x1122334455667788);put(a,GUEST+128,0)
 return a

def model(key,guest,fault=None):
 mode,x,n,target,short=key;a=initial(key,guest);iteration=0;history=oldtarget=x;calls=0;put(a,GUEST+256,x&1)
 def unknown_call():
  nonlocal calls
  calls+=1;put(a,GUEST+264,x&1)
  value=777 if x&1 else 99999
  put(a,GUEST+512,value);put(a,GUEST+520,value);put(a,GUEST+1024,get(a,GUEST+1024)^x);put(a,REGS+56,get(a,REGS+56)+1)
 while True:
  fresh=get(a,GUEST+1024)^iteration;used=history;put(a,GUEST+288,used)
  put(a,GUEST+264,get(a,GUEST+256))
  if mode=='call_before':unknown_call()
  address=512+8*get(a,GUEST+264)
  if short:answer=address^FAILURE;kind=5;routed=address;tag=3;break
  if mode=='call_inside':unknown_call()
  forced=777 if x&1 else 99999
  if mode=='table_before' and fault!='drop_store':put(a,GUEST+address,forced)
  loaded=get(a,GUEST+address)
  if mode=='table_after':put(a,GUEST+address,forced)
  selected=43 if fault=='trust_hint' else get(a,GUEST+address) if fault=='reload' else loaded
  tag={43:1,44:1,99:2,127:3,128:4,255:5,777:6}.get(selected,1)
  if fault=='wrong_hit' and selected==43:tag=2
  routed=(loaded+1)&MASK if fault=='wrong_phi' and tag==1 else loaded
  put(a,GUEST+128,get(a,GUEST+128)+tag+routed);answer=routed^fresh^used^oldtarget^tag
  if iteration>=n:kind=1;break
  next_iteration=iteration+1;put(a,GUEST+1024,x^(next_iteration+85));put(a,GUEST+520,TARGETS[(next_iteration+x)&7]);put(a,GUEST+256,1)
  iteration=next_iteration;history=fresh;oldtarget=loaded
 for o,v in ((0,answer),(16,iteration),(24,fresh),(32,used),(40,oldtarget),(48,routed)):put(a,REGS+o,v)
 for i,v in enumerate((kind,0,0x9000,routed,int(kind==1),x,tag)):put(a,EXIT+i*8,v)
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
  result=fn(address+REGS,address+DESC,address+EXIT,address+CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for stage,directory in (('before',before),('set6',after),('wronghint',wrong)):
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
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('wrong',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=p.parse_args()
 if a.action=='emit':emit(a.directory)
 else:run(a.before,a.after,a.wrong,a.output,a.opt)
if __name__=='__main__':main()
