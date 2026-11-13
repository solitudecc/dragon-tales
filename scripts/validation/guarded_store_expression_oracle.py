#!/usr/bin/env python3
"""Independent encoded-state fixtures/model. --emit writes only; never compiles.

The expected return and complete arena are calculated from the actual input
memory by Python, independently of the transformed LLVM and prediction manifest.
Native execution is an explicit separate --run operation on already-built SOs.
"""
import argparse, ctypes, gzip, hashlib, json, struct, shutil, time, traceback
from pathlib import Path

MASK=(1<<64)-1
ARENA=0x12000
REGS,EXIT,DESC,CONFIG,GUEST=0x100,0x400,0x500,0x600,0x1000
FAILURE=0xBAD0000000000000
MODES=('plain','wrong-address','wrong-width','overlap','state-after',
       'table-before','table-after','call-prefix','call-inside','side-error')
FAULTS={'trust-hint':'plain','reload':'table-after','drop-store':'plain',
        'wrong-state':'state-after','pin-fresh':'plain',
        'history-as-fresh':'plain','drop-side':'call-inside','wrong-exit':'plain'}
TARGETS=((43,44,99,127),(99,43,127,44),(777,99999,44,43))
VALUES=(0,1,1<<63,MASK)
INPUTS=tuple([(x,n,t,65536,2+(x&1)) for x in VALUES for n in range(4) for t in range(3)]
 +[(x,3,0,size,2+(x&1)) for size in (263,295,519,527) for x in (0,1)]
 +[(x,3,2,65536,o) for x in (0,1) for o in (0,1,0xffffffff,MASK)])
assert len(INPUTS)==64
LIMITS=dict(command_seconds=180,start_free_bytes=512*1024**2,
            running_free_bytes=64*1024**2,max_loop_iterations=4,
            max_candidates=64,max_rewrites=32,max_blocks=128,max_instructions=16384)

def key(width):return 0x6a09e667 if width==32 else 0x6a09e667f3bcc909
def encoded(width,n):return ((n^key(width))+5)&((1<<width)-1)
def get(a,o,n=8):return int.from_bytes(a[o:o+n],'little')
def put(a,o,v,n=8):a[o:o+n]=(v&((1<<(8*n))-1)).to_bytes(n,'little')
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

HELPER=r'''#include <stdint.h>
#include <string.h>
static uint64_t rd(const unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void wr(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
void oracle_side(unsigned char *regs,unsigned char *guest,unsigned char *config,
                 uint64_t x,uint64_t width) {
 uint64_t k=width==4?UINT64_C(0x6a09e667):UINT64_C(0x6a09e667f3bcc909);
 uint64_t state=(((x&1)+2)^k)+5;
 if(width==4)state=(uint32_t)state;
 memcpy(guest+264,&state,width);
 wr(guest+512,(x&1)?99:777);
 wr(guest+128,rd(guest+128)^(x|1));
 wr(config,rd(config)^(x+1));
 wr(config+8,rd(config+8)+1);
 wr(regs+64,rd(regs+64)+1);
}
'''

def finish(label,kind,answer,observed,seen,tag,fault):
 s=label+':\n'
 for offset,v in ((0,answer),(24,'%history_used'),(32,observed),(40,seen),(48,'%fresh'),(56,'%iteration')):
  s+=f'  %{label}.r{offset}=getelementptr i8, ptr %regs, i64 {offset}\n  store i64 {v}, ptr %{label}.r{offset}, align 1\n'
 pc=36865 if fault=='wrong-exit' else 36864
 vals=(str(kind),'0',str(pc),observed,'1' if kind==1 else '0','%x',tag)
 for i,v in enumerate(vals):
  s+=f'  %{label}.e{i}=getelementptr i8, ptr %exit, i64 {i*8}\n  store i64 {v}, ptr %{label}.e{i}, align 1\n'
 return s+f'  ret i64 {answer}\n'

