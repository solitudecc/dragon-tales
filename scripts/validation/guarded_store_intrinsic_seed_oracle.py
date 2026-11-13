#!/usr/bin/env python3
"""Independent funnel-intrinsic seed fixture. --emit/--plan never execute native.

The LLVM seed executes an actual fshl/fshr and observable counter/trace stores.
Prediction bytes are suggestions only. Python calculates the real program from
input memory and bit-string rotations; transformed LLVM is never the expected.
"""
import argparse,ctypes,gzip,hashlib,json,shutil,struct,time,traceback
from pathlib import Path
MASK=(1<<64)-1
ARENA=0x12000
REGS,EXIT,DESC,CONFIG,GUEST=0x100,0x400,0x500,0x600,0x1000
FAILURE=0xBAD0000000000000
MODES=('seed','table-after','earlier','copy-barrier')
OPS=('fshl','fshr')
FAULTS={'wrong-count':'seed','omit-effect':'seed','double-effect':'seed',
        'old-freeze':'seed','wrong-history':'seed','reload':'table-after'}
LIMITS=dict(command_seconds=180,start_free_bytes=512*1024**2,
            running_free_bytes=64*1024**2,max_loop_iterations=4,
            max_candidates=64,max_rewrites=32,max_blocks=128,max_instructions=16384)
def key(w):return 0x6a09e667 if w==32 else 0x6a09e667f3bcc909
def enc(w,n):return ((n^key(w))+5)&((1<<w)-1)
def inputs(w):
 high=1<<(w-1);mask=(1<<w)-1
 return ((0,0,0,0,65536),(1,0,3,1,65536),(high,1,3,2,65536),
  (1,w-1,3,3,65536),(mask,w,2,0,65536),(mask^1,w+1,2,1,65536),
  (0x0123456789abcdef&mask,2*w-1,3,2,65536),(0x89abcdef&mask,1,1,3,65536),
  (0,0,3,0,271),(2,0,3,1,519),(2,0,3,2,520),(2,0,3,3,520),
  (high-1,w-1,1,0,272),(0,w,3,3,65536),
  (0xa5a5a5a5a5a5a5a5&mask,w+1,3,1,65536),(mask>>1,0,3,2,65536))
def get(a,o,n=8):return int.from_bytes(a[o:o+n],'little')
def put(a,o,v,n=8):a[o:o+n]=(v&((1<<(8*n))-1)).to_bytes(n,'little')
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def rotate(w,op,value,count):
 bits=format(value&((1<<w)-1),f'0{w}b');n=count%w
 if op=='fshl':bits=bits[n:]+bits[:n]
 elif n:bits=bits[-n:]+bits[:-n]
 return int(bits,2)
def name(w,op,mode):return f'i{w}-{op}-{mode}'

def finish(label,kind,answer,target,state,tag):
 s=label+':\n'
 for off,v in ((0,answer),(24,'%rot64'),(32,'%usedold'),(40,target),(48,'%iteration'),(56,state)):
  s+=f'  %{label}.r{off}=getelementptr i8,ptr %regs,i64 {off}\n  store i64 {v},ptr %{label}.r{off},align 1\n'
 for i,v in enumerate((str(kind),'0','36864',target,'1' if kind==1 else '0','%x',tag)):
  s+=f'  %{label}.e{i}=getelementptr i8,ptr %exit,i64 {i*8}\n  store i64 {v},ptr %{label}.e{i},align 1\n'
 return s+f'  ret i64 {answer}\n'

