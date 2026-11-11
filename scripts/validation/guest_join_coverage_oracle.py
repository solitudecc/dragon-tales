#!/usr/bin/env python3
"""Independent whole-state oracle for complete GuestSSA PHI incoming edges.
Invalid candidate IR is evidence only, never compiled or executed.
"""
import argparse,ctypes,gzip,hashlib,json,struct,subprocess
from pathlib import Path
MODES=('stable','changed','self','nested','unknown_call','ordinary','duplicate_join','unreachable_pred','duplicate_backedge')
VALUES=(0,1,1<<63,(1<<64)-1)
INPUTS=tuple((mode,x,n,path) for mode in MODES for x in VALUES for n in range(4) for path in (0,1))
FAULTS={'stale_load':'changed','drop_store':'self','wrong_entry':'stable','drop_call':'unknown_call','wrong_phi':'duplicate_join','wrong_exit':'ordinary'}
MASK=(1<<64)-1;ARENA=0x12000;REGS=0x100;GUEST=0x1000;EXIT=0x400;DESC=0x500
HELPER='''#include <stdint.h>
#include <string.h>
uint64_t join_calls;
void join_unknown(unsigned char *regs,unsigned char *guest,uint64_t x){
 uint64_t v,r;++join_calls;memcpy(&v,guest+64,8);v+=(x|UINT64_C(1));memcpy(guest+64,&v,8);
 memcpy(&r,regs+56,8);r^=x;memcpy(regs+56,&r,8);
}
'''
def finish(mode,fault):
 s='done_output:\n  %result = xor i64 %sum, %last\n'
 for offset,value in ((0,'%result'),(16,'%done_count'),(32,'%sum'),(40,'%last')):
  s+=f'  %r{offset} = getelementptr i8, ptr %regs, i64 {offset}\n  store i64 {value}, ptr %r{offset}, align 1\n'
 fields=('1','0',str(4096+MODES.index(mode)+(1 if fault=='wrong_exit' else 0)),'%result','1','%done_count','%last')
 for i,value in enumerate(fields):s+=f'  %e{i} = getelementptr i8, ptr %exit, i64 {i*8}\n  store i64 {value}, ptr %e{i}, align 1\n'
 return s+'  ret i64 %result\n}\n'
