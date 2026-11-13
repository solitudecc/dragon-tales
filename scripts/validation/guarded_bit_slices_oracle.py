#!/usr/bin/env python3
"""Frozen bit-slice fixture domain; Python and C sequential references are independent.

The original IR always executes its source loads, shifts, stores and complete
switch. Invalid shifts and explicit undef/poison are structural refusal probes,
never native inputs. No output byte is normalized or omitted.
"""
import argparse, ctypes, gzip, hashlib, itertools, json, struct, subprocess
from pathlib import Path

MASK=(1<<64)-1
ARENA,REGS,GUEST,EXIT,DESC,CONFIG=0x12000,0x100,0x1000,0x400,0x500,0x600
ARENA_PATTERN=bytes((i*67+23)&255 for i in range(ARENA))
BASE_MODES=tuple(f'{op}{width}t{bits}' for op in ('l','a') for width in (32,64,128) for bits in (1,2,3))
MODES=BASE_MODES+('dynamic_legal','dynamic_zero','history','table_before','table_after','sidecall','call_inside')
# Each tuple is independent from the older checked-corridor campaign.
# mode,n,lo,hi,size,alias,target,trigger,replacement,count-selector
SOURCES=((0,0),(1,0),(MASK,MASK),(1<<31,0),((1<<31)-1,0),(1<<63,0),((1<<63)-1,0),(0,1<<63),(MASK,(1<<63)-1),(MASK,1<<63))
INPUTS=[]
for mode in MODES:
 for i,(n,pair,target) in enumerate(itertools.product((0,1,3,7),SOURCES,(43,44,45,99999))):
  INPUTS.append((mode,n,*pair,1024,(i//4)%2,target,(0,1,3)[(i//8)%3],(43,44,777,99999)[(i//16)%4],i%2))
 for alias,size,target in itertools.product((0,1),(0,847,848,855,856,903,904,65536),(43,44,99999)):
  INPUTS.append((mode,2,MASK,MASK,size,alias,target,1,777,1))
INPUTS=tuple(INPUTS)
assert len(INPUTS)==len(set(INPUTS))==5200
# Keep the entire first freeze as a prefix, then append an explicit complete
# bit-pattern grid. Edge values alone cover only 0/3/4/7 of eight top3 values.
PATTERN_GRID=[]
for op,width,bits in itertools.product(('l','a'),(32,64,128),(1,2,3)):
 for pattern,alias,target in itertools.product(range(1<<bits),(0,1),(43,44,45)):
  desired=pattern<<(width-bits)
  lo=(desired&MASK)^0x8000000080000000
  hi=(desired>>64)^0x8000000000000000
  PATTERN_GRID.append((f'{op}{width}t{bits}',7,lo,hi,1024,alias,target,6,777,1))
INPUTS+=tuple(PATTERN_GRID)
assert len(PATTERN_GRID)==504 and len(INPUTS)==len(set(INPUTS))==5704
FAULTS={'omit_prefix':'l32t1','double_prefix':'l32t1','frozen_source':'l64t1',
 'logical_for_arithmetic':'a64t3','zeroextend_signed32':'a32t2','ignore_high128':'l128t3',
 'wrong_count':'l64t2','frozen_count':'dynamic_legal','stale_history':'history',
 'reload_selector':'table_after','drop_table_store':'table_after',
 'drop_case45':'l64t1','omit_sidecall':'sidecall','wrong_root_history':'history'}

def profile(mode):
 if mode in BASE_MODES:
  return int(mode[1:mode.index('t')]),int(mode[mode.index('t')+1:]),mode[0]=='a'
 return 64,1,False

def put(a,o,v):struct.pack_into('<Q',a,o,v&MASK)
def get(a,o):return struct.unpack_from('<Q',a,o)[0]
def regoff(k):return GUEST+32 if k[5] else REGS

def initial(k,guest):
 mode,n,lo,hi,size,alias,target,trigger,replacement,sel=k;r=regoff(k)
 a=bytearray(ARENA_PATTERN)
 for o,v in ((0,lo^0xa1),(8,hi^0x71),(16,0x1234),(24,0),(32,0x4321),(40,n),(48,0x1111),(56,0x2222),(64,lo),(72,hi),(80,sel)):
  put(a,r+o,v)
 for o,v in ((128,0xabc),(160,0x987)):
  put(a,GUEST+o,v)
 for index in range(42,50):put(a,GUEST+512+index*8,777)
 put(a,DESC,guest);put(a,DESC+8,size)
 for o,v in ((0,target),(8,replacement),(16,trigger),(24,lo),(32,hi),(40,sel)):
  put(a,CONFIG+o,v)
 return a

def finish(label,kind,n,i,state,h,acc,last,tag):
 values=(acc,h,state,last,i,'%x',str(tag));s=label+':\n';v=values[0]
 for j,z in enumerate(values[1:]):
  nv=f'%{label}.answer{j}';s+=f'  {nv} = xor i64 {v}, {z}\n';v=nv
 for o,z in ((0,v),(8,h),(16,state),(24,i),(32,acc),(40,n),(48,last)):
  s+=f'  %{label}.r{o} = getelementptr i8, ptr %regs, i64 {o}\n  store i64 {z}, ptr %{label}.r{o}, align 1\n'
 for j,z in enumerate((str(kind),'0',str(0xe000+tag),last,'1' if kind==1 else '0','%x',str(tag))):
  s+=f'  %{label}.e{j} = getelementptr i8, ptr %exit, i64 {j*8}\n  store i64 {z}, ptr %{label}.e{j}, align 1\n'
 return s+f'  ret i64 {v}\n'

def ir(mode,fault=None,invalid=None):
 assert mode in MODES
 width,bits,signed=profile(mode);ty=f'i{width}';mask=(1<<width)-1;shift=width-bits;signmask=1<<(width-1)
 history=mode=='history';dynamic=mode.startswith('dynamic_')
 s='''source_filename = "guarded-bit-slices-independent-v1"
target datalayout = "e-p:64:64"
declare void @prefix_unknown(ptr,ptr,ptr)
declare void @side_unknown(ptr,ptr,ptr,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %x = load i64, ptr %regs, align 1
  %hp = getelementptr i8, ptr %regs, i64 8
  %initial_history = load i64, ptr %hp, align 1
  %np = getelementptr i8, ptr %regs, i64 40
  %initial_n = load i64, ptr %np, align 1
  %sidep = getelementptr i8, ptr %regs, i64 48
  %sourcep = getelementptr i8, ptr %regs, i64 64
  %countp = getelementptr i8, ptr %regs, i64 80
  %ledgerp = getelementptr i8, ptr %guest, i64 128
'''
 s+=f'  %initial_source = load {ty}, ptr %sourcep, align 1\n'
 call='  call void @prefix_unknown(ptr %regs, ptr %guest, ptr %config)\n'
 if fault!='omit_prefix':s+=call*(2 if fault=='double_prefix' else 1)
 s+='''  %target = load i64, ptr %config, align 1
  %replacementp = getelementptr i8, ptr %config, i64 8
  %replacement = load i64, ptr %replacementp, align 1
  %triggerp = getelementptr i8, ptr %config, i64 16
  %trigger = load i64, ptr %triggerp, align 1
  %initial_count = load i64, ptr %countp, align 1
  br label %seed
seed:
  br label %head
head:
'''
 phis={'remaining':('%initial_n','%nnext','%side.n','%callee.n'),
 'iteration':('0','%inext','%side.i','%callee.i'),
 'history':('%initial_history','%accnext' if fault=='wrong_root_history' else '%hnext','%side.h','%callee.h'),
 'acc':('%x','%accnext','%side.a','%callee.a'),
 'last':('0','%loaded','%side.last','%callee.last')}
 for name,values in phis.items():
  pairs=[(values[0],'seed'),(values[1],'dispatch')]
  if mode=='sidecall':pairs.append((values[2],'side.outside'))
  if fault!='drop_case45':pairs.append((values[3],'callee.outside'))
  s+=f'  %{name} = phi i64 '+', '.join(f'[ {v}, %{p} ]' for v,p in pairs)+'\n'
 if history:
  pairs=[('%initial_source','seed'),('%fresh','dispatch')]
  if fault!='drop_case45':pairs.append(('%callee.source','callee.outside'))
  s+=f'  %oldsource = phi {ty} '+', '.join(f'[ {v}, %{p} ]' for v,p in pairs)+'\n'
 s+=f'  %loaded_source = load {ty}, ptr %sourcep, align 1\n'
 src='%initial_source' if fault=='frozen_source' else '%loaded_source'
 if fault=='ignore_high128':
  s+='  %source_low = trunc i128 %loaded_source to i64\n  %source_zero = zext i64 %source_low to i128\n';src='%source_zero'
 if invalid in ('undef','poison'):src=invalid
 s+=f'  %fresh = freeze {ty} {src}\n'
 if invalid in ('direct_undef','direct_poison'):src=invalid.removeprefix('direct_')
 else:src='%fresh'
 count=str(shift)
 if dynamic:
  s+='  %rawcount = load i64, ptr %countp, align 1\n  %countbit = and i64 '+('%initial_count' if fault=='frozen_count' else '%rawcount')+', 1\n  %countbool = icmp ne i64 %countbit, 0\n'
  other=0 if mode=='dynamic_zero' else width-2
  s+=f'  %count = select i1 %countbool, {ty} {width-1}, {ty} {other}\n';count='%count'
 if fault=='wrong_count':count=str(shift-1)
 if invalid in ('width','above'):count=str(width+(invalid=='above'))
 opcode='ashr' if signed and fault!='logical_for_arithmetic' else 'lshr'
 s+=f'  %slice = {opcode} {ty} {src}, {count}\n'
 if width==32:s+='  %part = '+('zext' if fault=='zeroextend_signed32' else 'sext' if signed else 'zext')+' i32 %slice to i64\n'
 elif width==128:s+='  %part = trunc i128 %slice to i64\n'
 else:s+='  %part = add i64 %slice, 0\n'
 if history:
  s+=f'  %oldslice = lshr {ty} '+('%initial_source' if fault=='stale_history' else '%oldsource')+f', {width-1}\n  %oldscaled = shl i64 %oldslice, 1\n  %indexpart = add i64 %oldscaled, %part\n'
 else:s+='  %indexpart = add i64 %part, 0\n'
 s+=f'  %state = add i64 %indexpart, {42+(1<<(bits-1)) if signed else 42}\n'
 s+='''  %ledger = load i64, ptr %ledgerp, align 1
  %observe0 = xor i64 %history, %state
  %observe = xor i64 %observe0, %ledger
  %ledger_next = add i64 %ledger, %observe
  store i64 %ledger_next, ptr %ledgerp, align 1
  %done = icmp eq i64 %remaining, 0
  br i1 %done, label %finish, label %body
body:
  %side = load i64, ptr %sidep, align 1
  %running0 = add i64 %acc, %observe
  %running1 = add i64 %running0, %side
  %running = add i64 %running1, %iteration
  %nnext = sub i64 %remaining, 1
  %inext = add i64 %iteration, 1
'''
 s+=f'  %next_source = xor {ty} %fresh, {signmask}\n  store {ty} %next_source, ptr %sourcep, align 1\n'
 s+='  %countword = load i64, ptr %countp, align 1\n  %next_count = xor i64 %countword, 1\n  store i64 %next_count, ptr %countp, align 1\n'
 if mode=='sidecall':s+='  %leave = icmp eq i64 %iteration, %trigger\n  br i1 %leave, label %side.outside, label %range\n'
 else:s+='  br label %range\n'
 s+='''range:
  %state_ok = icmp ult i64 %state, 64
  br i1 %state_ok, label %index, label %state_failure
index:
  %scaled = shl i64 %state, 3
  %address = add i64 %scaled, 512
  %tableptr = getelementptr i8, ptr %guest, i64 %address
  %end = add i64 %address, 8
  %room = icmp ule i64 %end, %size
  br i1 %room, label %dispatch, label %table_failure
dispatch:
  %change = icmp uge i64 %iteration, %trigger
  %stored = select i1 %change, i64 %replacement, i64 %target
'''
 if mode=='call_inside':s+='  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %iteration)\n'
 if mode=='table_before':s+='  store i64 %stored, ptr %tableptr, align 1\n'
 s+='  %loaded = load i64, ptr %tableptr, align 1\n'
 if mode=='table_after' and fault!='drop_table_store':s+='  store i64 %stored, ptr %tableptr, align 1\n'
 selector='%loaded'
 if fault=='reload_selector':s+='  %reloaded = load i64, ptr %tableptr, align 1\n';selector='%reloaded'
 s+='  %hnext = xor i64 %observe, %loaded\n  %accnext = add i64 %running, %loaded\n'
 s+=f'  switch i64 {selector}, label %default_exit [\n    i64 43, label %head\n    i64 44, label %business\n'
 if fault!='drop_case45':s+='    i64 45, label %callee.outside\n'
 s+='    i64 777, label %alternate\n  ]\n'
 if mode=='sidecall':
  s+='side.outside:\n'
  for name,value in (('n','%nnext'),('i','%inext'),('h','%observe'),('a','%running'),('last','%last'),('calliter','%iteration')):
   s+=f'  %side.{name} = phi i64 [ {value}, %body ]\n'
  if fault!='omit_sidecall':s+='  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %side.calliter)\n'
  s+='  br label %head\n'
 if fault!='drop_case45':
  s+='callee.outside:\n'
  for name,value in (('n','%nnext'),('i','%inext'),('h','%hnext'),('a','%accnext'),('last','%loaded'),('calliter','%iteration')):
   s+=f'  %callee.{name} = phi i64 [ {value}, %dispatch ]\n'
  if history:s+=f'  %callee.source = phi {ty} [ %fresh, %dispatch ]\n'
  s+='  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %callee.calliter)\n  br label %head\n'
 s+=finish('business',1,'%nnext','%inext','%state','%hnext','%accnext','%loaded',2)
 s+=finish('alternate',1,'%nnext','%inext','%state','%hnext','%accnext','%loaded',3)
 s+=finish('default_exit',5,'%nnext','%inext','%state','%hnext','%accnext','%loaded',13)
 s+=finish('state_failure',5,'%nnext','%inext','%state','%history','%running','%state',11)
 s+=finish('table_failure',5,'%nnext','%inext','%state','%history','%running','%address',12)
 s+=finish('finish',1,'%remaining','%iteration','%state','%history','%acc','%last',1)
 return s+'}\n'

HELPER=r'''#include <stdint.h>
#include <string.h>
uint64_t business_calls;
static uint64_t rd(unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void wr(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
void prefix_unknown(unsigned char*r,unsigned char*g,unsigned char*c){
 uint64_t lo=rd(c+24),hi=rd(c+32);++business_calls;
 wr(r,lo+0x31);wr(r+8,hi^0x55);wr(r+16,0x9876);wr(r+48,0x8888);
 wr(r+56,rd(r+56)+lo+3);wr(r+64,lo^0x8000000080000000ULL);wr(r+72,hi^0x8000000000000000ULL);wr(r+80,rd(c+40));
 wr(g+80,0x9abc);wr(g+128,0);
 for(unsigned i=42;i<50;++i)wr(g+512+i*8,rd(c));
}
void side_unknown(unsigned char*r,unsigned char*g,unsigned char*c,uint64_t i){
 ++business_calls;wr(r,rd(r)+i+11);wr(r+16,rd(r+16)^1);wr(r+48,0xbeef+i);
 wr(g+80,0xcafe + i);wr(g+128,rd(g+128)+i+3);wr(r+56,rd(r+56)+i+0x33);
 wr(r+64,rd(r+64)^0x8000000080000000ULL);wr(r+72,rd(r+72)^0x8000000000000000ULL);
 for(unsigned j=42;j<50;++j)wr(g+512+j*8,rd(c+8));
}
'''

def model(k,guest,fault=None):
 mode,n,lo,hi,size,alias,target,trigger,replacement,sel=k;w,b,sgn=profile(mode);wm=(1<<w)-1
 a=initial(k,guest);r=regoff(k);x=get(a,r);h=get(a,r+8);old=int.from_bytes(a[r+64:r+64+w//8],'little');calls=0
 def prefix():
  nonlocal calls
  calls+=1
  for o,v in ((0,lo+0x31),(8,hi^0x55),(16,0x9876),(48,0x8888)):put(a,r+o,v)
  put(a,r+56,get(a,r+56)+lo+3);put(a,r+64,lo^0x8000000080000000);put(a,r+72,hi^0x8000000000000000);put(a,r+80,sel)
  put(a,GUEST+80,0x9abc);put(a,GUEST+128,0)
  for j in range(42,50):put(a,GUEST+512+j*8,target)
 def external(i):
  nonlocal calls
  calls+=1;put(a,r,get(a,r)+i+11);put(a,r+16,get(a,r+16)^1);put(a,r+48,0xbeef+i)
  put(a,GUEST+80,0xcafe+i);put(a,GUEST+128,get(a,GUEST+128)+i+3);put(a,r+56,get(a,r+56)+i+0x33)
  put(a,r+64,get(a,r+64)^0x8000000080000000);put(a,r+72,get(a,r+72)^0x8000000000000000)
  for j in range(42,50):put(a,GUEST+512+j*8,replacement)
 if fault!='omit_prefix':prefix()
 if fault=='double_prefix':prefix()
 initial_count=get(a,r+80);initial_source=old;remaining,i,acc,last=n,0,x,0
 for visits in range(100):
  fresh=initial_source if fault=='frozen_source' else int.from_bytes(a[r+64:r+64+w//8],'little')
  if fault=='ignore_high128':fresh&=MASK
  count=w-b
  if mode.startswith('dynamic_'):
   choice=(initial_count if fault=='frozen_count' else get(a,r+80))&1
   count=w-1 if choice else 0 if mode=='dynamic_zero' else w-2
  if fault=='wrong_count':count-=1
  signed=sgn and fault!='logical_for_arithmetic'
  val=fresh-(1<<w) if signed and fresh>>(w-1) else fresh
  part=(val>>count)&wm
  if w==32 and sgn and fault!='zeroextend_signed32' and part>>31:part|=MASK^((1<<32)-1)
  part&=MASK
  if mode=='history':part=(part+2*((initial_source if fault=='stale_history' else old)>>(w-1)))&MASK
  state=(part+42+((1<<(b-1)) if sgn else 0))&MASK
  ledger=get(a,GUEST+128);observe=h^state^ledger;put(a,GUEST+128,ledger+observe)
  if remaining==0:kind,tag=1,1;break
  running=(acc+observe+get(a,r+48)+i)&MASK;nextn,nexti=remaining-1,i+1
  a[r+64:r+64+w//8]=((fresh^(1<<(w-1)))&wm).to_bytes(w//8,'little')
  put(a,r+80,get(a,r+80)^1)
  if mode=='sidecall' and i==trigger:
   if fault!='omit_sidecall':external(i)
   remaining,i,h,acc=nextn,nexti,observe,running;continue
  if state>=64:
   kind,tag,last,acc=5,11,state,running;remaining,i=nextn,nexti;break
  address=512+state*8
  if address+8>size:
   kind,tag,last,acc=5,12,address,running;remaining,i=nextn,nexti;break
  if mode=='call_inside':external(i)
  stored=replacement if i>=trigger else target
  if mode=='table_before':put(a,GUEST+address,stored)
  loaded=get(a,GUEST+address)
  if mode=='table_after' and fault!='drop_table_store':put(a,GUEST+address,stored)
  selected=get(a,GUEST+address) if fault=='reload_selector' else loaded
  nh,na=observe^loaded,(running+loaded)&MASK
  if selected==43 or (selected==45 and fault!='drop_case45'):
   if selected==45:external(i)
   remaining,i,h,acc,last,old=nextn,nexti,(na if fault=='wrong_root_history' and selected==43 else nh),na,loaded,fresh;continue
  remaining,i,h,acc,last=nextn,nexti,nh,na,loaded
  kind,tag=(1,2) if selected==44 else (1,3) if selected==777 else (5,13);break
 else:raise AssertionError('fixture count failed independent bound')
 answer=(acc^h^state^last^i^x^tag)&MASK
 for o,v in ((0,answer),(8,h),(16,state),(24,i),(32,acc),(40,remaining),(48,last)):put(a,r+o,v)
 for j,v in enumerate((kind,0,0xe000+tag,last,int(kind==1),x,tag)):put(a,EXIT+j*8,v)
 return struct.pack('<QQ',answer,calls)+a

def same(actual,expected,label):
 if actual!=expected:
  at=next((i for i,(x,y) in enumerate(zip(actual,expected)) if x!=y),None)
  raise AssertionError((label,at,len(actual),len(expected)))

def emit(out):
 out.mkdir(parents=True,exist_ok=False)
 for mode in MODES:(out/(mode+'.ll')).write_text(ir(mode))
 for fault,mode in FAULTS.items():(out/('control_'+fault+'.ll')).write_text(ir(mode,fault))
 for bad in ('direct_undef','direct_poison','width','above'):(out/('refuse_'+bad+'.ll')).write_text(ir('l64t1',invalid=bad))
 (out/'helper.c').write_text(HELPER);(out/'inputs.json').write_text(json.dumps(INPUTS,indent=2)+'\n')
 (out/'reference.c').write_bytes(Path(__file__).with_name('guarded_bit_slices_reference.c').read_bytes())
 (out/'model.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'\n')

def run(before,after,wrong,out,opt,before_only=False):
 out.mkdir(parents=True,exist_ok=False);arena=(ctypes.c_ubyte*ARENA)();base=ctypes.addressof(arena);guest=base+GUEST
 report=dict(opt=opt,guest_address=guest,normal=0,c_reference=0,fault_observations=0,faults={},comparator_controls=0,errors=[],before_only=before_only)
 def compile(path,name):
  with (out/(name+'.compile.log')).open('wb') as log:
   subprocess.run(['opt-21','-passes=verify','-disable-output',str(path)],stdout=log,stderr=log,check=True,timeout=180)
   so=out/(name+'.so');subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}','-Wno-override-module',str(path),str(before/'helper.c'),'-o',str(so)],stdout=log,stderr=log,check=True,timeout=180)
  lib=ctypes.CDLL(str(so.resolve()));fn=lib.oracle;fn.argtypes=[ctypes.c_void_p]*4;fn.restype=ctypes.c_uint64;return lib,fn
 def invoke(lib,fn,k):
  ctypes.memmove(base,bytes(initial(k,guest)),ARENA);calls=ctypes.c_uint64.in_dll(lib,'business_calls');calls.value=0
  value=fn(base+regoff(k),base+DESC,base+EXIT,base+CONFIG)
  return struct.pack('<QQ',value,calls.value)+bytes(arena)
 try:
  with (out/'reference.compile.log').open('wb') as log:
   subprocess.run(['clang-21','-shared','-fPIC',f'-O{opt}',str(before/'reference.c'),str(before/'helper.c'),'-o',str(out/'reference.so')],stdout=log,stderr=log,check=True,timeout=180)
  reference=ctypes.CDLL(str((out/'reference.so').resolve()));ref=reference.reference
  ref.argtypes=[ctypes.c_void_p]*4+[ctypes.c_uint]*4;ref.restype=ctypes.c_uint64
  extra={'dynamic_legal':1,'dynamic_zero':2,'history':3,'table_before':4,'table_after':5,'sidecall':6,'call_inside':7}
  for mode in MODES:
   with gzip.open(out/('reference-'+mode+'.snapshots.gz'),'wb') as stream:
    for k in INPUTS:
     if k[0]!=mode:continue
     ctypes.memmove(base,bytes(initial(k,guest)),ARENA)
     calls=ctypes.c_uint64.in_dll(reference,'business_calls');calls.value=0
     w,b,signed=profile(mode)
     value=ref(base+regoff(k),base+DESC,base+EXIT,base+CONFIG,w,b,int(signed),extra.get(mode,0))
     actual=struct.pack('<QQ',value,calls.value)+bytes(arena);stream.write(actual)
     same(actual,model(k,guest),('independent-C-reference',k));report['c_reference']+=1
  stages=[('before',before,MODES)]
  if not before_only:stages += [('after',after,MODES),('wrong',wrong,('l64t1',))]
  for stage,directory,modes in stages:
   for mode in modes:
    lib,fn=compile(directory/(mode+'.ll'),stage+'-'+mode)
    with gzip.open(out/(stage+'-'+mode+'.snapshots.gz'),'wb') as stream:
     for k in INPUTS:
      if k[0]!=mode:continue
      actual=invoke(lib,fn,k);stream.write(actual);expected=model(k,guest);same(actual,expected,(stage,k));report['normal']+=1
      for offset in (0,8,16+regoff(k),16+GUEST+96)+tuple(16+EXIT+j*8 for j in range(7)):
       damaged=bytearray(actual);damaged[offset]^=1
       try:same(damaged,expected,'comparator-control')
       except AssertionError:report['comparator_controls']+=1
       else:raise AssertionError('comparator control escaped')
  for fault,mode in FAULTS.items():
   lib,fn=compile(before/('control_'+fault+'.ll'),'control-'+fault);required=caught=0
   with gzip.open(out/('control-'+fault+'.snapshots.gz'),'wb') as stream:
    for k in INPUTS:
     if k[0]!=mode:continue
     actual=invoke(lib,fn,k);stream.write(actual);damaged,normal=model(k,guest,fault),model(k,guest)
     same(actual,damaged,('fault-model',fault,k));report['fault_observations']+=1
     if damaged!=normal:
      required+=1
      try:same(actual,normal,('fault',fault,k))
      except AssertionError:caught+=1
      else:raise AssertionError('fault escaped')
   assert required and caught==required;report['faults'][fault]=dict(required=required,caught=caught)
 except Exception as e:report['errors'].append(repr(e));raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))

if __name__=='__main__':
 p=argparse.ArgumentParser();sub=p.add_subparsers(dest='action',required=True);e=sub.add_parser('emit');e.add_argument('out',type=Path);r=sub.add_parser('run')
 for name in ('before','after','wrong','out'):r.add_argument(name,type=Path)
 r.add_argument('--opt',type=int,choices=(0,2),required=True);r.add_argument('--before-only',action='store_true');a=p.parse_args()
 if a.action=='emit':emit(a.out)
 else:run(a.before,a.after,a.wrong,a.out,a.opt,a.before_only)