def ir(w,op,mode,fault=None):
 W=f'i{w}'
 s=f'''source_filename = "intrinsic-seed-independent"
target datalayout = "e-p:64:64"
declare {W} @llvm.{op}.i{w}({W},{W},{W})
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {{
entry:
  %guest=load ptr,ptr %memory,align 1
  %sizep=getelementptr i8,ptr %memory,i64 8
  %size=load i64,ptr %sizep,align 1
  %xp=getelementptr i8,ptr %regs,i64 8
  %x=load i64,ptr %xp,align 1
  %np=getelementptr i8,ptr %regs,i64 16
  %n=load i64,ptr %np,align 1
  %limit=and i64 %n,3
  %initial=load i64,ptr %config,align 1
  %cp=getelementptr i8,ptr %config,i64 16
  %stateptr=getelementptr i8,ptr %guest,i64 264
  %counterptr=getelementptr i8,ptr %guest,i64 208
  %traceptr=getelementptr i8,ptr %guest,i64 216
  %extraptr=getelementptr i8,ptr %guest,i64 224
  %accptr=getelementptr i8,ptr %guest,i64 128
  %tableptr=getelementptr i8,ptr %guest,i64 512
  br label %head
head:
  %iteration=phi i64 [0,%entry],[%next,%loop]
  %oldrot=phi i64 [%x,%entry],[%rot64,%loop]
  %oldtarget=phi i64 [%x,%entry],[%target,%loop]
  %seedroom=icmp uge i64 %size,272
  br i1 %seedroom,label %rotate,label %early
rotate:
'''
 s+=('  %raw=add i64 %initial,0\n' if fault=='old-freeze' else '  %raw=load i64,ptr %config,align 1\n')
 s+='  %fresh=freeze i64 %raw\n  %count64=load i64,ptr %cp,align 1\n'
 if fault=='wrong-count':s+='  %countbad=add i64 %count64,1\n'
 count='%countbad' if fault=='wrong-count' else '%count64'
 if w==32:s+=f'  %word=trunc i64 %fresh to i32\n  %count=trunc i64 {count} to i32\n'
 else:s+=f'  %word=add i64 %fresh,0\n  %count=add i64 {count},0\n'
 if mode!='earlier':s+='  br label %seed\nseed:\n'
 s+=f'  %rot=call {W} @llvm.{op}.i{w}({W} %word,{W} %word,{W} %count)\n'
 s+=('  %rot64=zext i32 %rot to i64\n' if w==32 else '  %rot64=add i64 %rot,0\n')
 if mode=='earlier':s+='  br label %seed\nseed:\n'
 s+='  %usedold=add i64 '+('%rot64' if fault=='wrong-history' else '%oldrot')+',0\n'
 s+='  %counter=load i64,ptr %counterptr,align 1\n'
 if fault!='omit-effect':
  s+=f'  %counter_next=add i64 %counter,{2 if fault=="double-effect" else 1}\n  store i64 %counter_next,ptr %counterptr,align 1\n'
 s+='  store i64 %rot64,ptr %traceptr,align 1\n'
 s+=f'''  %bit=and {W} %rot,1
  %choose6=icmp ne {W} %bit,0
  %proposal=select i1 %choose6,{W} {enc(w,6)},{W} {enc(w,0)}
  store {W} %proposal,ptr %stateptr,align 1
  br label %between
between:
'''
 if mode=='copy-barrier':
  s+=f'  %future=call {W} @llvm.{op}.i{w}({W} %rot,{W} %rot,{W} %count)\n'
  s+=('  %future64=zext i32 %future to i64\n' if w==32 else '  %future64=add i64 %future,0\n')
  s+='  store i64 %future64,ptr %extraptr,align 1\n'
 s+=f'''  br label %state_load
state_load:
  %state=load {W},ptr %stateptr,align 1
  %unbias=sub {W} %state,5
  %decoded=xor {W} %unbias,{key(w)}
'''
 s+=('  %index64=zext i32 %decoded to i64\n  %state64=zext i32 %state to i64\n' if w==32
     else '  %index64=add i64 %decoded,0\n  %state64=add i64 %state,0\n')
 s+='''  %state_valid=icmp ult i64 %index64,4
  br i1 %state_valid,label %bounds,label %state_error
bounds:
  %scaled=shl i64 %index64,3
  %index=add i64 %scaled,512
  %ptr=getelementptr i8,ptr %guest,i64 %index
  %end=add i64 %index,8
  %table_room=icmp ule i64 %end,%size
  br i1 %table_room,label %dispatch,label %table_error
dispatch:
  %target=load i64,ptr %ptr,align 1
'''
 if mode=='table-after':s+='  store i64 99,ptr %ptr,align 1\n'
 if fault=='reload':s+='  %reloaded=load i64,ptr %ptr,align 1\n'
 selector='%reloaded' if fault=='reload' else '%target'
 s+='  switch i64 '+selector+''',label %miss [
    i64 43,label %a
    i64 44,label %b
    i64 99,label %c
  ]
a:
  br label %business
b:
  br label %business
c:
  br label %business
miss:
  br label %business
business:
  %tag=phi i64 [1,%a],[2,%b],[3,%c],[4,%miss]
  %acc=load i64,ptr %accptr,align 1
  %a1=add i64 %acc,%target
  %a2=add i64 %a1,%rot64
  %a3=add i64 %a2,%usedold
  %a4=add i64 %a3,%iteration
  %accnext=add i64 %a4,%tag
  store i64 %accnext,ptr %accptr,align 1
  %r1=xor i64 %accnext,%oldtarget
  %answer=xor i64 %r1,%usedold
  %again=icmp ult i64 %iteration,%limit
  br i1 %again,label %loop,label %done
loop:
  %next=add i64 %iteration,1
  %step=add i64 %next,11400714819323198485
  %next_raw=add i64 %raw,%step
  store i64 %next_raw,ptr %config,align 1
  %next_count=add i64 %count64,1
  store i64 %next_count,ptr %cp,align 1
  br label %head
'''
 s+=finish('done',1,'%answer','%target','%state64','%tag')
 # The early block cannot read %rot64/%usedold. It preserves all input fields
 # except explicitly defined return+Exit, making before-seed effects observable.
 s+='early:\n'
 early=FAILURE^208
 s+=f'  store i64 {early},ptr %regs,align 1\n'
 for i,v in enumerate(('5','0','36864','208','0','%x','11')):
  s+=f'  %early.e{i}=getelementptr i8,ptr %exit,i64 {8*i}\n  store i64 {v},ptr %early.e{i},align 1\n'
 s+=f'  ret i64 {early}\n'
 s+='state_error:\n  %badstate=xor i64 264,'+str(FAILURE)+'\n  br label %state_done\n'
 s+=finish('state_done',5,'%badstate','264','%state64','12')
 s+='table_error:\n  %badtable=xor i64 %index,'+str(FAILURE)+'\n  br label %table_done\n'
 s+=finish('table_done',5,'%badtable','%index','%state64','13')
 return s+'}\n'