def ir(mode,fault=None):
 assert mode in MODES
 s='''source_filename = "guest-join-coverage-independent"
target datalayout = "e-p:64:64"
declare void @join_unknown(ptr,ptr,i64)
define i64 @oracle(ptr %regs, ptr %memory, ptr %exit, ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %xp = getelementptr i8, ptr %regs, i64 8
  %x = load i64, ptr %xp, align 1
  %np = getelementptr i8, ptr %regs, i64 16
  %n0 = load i64, ptr %np, align 1
  %n = and i64 %n0, 3
  %pp = getelementptr i8, ptr %regs, i64 24
  %path = load i64, ptr %pp, align 1
  %choose = icmp ne i64 %path, 0
  %vleft = xor i64 %x, 14
  %vright = xor i64 %x, 34
  %initial = select i1 %choose, i64 %vright, i64 %vleft
  %step = or i64 %x, 1
  %backvalue = xor i64 %x, 99
  %slot = getelementptr i8, ptr %guest, i64 64
'''
 join=mode in ('ordinary','duplicate_join','unreachable_pred')
 if mode=='duplicate_backedge':s+='  store i64 %initial, ptr %slot, align 1\n  br label %header\n'
 else:
  s+='  br i1 %choose, label %right, label %left\nleft:\n  store i64 %vleft, ptr %slot, align 1\n'
  target='join' if join else 'header'
  s+=('  switch i64 %x, label %join [ i64 1, label %join ]\n' if mode=='duplicate_join' else f'  br label %{target}\n')
  s+='right:\n  store i64 '+('%vleft' if fault=='wrong_entry' else '%vright')+', ptr %slot, align 1\n'+f'  br label %{target}\n'
 if join:
  if mode=='unreachable_pred':s+='dead:\n  store i64 99, ptr %slot, align 1\n  br label %join\n'
  s+='join:\n'
  if fault=='wrong_phi':s+='  %loaded = phi i64 [ %vleft, %left ], [ %vleft, %left ], [ %vleft, %right ]\n'
  else:s+='  %loaded = load i64, ptr %slot, align 1\n'
  s+='  %sum = add i64 %loaded, 0\n  %last = add i64 %loaded, 0\n  %done_count = add i64 0, 0\n  br label %done_output\n'
  return s+finish(mode,fault)
 incoming='[ 0, %entry ]' if mode=='duplicate_backedge' else '[ 0, %left ], [ 0, %right ]'
 back='header' if mode=='self' else 'body'
 if mode=='nested':back='outer_latch'
 multi=2 if mode=='duplicate_backedge' else 1
 s+='header:\n  %i = phi i64 '+incoming+''.join(f', [ %next, %{back} ]' for _ in range(multi))+'\n'
 s+='  %total = phi i64 '+incoming+''.join(f', [ %total_now, %{back} ]' for _ in range(multi))+'\n'
 s+=('  %loaded = add i64 %initial, 0\n' if fault=='stale_load' else '  %loaded = load i64, ptr %slot, align 1\n')
 s+='  %total_now = add i64 %total, %loaded\n  %next = add i64 %i, 1\n  %more = icmp ult i64 %i, %n\n'
 if mode=='self':
  s+='  %updated = add i64 %loaded, %step\n'
  if fault!='drop_store':s+='  store i64 %updated, ptr %slot, align 1\n'
  s+='  br i1 %more, label %header, label %finish\n'
 else:
  s+='  br i1 %more, label %body, label %finish\nbody:\n'
  if mode=='changed':
   s+='  %updated = add i64 %loaded, %step\n'
   if fault!='drop_store':s+='  store i64 %updated, ptr %slot, align 1\n'
  elif mode=='unknown_call':
   if fault!='drop_call':s+='  call void @join_unknown(ptr %regs, ptr %guest, i64 %x)\n'
  elif mode=='duplicate_backedge':s+='  store i64 %backvalue, ptr %slot, align 1\n'
  if mode=='nested':
   s+='''  br label %inner
inner:
  %j = phi i64 [ 0, %body ], [ %jnext, %inner_body ]
  %inner_more = icmp ult i64 %j, %n
  br i1 %inner_more, label %inner_body, label %outer_latch
inner_body:
  %inner_load = load i64, ptr %slot, align 1
  %inner_new = add i64 %inner_load, %step
  store i64 %inner_new, ptr %slot, align 1
  %jnext = add i64 %j, 1
  br label %inner
outer_latch:
  br label %header
'''
  elif mode=='duplicate_backedge':s+='  br i1 %choose, label %header, label %header\n'
  else:s+='  br label %header\n'
 s+='finish:\n  %sum = add i64 %total_now, 0\n  %last = add i64 %loaded, 0\n  %done_count = add i64 %i, 0\n  br label %done_output\n'
 return s+finish(mode,fault)
def put(a,offset,value):struct.pack_into('<Q',a,offset,value&MASK)
def get(a,offset):return struct.unpack_from('<Q',a,offset)[0]
def initial(key,guest_address):
 mode,x,n,path=key;a=bytearray((i*37+11)&255 for i in range(ARENA))
 for offset,value in ((8,x),(16,n),(24,path)):put(a,REGS+offset,value)
 put(a,DESC,guest_address);put(a,DESC+8,65536)
 return a