def ir(width,mode,fault=None):
 assert width in (32,64) and mode in MODES
 W=f'i{width}';k=key(width);bytewidth=width//8
 dest=280 if mode=='wrong-address' else 266 if mode=='overlap' else 264
 storeW=f'i{width//2}' if mode=='wrong-width' else W
 s=f'''source_filename = "guarded-store-expression-independent"
target datalayout = "e-p:64:64"
declare void @oracle_side(ptr,ptr,ptr,i64,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {{
entry:
  %guest=load ptr, ptr %memory, align 1
  %sizep=getelementptr i8, ptr %memory, i64 8
  %size=load i64, ptr %sizep, align 1
  %xp=getelementptr i8, ptr %regs, i64 8
  %x=load i64, ptr %xp, align 1
  %np=getelementptr i8, ptr %regs, i64 16
  %n=load i64, ptr %np, align 1
  %limit=and i64 %n,3
  %overridep=getelementptr i8, ptr %regs, i64 72
  %override64=load i64, ptr %overridep, align 1
  %initialfresh=load i64, ptr %config, align 1
  %stateptr=getelementptr i8, ptr %guest, i64 264
  %storeptr=getelementptr i8, ptr %guest, i64 {dest}
  %accptr=getelementptr i8, ptr %guest, i64 128
  %oddx=and i64 %x,1
  %isoddx=icmp ne i64 %oddx,0
  %forced=select i1 %isoddx,i64 99,i64 777
  br label %head
head:
  %iteration=phi i64 [0,%entry],[%next,%loopseed]
  %history=phi i64 [%x,%entry],[%fresh,%loopseed]
  %oldtarget=phi i64 [%x,%entry],[%target,%loopseed]
'''
 if mode=='call-prefix' and fault!='drop-side':
  s+=f'  call void @oracle_side(ptr %regs,ptr %guest,ptr %config,i64 %x,i64 {bytewidth})\n'
 s+=('  %rawfresh=add i64 %initialfresh,0\n' if fault=='pin-fresh'
     else '  %rawfresh=load i64,ptr %config,align 1\n')
 s+='  %fresh=freeze i64 %rawfresh\n'
 s+='  %history_used=add i64 '+('%fresh' if fault=='history-as-fresh' else '%history')+',0\n'
 s+=f'''  %bit=and i64 %fresh,1
  %odd=icmp ne i64 %bit,0
  %proposal=select i1 %odd,{W} {encoded(width,1)},{W} {encoded(width,0)}
  %seedroom=icmp uge i64 %size,296
  br i1 %seedroom,label %seed,label %early_fail
seed:
'''
 if fault!='drop-store':
  if mode=='wrong-width':
   s+=f'  %narrowstore=trunc {W} %proposal to {storeW}\n  store {storeW} %narrowstore,ptr %storeptr,align 1\n'
  else:s+=f'  store {W} %proposal,ptr %storeptr,align 1\n'
 s+='  br label %mutate\nmutate:\n'
 if mode=='state-after':
  if width==32:s+='  %override=trunc i64 %override64 to i32\n'
  s+=f'  store {W} '+('%override' if width==32 else '%override64')+',ptr %stateptr,align 1\n'
 if mode=='call-inside' and fault!='drop-side':
  s+=f'  call void @oracle_side(ptr %regs,ptr %guest,ptr %config,i64 %x,i64 {bytewidth})\n'
 if mode=='side-error':
  s+='  %errorbit=and i64 %x,4\n  %error=icmp ne i64 %errorbit,0\n  br i1 %error,label %side_fail,label %state_load\n'
 else:s+='  br label %state_load\n'
 s+='state_load:\n'
 s+=(f'  %state=add {W} {encoded(width,0)},0\n' if fault=='wrong-state'
      else f'  %state=load {W},ptr %stateptr,align 1\n')
 s+=f'  %unbias=sub {W} %state,5\n  %decoded=xor {W} %unbias,{k}\n'
 s+=('  %idx=zext i32 %decoded to i64\n  %seen=zext i32 %state to i64\n' if width==32
      else '  %idx=add i64 %decoded,0\n  %seen=add i64 %state,0\n')
 s+='''  %valid=icmp ult i64 %idx,4
  br i1 %valid,label %index,label %state_fail
index:
  %scaled=shl i64 %idx,3
  %table_index=add i64 %scaled,512
  %tableptr=getelementptr i8,ptr %guest,i64 %table_index
  %end=add i64 %table_index,8
  %room=icmp ule i64 %end,%size
  br i1 %room,label %dispatch,label %table_fail
dispatch:
'''
 if mode=='table-before':s+='  store i64 %forced,ptr %tableptr,align 1\n'
 s+='  %target=load i64,ptr %tableptr,align 1\n'
 if mode=='table-after':s+='  store i64 %forced,ptr %tableptr,align 1\n'
 selector='%target'
 if fault=='trust-hint':selector='43'
 if fault=='reload':
  s+='  %reloaded=load i64,ptr %tableptr,align 1\n';selector='%reloaded'
 s+='  switch i64 '+selector+''',label %miss [
    i64 43,label %a
    i64 44,label %a
    i64 99,label %b
    i64 127,label %c
  ]
a:
  %routed=phi i64 [%target,%dispatch],[%target,%dispatch]
  br label %business
b:
  br label %business
c:
  br label %business
miss:
  br label %business
business:
  %tag=phi i64 [1,%a],[2,%b],[3,%c],[4,%miss]
  %selected=phi i64 [%routed,%a],[%target,%b],[%target,%c],[%target,%miss]
  %acc=load i64,ptr %accptr,align 1
  %a1=add i64 %acc,%tag
  %a2=add i64 %a1,%selected
  %a3=add i64 %a2,%fresh
  %accnext=add i64 %a3,%history_used
  store i64 %accnext,ptr %accptr,align 1
  %v1=xor i64 %accnext,%fresh
  %v2=xor i64 %v1,%history_used
  %answer=xor i64 %v2,%oldtarget
  %again=icmp ult i64 %iteration,%limit
  br i1 %again,label %loopseed,label %finish
loopseed:
  %next=add i64 %iteration,1
  %noise=mul i64 %next,17
  %nextfresh=xor i64 %x,%noise
  store i64 %nextfresh,ptr %config,align 1
  br label %head
'''
 s+=finish('finish',1,'%answer','%target','%seen','%tag',fault)
 s+='early_fail:\n  %badearly=xor i64 256,'+str(FAILURE)+'\n  br label %early_done\n'
 s+=finish('early_done',5,'%badearly','256','0','7',fault)
 s+='side_fail:\n  %badside=xor i64 280,'+str(FAILURE)+'\n  br label %side_done\n'
 s+=finish('side_done',5,'%badside','280','0','8',fault)
 s+='state_fail:\n  %badstate=xor i64 264,'+str(FAILURE)+'\n  br label %state_done\n'
 s+=finish('state_done',5,'%badstate','264','%seen','9',fault)
 s+='table_fail:\n  %badtable=xor i64 %table_index,'+str(FAILURE)+'\n  br label %table_done\n'
 s+=finish('table_done',5,'%badtable','%table_index','%seen','10',fault)
 return s+'}\n'