def initial(w,k,guest):
 x,count,n,table,size=k
 a=bytearray((i*37+11)&255 for i in range(ARENA))
 for off,value in ((8,x),(16,n),(80,count)):put(a,REGS+off,value)
 put(a,DESC,guest);put(a,DESC+8,size)
 put(a,CONFIG,x);put(a,CONFIG+16,count)
 put(a,GUEST+128,0);put(a,GUEST+208,0)
 put(a,GUEST+264,enc(w,6),w//8)
 put(a,GUEST+512,(43,44,99,777)[table])
 return a

def model(w,op,mode,k,guest,fault=None):
 x,count,n,table,size=k;a=initial(w,k,guest);oldrot=oldtarget=x
 def result(kind,answer,target,state,tag,early=False):
  if early:put(a,REGS,answer)
  else:
   for off,value in ((0,answer),(24,rot),(32,usedold),(40,target),(48,iteration),(56,state)):put(a,REGS+off,value)
  for i,value in enumerate((kind,0,36864,target,0 if kind==5 else 1,x,tag)):put(a,EXIT+8*i,value)
  return struct.pack('<Q',answer&MASK)+a
 for iteration in range((n&3)+1):
  if size<272:return result(5,FAILURE^208,208,0,11,True)
  raw=x if fault=='old-freeze' else get(a,CONFIG)
  rawcount=get(a,CONFIG+16)
  count_used=(rawcount+(fault=='wrong-count'))&MASK
  rot=rotate(w,op,raw,count_used)
  usedold=rot if fault=='wrong-history' else oldrot
  if fault!='omit-effect':put(a,GUEST+208,get(a,GUEST+208)+(2 if fault=='double-effect' else 1))
  put(a,GUEST+216,rot)
  choice=6 if rot%2 else 0
  put(a,GUEST+264,enc(w,choice),w//8)
  if mode=='copy-barrier':put(a,GUEST+224,rotate(w,op,rot,count_used))
  state=get(a,GUEST+264,w//8)
  decoded=((state-5)&((1<<w)-1))^key(w)
  if decoded>=4:return result(5,FAILURE^264,264,state,12)
  index=512+8*decoded
  if index+8>size:return result(5,FAILURE^index,index,state,13)
  target=get(a,GUEST+index)
  if mode=='table-after':put(a,GUEST+index,99)
  selected=get(a,GUEST+index) if fault=='reload' else target
  tag={43:1,44:2,99:3}.get(selected,4)
  acc=(get(a,GUEST+128)+target+rot+usedold+iteration+tag)&MASK
  put(a,GUEST+128,acc);answer=acc^oldtarget^usedold
  if iteration==(n&3):return result(1,answer,target,state,tag)
  put(a,CONFIG,raw+11400714819323198485+iteration+1)
  put(a,CONFIG+16,rawcount+1);oldrot=rot;oldtarget=target
 raise AssertionError('bounded model exceeded four iterations')

def emit(out):
 assert not out.exists();out.mkdir(parents=True);files=[];cases=[]
 def save(n,s):
  (out/n).write_text(s);files.append(n)
 for w in (32,64):
  save(f'INPUTS-i{w}.json',json.dumps(inputs(w),indent=2)+'\n')
  for op in OPS:
   for mode in MODES:
    stem=name(w,op,mode)
    save(stem+'.before.ll',ir(w,op,mode))
    # No entry6 bytes: the actual bound-error edge must remain, while partial
    # prediction may retain the admissible index0 business target.
    for suffix,value in (('manifest',43),('wrong-manifest',44)):
     save(stem+'.'+suffix+'.json',json.dumps(dict(view_symbol='oracle',memory=[dict(offset='0x200',bytes=value.to_bytes(8,'little').hex())]),indent=2)+'\n')
    cases.append(dict(name=stem,width=w,operation=op,placement=mode,inputs=f'INPUTS-i{w}.json',
                      expected_intrinsic_calls=(2 if mode=='copy-barrier' else 1),
                      expected_boundary='copied-path intrinsic remains a barrier' if mode=='copy-barrier' else 'already-executed intrinsic must not be copied'))
   for fault,mode in FAULTS.items():save(name(w,op,mode)+'.fault-'+fault+'.ll',ir(w,op,mode,fault))
   source=ir(w,op,'seed')
   save(name(w,op,'poison')+'.structural-only.ll',source.replace(f'i{w} %word,',f'i{w} poison,',1))
   old=f'%rot=call i{w} @llvm.{op}.i{w}(i{w} %word,i{w} %word,i{w} %count)'
   wrapper=f'%rot=call i{w} @closed_math_wrapper(i{w} %word,i{w} %count)'
   helper=f'\ndefine internal i{w} @closed_math_wrapper(i{w} %v,i{w} %n) {{\n  %r=call i{w} @llvm.{op}.i{w}(i{w} %v,i{w} %v,i{w} %n)\n  ret i{w} %r\n}}\n'
   assert old in source
   save(name(w,op,'ordinary-call')+'.structural-only.ll',source.replace(old,wrapper)+helper)
 manifest=dict(schema='guarded-intrinsic-seed-independent-v1',source_sha256=digest(__file__),
     cases=cases,faults=FAULTS,limits=LIMITS,partial_prediction_index=0,bound_side_exit_index=6,
     domain_scope='16 input keys per width; count>=width is defined funnel-intrinsic modular rotation; no new VM/heap/system calls',
     precision_scope='seed effects exactly once; no intrinsic copied; direct business fast case under actual target equality; retained state6/bounds/miss fallback',
     producer_runs=0,native_runs=0,
     files={n:dict(bytes=(out/n).stat().st_size,sha256=digest(out/n)) for n in files})
 (out/'EMITTED.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
 print('emit only',out,'files',len(files),'manifest',digest(out/'EMITTED.json'))

def load(path):
 lib=ctypes.CDLL(str(path.resolve()));fn=lib.oracle
 fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64
 return lib,fn
def execute(fn,buffer,initial_bytes):
 address=ctypes.addressof(buffer);ctypes.memmove(address,bytes(initial_bytes),ARENA)
 value=fn(address+REGS,address+DESC,address+EXIT,address+CONFIG)
 return struct.pack('<Q',value)+bytes(buffer)

def run(a):
 assert not a.out.exists();a.out.mkdir(parents=True)
 started=time.monotonic();active=dict(phase='startup')
 report=dict(schema='guarded-intrinsic-seed-native-v1',passed=False,completed=False,
             limits=LIMITS,source_sha256=digest(__file__),matches=0,comparator_controls=0,
             faults={},failures=[],binary_pins={})
 def guard(start=False):
  paths={'output':a.out}
  if Path('/dev/shm').exists():paths['tmpfs']=Path('/dev/shm')
  free={k:shutil.disk_usage(v).free for k,v in paths.items()}
  if start:report['initial_free_bytes']=free
  prior=report.setdefault('minimum_free_bytes',dict(free))
  for k,v in free.items():prior[k]=min(prior[k],v)
  assert min(free.values())>=(LIMITS['start_free_bytes'] if start else LIMITS['running_free_bytes'])
  assert time.monotonic()-started<180
 def library(p):
  h=digest(p);path=str(p.resolve());old=report['binary_pins'].setdefault(path,h);assert old==h
  return load(p)
 buffer=(ctypes.c_ubyte*ARENA)();guest=ctypes.addressof(buffer)+GUEST
 try:
  guard(True)
  with gzip.open(a.out/'full-frames.bin.gz','wb') as normal,gzip.open(a.out/'fault-frames.bin.gz','wb') as faults:
   for w in (32,64):
    for op_index,op in enumerate(OPS):
     for mode_index,mode in enumerate(MODES):
      stem=name(w,op,mode);active=dict(phase='load',fixture=stem)
      bl,bf=library(a.before_dir/(stem+'.so'));al,af=library(a.after_dir/(stem+'.so'))
      for i,k in enumerate(inputs(w)):
       active=dict(phase='normal',width=w,op=op,mode=mode,input_number=i,input=list(k));guard()
       init=initial(w,k,guest);expected=model(w,op,mode,k,guest)
       before=execute(bf,buffer,init);after=execute(af,buffer,init)
       normal.write(struct.pack('<4I',w,op_index,mode_index,i));normal.write(init);normal.write(expected);normal.write(before);normal.write(after)
       active.update(expected_sha256=hashlib.sha256(expected).hexdigest(),before_sha256=hashlib.sha256(before).hexdigest(),after_sha256=hashlib.sha256(after).hexdigest())
       assert before==expected and after==expected
       report['matches']+=2
       for offset in (8+REGS,8+GUEST+208,8+EXIT):
        damaged=bytearray(expected);damaged[offset]^=1;assert damaged!=expected;report['comparator_controls']+=1
     if a.fault_dir:
      for fault_index,(fault,mode) in enumerate(FAULTS.items()):
       stem=name(w,op,mode);active=dict(phase='load-fault',fixture=stem,fault=fault)
       lib,fn=library(a.fault_dir/(stem+'.fault-'+fault+'.so'))
       counts=dict(observations=0,specified=0,detected=0);report['faults'][name(w,op,fault)]=counts
       for i,k in enumerate(inputs(w)):
        active=dict(phase='fault',width=w,op=op,fault=fault,input_number=i,input=list(k));guard()
        init=initial(w,k,guest);expected=model(w,op,mode,k,guest);faulty=model(w,op,mode,k,guest,fault)
        actual=execute(fn,buffer,init)
        faults.write(struct.pack('<4I',w,op_index,fault_index,i));faults.write(init);faults.write(expected);faults.write(faulty);faults.write(actual)
        active.update(expected_sha256=hashlib.sha256(expected).hexdigest(),fault_expected_sha256=hashlib.sha256(faulty).hexdigest(),actual_sha256=hashlib.sha256(actual).hexdigest())
        assert actual==faulty;counts['observations']+=1
        if faulty!=expected:counts['specified']+=1;assert actual!=expected;counts['detected']+=1
       assert counts['specified']>0
   for path,h in report['binary_pins'].items():assert digest(Path(path))==h
   report['passed']=report['completed']=True
 except BaseException as e:
  report['failures'].append(dict(case=active,type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()));raise
 finally:
  report['seconds']=time.monotonic()-started;report['last_case']=active
  report['streams']={n:dict(bytes=(a.out/n).stat().st_size,sha256=digest(a.out/n)) for n in ('full-frames.bin.gz','fault-frames.bin.gz') if (a.out/n).exists()}
  (a.out/'RESULTS.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 q=p.add_mutually_exclusive_group(required=True);q.add_argument('--emit',type=Path);q.add_argument('--run',action='store_true')
 p.add_argument('--out',type=Path);p.add_argument('--before-dir',type=Path);p.add_argument('--after-dir',type=Path);p.add_argument('--fault-dir',type=Path)
 a=p.parse_args()
 if a.emit:return emit(a.emit)
 assert a.out and a.before_dir and a.after_dir;return run(a)
if __name__=='__main__':main()
