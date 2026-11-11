#!/usr/bin/env python3
"""A new literal-switch/duplicate-edge shape using the unchanged business reference."""
import argparse,ctypes,gzip,hashlib,importlib.util,json,struct,subprocess
from pathlib import Path
REFERENCE='19bb40a10559a6ea3c385f60c590ad3c809a6e69e7fcd5e6d108c468c12888bd'
def reference(path):
 assert hashlib.sha256(path.read_bytes()).hexdigest()==REFERENCE
 spec=importlib.util.spec_from_file_location('business_reference',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
SENTINEL=123456789;OFFSET=1536

def ir(m,fault=None):
 s=m.ir('plain')
 old='  br label %head\nhead:\n'
 assert s.count(old)==1
 extra='''  switch i64 SELECTOR, label %constant.dead [
    i64 0, label %constant.dead
    i64 1, label %constant.join
    i64 2, label %constant.join
  ], !dragon.guarded-transitions.generated !0
constant.join:
  %constant.value = phi i64 [ %x, %entry ], [ %x, %entry ]
  br label %constant.branch
constant.branch:
  br i1 true, label %head, label %constant.dead, !dragon.guarded-transitions.generated !0
constant.dead:
  %constant.deadptr = getelementptr i8, ptr %guest, i64 1536
  store i64 123456789, ptr %constant.deadptr, align 1
  br label %head
head:
'''.replace('SELECTOR','0' if fault=='wrong_target' else '1')
 s=s.replace(old,extra)
 replacements={
 '%iteration = phi i64 [ 0, %entry ], [ %next, %seed ]':'%iteration = phi i64 [ 0, %constant.branch ], [ 0, %constant.dead ], [ %next, %seed ]',
 '%history = phi i64 [ %x, %entry ], [ %fresh, %seed ]':'%history = phi i64 [ %constant.value, %constant.branch ], [ %x, %constant.dead ], [ %fresh, %seed ]',
 '%oldtarget = phi i64 [ %x, %entry ], [ %loaded, %seed ]':'%oldtarget = phi i64 [ %x, %constant.branch ], [ %x, %constant.dead ], [ %loaded, %seed ]'}
 for before,after in replacements.items():assert s.count(before)==1;s=s.replace(before,after)
 if fault=='drop_live_store':
  old='  store i64 %accnext, ptr %accptr, align 1\n';assert s.count(old)==1;s=s.replace(old,'')
 return s+'\n!0 = !{}\n'
def model(m,key,guest,fault=None):
 result=bytearray(m.model(key,guest))
 if fault=='wrong_target':struct.pack_into('<Q',result,16+m.GUEST+OFFSET,SENTINEL)
 if fault=='drop_live_store':struct.pack_into('<Q',result,16+m.GUEST+128,0)
 return bytes(result)
def emit(m,out):
 out.mkdir(exist_ok=False)
 for name in ('normal','wrong_target','drop_live_store'):(out/(name+'.ll')).write_text(ir(m,None if name=='normal' else name))
 (out/'helper.c').write_text(m.HELPER);(out/'inputs.json').write_text(json.dumps([key for key in m.INPUTS if key[0]=='plain'],indent=2)+'\n')
def same(m,actual,expected,label):m.same(actual,expected,label)
def run(m,before,after,out,opt):
 out.mkdir(exist_ok=False);arena=(ctypes.c_ubyte*m.ARENA)();address=ctypes.addressof(arena);guest=address+m.GUEST;keys=[key for key in m.INPUTS if key[0]=='plain']
 report={'opt':opt,'guest_address':guest,'normal':0,'fault_observations':0,'faults':{},'comparator_controls':0,'errors':[]}
 def compile_ir(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,key):
  ctypes.memmove(address,bytes(m.initial(key,guest)),m.ARENA);calls=ctypes.c_uint64.in_dll(lib,'business_calls');calls.value=0
  result=fn(address+m.REGS,address+m.DESC,address+m.EXIT,address+m.CONFIG)
  return struct.pack('<QQ',result,calls.value)+bytes(arena)
 try:
  for name,path,fault in [('before',before/'normal.ll',None),('after',after,None),('wrong_target',before/'wrong_target.ll','wrong_target'),('drop_live_store',before/'drop_live_store.ll','drop_live_store')]:
   lib,fn=compile_ir(path,name);required=caught=0
   with gzip.open(out/(name+'.snapshots.gz'),'wb') as stream:
    for key in keys:
     actual=invoke(lib,fn,key);stream.write(actual);expected=model(m,key,guest,fault);same(m,actual,expected,(name,key))
     if fault:
      report['fault_observations']+=1;ordinary=model(m,key,guest)
      if expected!=ordinary:
       required+=1
       try:same(m,actual,ordinary,(fault,key))
       except AssertionError:caught+=1
       else:raise AssertionError('constant-branch fault escaped')
     else:
      report['normal']+=1
      for offset in (0,8,16+m.REGS,16+m.GUEST+OFFSET)+tuple(16+m.EXIT+i*8 for i in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:same(m,damaged,expected,'comparator')
       except AssertionError:report['comparator_controls']+=1
       else:raise AssertionError('constant-branch comparator escaped')
   if fault:assert required==caught and required>0;report['faults'][fault]={'required':required,'caught':caught}
 except Exception as error:report['errors'].append(repr(error));raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
def main():
 p=argparse.ArgumentParser();p.add_argument('reference',type=Path);sp=p.add_subparsers(dest='action',required=True)
 e=sp.add_parser('emit');e.add_argument('output',type=Path)
 r=sp.add_parser('run');r.add_argument('before',type=Path);r.add_argument('after',type=Path);r.add_argument('output',type=Path);r.add_argument('--opt',type=int,choices=(0,2),required=True)
 a=p.parse_args();m=reference(a.reference)
 if a.action=='emit':emit(m,a.output)
 else:run(m,a.before,a.after,a.output,a.opt)
if __name__=='__main__':main()