def model(key,guest_address,fault=None):
 mode,x,n,path=key;a=initial(key,guest_address);calls=0
 first=x^(34 if path and fault!='wrong_entry' else 14);put(a,GUEST+64,first)
 original=x^(34 if path else 14);step=x|1;total=0;done=0
 if mode in ('ordinary','duplicate_join','unreachable_pred'):
  last=x^14 if fault=='wrong_phi' else first;total=last
 else:
  for i in range(n+1):
   last=original if fault=='stale_load' else get(a,GUEST+64);total=(total+last)&MASK;done=i
   if mode=='self' and fault!='drop_store':put(a,GUEST+64,last+step)
   if i==n:break
   if mode=='changed' and fault!='drop_store':put(a,GUEST+64,last+step)
   if mode=='unknown_call' and fault!='drop_call':
    calls+=1;put(a,GUEST+64,get(a,GUEST+64)+step);put(a,REGS+56,get(a,REGS+56)^x)
   if mode=='duplicate_backedge':put(a,GUEST+64,x^99)
   if mode=='nested':
    for j in range(n):put(a,GUEST+64,get(a,GUEST+64)+step)
 result=total^last
 for offset,value in ((0,result),(16,done),(32,total),(40,last)):put(a,REGS+offset,value)
 for i,value in enumerate((1,0,4096+MODES.index(mode)+(fault=='wrong_exit'),result,1,done,last)):put(a,EXIT+i*8,value)
 return struct.pack('<QQ',result,calls)+a
def same(actual,expected,label):
 if actual!=expected:
  first=next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)
  raise AssertionError((label,first,len(actual),len(expected)))
def emit(directory):
 directory.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(directory/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(directory/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 (directory/'helper.c').write_text(HELPER)
 (directory/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (directory/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')
 (directory/'CONTRACT.json').write_text(json.dumps({'modes':MODES,'inputs':len(INPUTS),'faults':FAULTS,'snapshot_bytes':16+ARENA,
  'scope':'Generic host DL64 SSA transform; full arena includes descriptor pointer from actual allocation. No normalization.',
  'old_invalid_expected':['stable','changed','self','nested','unknown_call','duplicate_join','unreachable_pred','duplicate_backedge'],
  'old_valid_compatibility':['ordinary'],'invalid_ir_must_never_execute':True,'exclusions':[]},indent=2)+'\n')
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
  ctypes.memmove(address,bytes(initial(key,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'join_calls');calls.value=0
  result=fn(address+REGS,address+DESC,address+EXIT,address+0x600)
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
def audit(root):
 report={'streams':0,'frames':0,'native_executions_added':0,'errors':[]}
 try:
  for opt in (0,2):
   folder=root/f'results-O{opt}';stats=json.loads((folder/'report.json').read_text());assert not stats['errors'];guest=stats['guest_address'];expectednames=set()
   assert stats['normal']==576 and stats['fault_observations']==192 and stats['comparator_controls']==6336
   for stage in ('before','after'):
    for mode in MODES:
     name=stage+'-'+mode+'.snapshots.gz';expectednames.add(name)
     with gzip.open(folder/name,'rb') as stream:
      for key in INPUTS:
       if key[0]!=mode:continue
       same(stream.read(16+ARENA),model(key,guest),(opt,stage,key));report['frames']+=1
      assert not stream.read(1)
     report['streams']+=1
   for fault,mode in FAULTS.items():
    name='control-'+fault+'.snapshots.gz';expectednames.add(name);required=caught=0
    with gzip.open(folder/name,'rb') as stream:
     for key in INPUTS:
      if key[0]!=mode:continue
      actual=stream.read(16+ARENA);expected=model(key,guest);damaged=model(key,guest,fault);same(actual,damaged,(opt,fault,key));report['frames']+=1
      if damaged!=expected:
       required+=1
       try:same(actual,expected,'saved fault')
       except AssertionError:caught+=1
       else:raise AssertionError('saved fault escaped')
     assert not stream.read(1)
    assert stats['faults'][fault]=={'required':required,'caught':caught} and required==caught>0;report['streams']+=1
   assert {p.name for p in folder.glob('*.snapshots.gz')}==expectednames
  assert report['streams']==48 and report['frames']==1536
 except Exception as error:report['errors'].append(repr(error));raise
 finally:(root/'saved-audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
def main():
 p=argparse.ArgumentParser();s=p.add_subparsers(dest='action',required=True)
 e=s.add_parser('emit');e.add_argument('directory',type=Path)
 r=s.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=s.add_parser('audit');a.add_argument('root',type=Path);x=p.parse_args()
 if x.action=='emit':emit(x.directory)
 elif x.action=='run':run(x.before,x.after,x.output,x.opt)
 else:audit(x.root)
if __name__=='__main__':main()
