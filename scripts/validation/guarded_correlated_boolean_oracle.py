#!/usr/bin/env python3
"""Independent Boolean-correlation reference; actual LOAD remains authoritative."""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
import guarded_store_corridor_oracle as frozen
assert hashlib.sha256(Path(frozen.__file__).read_bytes()).hexdigest()=='10d50a43f6441f32c5df1da803df69edf4eb61c39d727a26868682b12e1d6b6c'
MODES=('shared','history','three','four','unknown_arm','freeze','table_before','table_after')
VALUES=frozen.VALUES;TARGETS=frozen.TARGETS
INPUTS=tuple((m,x,n,t,size) for m in MODES for x in VALUES for n in range(4) for t in (43,44,777,99999) for size in (263,295,583,65536))
FAULTS={'wrong_correlated':'shared','stale_bool':'history','drop_third':'three','trust_hint':'shared','reload':'table_after','drop_store':'table_before','wrong_phi':'shared'}
MASK=frozen.MASK;ARENA=frozen.ARENA;REGS=frozen.REGS;GUEST=frozen.GUEST;EXIT=frozen.EXIT;DESC=frozen.DESC;CONFIG=frozen.CONFIG;FAILURE=frozen.FAILURE
HELPER=frozen.HELPER;put=frozen.put;get=frozen.get;same=frozen.same

def ir(mode,fault=None):
 assert mode in MODES
 base_mode=mode if mode in ('table_before','table_after') else 'plain'
 s=frozen.ir(base_mode,fault if fault in ('trust_hint','reload','drop_store','wrong_phi') else None)
 s=s.replace('source_filename = "guarded-store-corridor-independent"','source_filename = "guarded-correlated-boolean-independent"')
 count=4 if mode=='four' else 3 if mode=='three' else 1
 pointers=''.join(f'  %cp{i} = getelementptr i8, ptr %config, i64 {8+i}\n' for i in range(count))
 pointers+='  %entry_c = load i1, ptr %cp0, align 1\n'
 s=s.replace('  %limit = and i64 %n0, 3',pointers+'  %limit = and i64 %n0, 3')
 if mode=='history':s=s.replace('head:\n','head:\n  %prior_c = phi i1 [ %entry_c, %initial_write ], [ %c0, %seed ], [ %c0, %seed ]\n')
 loads=''.join(f'  %c{i} = load i1, ptr %cp{i}, align 1\n' for i in range(count))
 if mode=='freeze':loads=loads.replace('%c0 = load','%raw_c0 = load')+'  %c0 = freeze i1 %raw_c0\n'
 expression=''
 if mode in ('three','four'):
  for i in range(count):
   value='false' if fault=='drop_third' and i==2 else f'%c{i}'
   expression+=f'  %b{i} = zext i1 {value} to i64\n  %w{i} = shl i64 %b{i}, {i}\n'
  expression+='  %sum1 = or i64 %w0, %w1\n  %sum2 = or i64 %sum1, %w2\n'
  expression+='  %dispatch_state = or i64 %sum2, %w3\n' if count==4 else '  %dispatch_state = add i64 %sum2, 0\n'
 elif mode=='unknown_arm':
  expression+='  %unknownptr = getelementptr i8, ptr %regs, i64 72\n  %unknown_state = load i64, ptr %unknownptr, align 1\n  %dispatch_state = select i1 %c0, i64 8, i64 %unknown_state\n'
 else:
  cond='%prior_c' if mode=='history' else '%c0'
  bit='%prior_c' if fault=='stale_bool' else '%c0'
  if fault=='wrong_correlated':expression+='  %wrong_c = xor i1 %c0, true\n';bit='%wrong_c'
  expression+=f'  %bool_arm = select i1 {cond}, i64 0, i64 8\n  %bool_bit = zext i1 {bit} to i64\n  %dispatch_state = or disjoint i64 %bool_arm, %bool_bit\n'
 s=s.replace('  %rawfresh = load i64',loads+expression+'  %rawfresh = load i64')
 s=s.replace('%state_valid = icmp ult i64 %state_b, 64','%state_valid = icmp ult i64 %dispatch_state, 64').replace('%scaled = shl i64 %state_b, 3','%scaled = shl i64 %dispatch_state, 3')
 updates=''
 for i in range(count):updates+=f'  %toggle{i} = xor i1 %c{i}, true\n  %nextbyte{i} = zext i1 %toggle{i} to i8\n  store i8 %nextbyte{i}, ptr %cp{i}, align 1\n'
 s=s.replace('  %next_state = select i1 %odd',updates+'  %next_state = select i1 %odd')
 return s

def initial(key,guest):
 a=frozen.initial(key,guest);mode,x,n,target,size=key
 for i,v in enumerate((x&1,(n>>1)&1,TARGETS.index(target)&1,(x>>63)&1)):a[CONFIG+8+i]=v
 return a

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
  elif mode=='unknown_arm':state=8 if bits[0] else get(a,REGS+72)
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