def initial(width,input_key,guest):
 x,n,variant,size,override=input_key
 a=bytearray((i*37+11)&255 for i in range(ARENA))
 for off,v in ((8,x),(16,n),(64,0),(72,encoded(width,override))):put(a,REGS+off,v)
 put(a,DESC,guest);put(a,DESC+8,size)
 put(a,GUEST+264,encoded(width,2+(x&1)),width//8)
 put(a,GUEST+128,0);put(a,CONFIG,x^0x1122334455667788);put(a,CONFIG+8,0)
 for i,v in enumerate(TARGETS[variant]):put(a,GUEST+512+8*i,v)
 return a

def model(width,mode,input_key,guest,fault=None):
 x,n,variant,size,override=input_key
 a=initial(width,input_key,guest);initialfresh=get(a,CONFIG)
 history=oldtarget=x
 def side():
  put(a,GUEST+264,encoded(width,(x&1)+2),width//8)
  put(a,GUEST+512,99 if x&1 else 777)
  put(a,GUEST+128,get(a,GUEST+128)^(x|1))
  put(a,CONFIG,get(a,CONFIG)^((x+1)&MASK))
  put(a,CONFIG+8,get(a,CONFIG+8)+1)
  put(a,REGS+64,get(a,REGS+64)+1)
 def done(kind,answer,observed,seen,tag):
  for off,v in ((0,answer),(24,history_used),(32,observed),(40,seen),(48,fresh),(56,iteration)):put(a,REGS+off,v)
  for i,v in enumerate((kind,0,36865 if fault=='wrong-exit' else 36864,observed,1 if kind==1 else 0,x,tag)):put(a,EXIT+8*i,v)
  return struct.pack('<Q',answer&MASK)+a
 for iteration in range((n&3)+1):
  if mode=='call-prefix' and fault!='drop-side':side()
  fresh=initialfresh if fault=='pin-fresh' else get(a,CONFIG)
  history_used=fresh if fault=='history-as-fresh' else history
  if size<296:return done(5,FAILURE^256,256,0,7)
  proposal=encoded(width,fresh%2)
  dest=280 if mode=='wrong-address' else 266 if mode=='overlap' else 264
  if fault!='drop-store':put(a,GUEST+dest,proposal,width//16 if mode=='wrong-width' else width//8)
  if mode=='state-after':put(a,GUEST+264,encoded(width,override),width//8)
  if mode=='call-inside' and fault!='drop-side':side()
  if mode=='side-error' and x&4:return done(5,FAILURE^280,280,0,8)
  state=encoded(width,0) if fault=='wrong-state' else get(a,GUEST+264,width//8)
  decoded=((state-5)&((1<<width)-1))^key(width)
  if decoded>=4:return done(5,FAILURE^264,264,state,9)
  address=512+decoded*8
  if address+8>size:return done(5,FAILURE^address,address,state,10)
  forced=99 if x&1 else 777
  if mode=='table-before':put(a,GUEST+address,forced)
  target=get(a,GUEST+address)
  if mode=='table-after':put(a,GUEST+address,forced)
  selector=43 if fault=='trust-hint' else get(a,GUEST+address) if fault=='reload' else target
  tag=1 if selector in (43,44) else 2 if selector==99 else 3 if selector==127 else 4
  acc=(get(a,GUEST+128)+tag+target+fresh+history_used)&MASK
  put(a,GUEST+128,acc)
  answer=acc^fresh^history_used^oldtarget
  if iteration==(n&3):return done(1,answer,target,state,tag)
  put(a,CONFIG,x^((iteration+1)*17));history=fresh;oldtarget=target
 raise AssertionError('bounded model did not terminate')

def emit(out):
 assert not out.exists();out.mkdir(parents=True)
 files=[]
 def save(name,data):
  p=out/name;p.write_text(data);files.append(name)
 save('helper.c',HELPER)
 save('INPUTS.json',json.dumps(INPUTS,indent=2)+'\n')
 for width in (32,64):
  for mode in MODES:
   name=f'i{width}-{mode}'
   save(name+'.before.ll',ir(width,mode))
   memory=[dict(offset=hex(512+8*i),bytes=v.to_bytes(8,'little').hex()) for i,v in enumerate(TARGETS[0])]
   save(name+'.manifest.json',json.dumps(dict(view_symbol='oracle',memory=memory),indent=2)+'\n')
   bad=[dict(offset=hex(512+8*i),bytes=v.to_bytes(8,'little').hex()) for i,v in enumerate((99,127,43,44))]
   save(name+'.wrong-manifest.json',json.dumps(dict(view_symbol='oracle',memory=bad),indent=2)+'\n')
  for fault,mode in FAULTS.items():save(f'i{width}-{mode}.fault-{fault}.ll',ir(width,mode,fault))
  poison=ir(width,'plain').replace(f'store i{width} %proposal,ptr %storeptr',f'store i{width} poison,ptr %storeptr')
  volatile=ir(width,'plain').replace(f'%state=load i{width},ptr %stateptr',f'%state=load volatile i{width},ptr %stateptr')
  save(f'i{width}-poison.structural-only.ll',poison)
  save(f'i{width}-volatile.structural-only.ll',volatile)
 manifest=dict(schema='guarded-store-expression-independent-v1',source_sha256=digest(__file__),
               files={n:dict(sha256=digest(out/n),bytes=(out/n).stat().st_size) for n in files},
               modes=MODES,faults=FAULTS,widths=[32,64],inputs=len(INPUTS),limits=LIMITS,
               prediction_scope='untrusted table target suggestions only; no known Guest memory',
               equivalence_scope='u64 return plus every arena byte; no exclusions',
               precision_scope='rewrites/copies alone insufficient; actual target guard and same-read fallback must remain',
               structural_only=['poison','volatile'],producer_runs=0,native_runs=0)
 (out/'EMITTED.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
 print('emit only',out,'files',len(files),'manifest',digest(out/'EMITTED.json'))

def load_so(path):
 lib=ctypes.CDLL(str(path.resolve()))
 fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64
 return lib,fn
def execute(fn,buffer,initial_bytes):
 ctypes.memmove(ctypes.addressof(buffer),bytes(initial_bytes),ARENA)
 base=ctypes.addressof(buffer)
 returned=fn(base+REGS,base+DESC,base+EXIT,base+CONFIG)
 return struct.pack('<Q',returned)+bytes(buffer)
def run(args):
 assert not args.out.exists();args.out.mkdir(parents=True)
 report=dict(schema='guarded-store-expression-native-v2',limits=LIMITS,
             source_sha256=digest(__file__),matches=0,comparator_controls=0,faults={},
             failures=[],passed=False,completed=False,binary_pins={},
             scope='full arena+return; no precision/full-CFF conclusion')
 active=dict(phase='startup')
 started=time.monotonic()
 def guard(start=False):
  roots={'output':args.out}
  if Path('/dev/shm').exists():roots['tmpfs']=Path('/dev/shm')
  free={k:shutil.disk_usage(v).free for k,v in roots.items()}
  if start:report['initial_free_bytes']=free
  previous=report.setdefault('minimum_free_bytes',dict(free))
  for k,v in free.items():previous[k]=min(previous[k],v)
  assert min(free.values())>=(LIMITS['start_free_bytes'] if start else LIMITS['running_free_bytes']),('space',free)
  assert time.monotonic()-started<LIMITS['command_seconds'],'native batch exceeds180s'
 def binary(path):
  h=digest(path);key=str(path.resolve())
  old=report['binary_pins'].setdefault(key,h);assert old==h,('binary changed',key)
  return load_so(path)
 buffer=(ctypes.c_ubyte*ARENA)();guest=ctypes.addressof(buffer)+GUEST
 try:
  guard(start=True)
  with gzip.open(args.out/'full-frames.bin.gz','wb') as stream,gzip.open(args.out/'fault-frames.bin.gz','wb') as fault_stream:
   for width in (32,64):
    for mode in MODES:
     name=f'i{width}-{mode}';before=args.before_dir/(name+'.so');after=args.after_dir/(name+'.so')
     active=dict(phase='load',width=width,mode=mode,before=str(before),after=str(after))
     b_lib,b_fn=binary(before);a_lib,a_fn=binary(after)
     for number,input_key in enumerate(INPUTS):
      active=dict(phase='normal',width=width,mode=mode,input_number=number,input=list(input_key),before=str(before),after=str(after))
      guard()
      init=initial(width,input_key,guest);expected=model(width,mode,input_key,guest)
      b=execute(b_fn,buffer,init);a=execute(a_fn,buffer,init)
      stream.write(struct.pack('<III',width,MODES.index(mode),number));stream.write(init);stream.write(expected);stream.write(b);stream.write(a)
      active.update(expected_sha256=hashlib.sha256(expected).hexdigest(),before_sha256=hashlib.sha256(b).hexdigest(),after_sha256=hashlib.sha256(a).hexdigest())
      assert b==expected,('before/reference',name,number)
      assert a==expected,('after/reference',name,number)
      report['matches']+=2
      for offset in (8+REGS,8+GUEST+128,8+EXIT):
       bad=bytearray(expected);bad[offset]^=1;assert bad!=expected;report['comparator_controls']+=1
    if args.fault_dir:
     for fault,mode in FAULTS.items():
      path=args.fault_dir/f'i{width}-{mode}.fault-{fault}.so'
      active=dict(phase='load-fault',width=width,mode=mode,fault=fault,binary=str(path))
      lib,fn=binary(path);counts=dict(observations=0,specified=0,detected=0)
      report['faults'][f'i{width}-{fault}']=counts
      for number,input_key in enumerate(INPUTS):
       active=dict(phase='fault',width=width,mode=mode,fault=fault,input_number=number,input=list(input_key),binary=str(path))
       guard()
       init=initial(width,input_key,guest);want=model(width,mode,input_key,guest)
       faulty=model(width,mode,input_key,guest,fault)
       actual=execute(fn,buffer,init)
       fault_stream.write(struct.pack('<III',width,list(FAULTS).index(fault),number))
       fault_stream.write(init);fault_stream.write(want);fault_stream.write(faulty);fault_stream.write(actual)
       active.update(expected_sha256=hashlib.sha256(want).hexdigest(),fault_expected_sha256=hashlib.sha256(faulty).hexdigest(),actual_sha256=hashlib.sha256(actual).hexdigest())
       assert actual==faulty,('fault/reference',width,fault,number)
       counts['observations']+=1
       if faulty!=want:
        counts['specified']+=1;assert actual!=want;counts['detected']+=1
      assert counts['specified']>0
   for path,h in report['binary_pins'].items():assert digest(path)==h,('binary changed after execution',path)
   report['passed']=True;report['completed']=True
 except BaseException as error:
  report['failures'].append(dict(type=type(error).__name__,detail=str(error),case=active,traceback=traceback.format_exc()))
  raise
 finally:
  report['elapsed_seconds']=time.monotonic()-started
  report['last_case']=active
  report['streams']={name:dict(bytes=(args.out/name).stat().st_size,sha256=digest(args.out/name))
                     for name in ('full-frames.bin.gz','fault-frames.bin.gz') if (args.out/name).exists()}
  (args.out/'RESULTS.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')

def main():
 p=argparse.ArgumentParser(description=__doc__)
 mode=p.add_mutually_exclusive_group(required=True)
 mode.add_argument('--emit',type=Path);mode.add_argument('--run',action='store_true')
 p.add_argument('--out',type=Path);p.add_argument('--before-dir',type=Path)
 p.add_argument('--after-dir',type=Path);p.add_argument('--fault-dir',type=Path)
 a=p.parse_args()
 if a.emit:return emit(a.emit)
 assert a.out and a.before_dir and a.after_dir
 return run(a)
if __name__=='__main__':main()
