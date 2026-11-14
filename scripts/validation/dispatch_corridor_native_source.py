"""Emit-only independent C/Python/LLVM sources for a bounded corridor campaign.
No cloner, compiler, native target or prior oracle is imported or executed.
"""
import argparse
import hashlib
import json
from pathlib import Path

FRAME=66112
M32=(1<<32)-1
M64=(1<<64)-1
NAMES=('historical-loop','bit-ordered-alias','unknown-post-store','historical-state')
CONTROLS=(0,32,64,80,96,112,200,256,264,2048,8704,66048,FRAME-1)
WORDS=(0,1,2,3,7,0x7f,0x80,0x7fff,0x8000,0x7fffffff,0x80000000,0xfffffffe,0xffffffff,0xaaaaaaaa,0x55555555,16)
LIMITS=(0,4,M32,3,7,0,4,M32,0x7fffffff,4,2,8,M32,4,0,4)
UNKNOWN=(0,2,3,1,7,M32,2,3,0,4,5,2,3,0,M32,7)
EXIT_BASE=0xd15c0ff000000000
WRAPPER=r'''
#include <stdio.h>
#include <stdlib.h>
unsigned oracle_case;
void probe(unsigned char *);
int main(int argc,char **argv) {
 unsigned char frame[66112];size_t count;
 if(argc!=2)return 4;
 oracle_case=(unsigned)strtoul(argv[1],0,10);if(oracle_case>=4)return 4;
 while((count=fread(frame,1,sizeof frame,stdin))!=0){
  if(count!=sizeof frame)return 2;probe(frame);
  if(fwrite(frame,1,sizeof frame,stdout)!=sizeof frame)return 3;
 }
 return ferror(stdin)||fflush(stdout)!=0;
}
'''
# Independent C: byte packing and structured loops, never evaluates LLVM output.
C_SPEC=r'''
#include <stdint.h>
extern unsigned oracle_case;
static uint32_t word(const unsigned char*p,unsigned at){
 uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[at+i]<<(8*i);return v;
}
static uint64_t wide(const unsigned char*p,unsigned at){
 uint64_t v=0;for(unsigned i=8;i;--i)v=(v<<8)|p[at+i-1];return v;
}
static void w32(unsigned char*p,unsigned at,uint32_t v){
 for(unsigned i=0;i<4;++i){p[at+i]=(unsigned char)v;v>>=8;}
}
static void w64(unsigned char*p,unsigned at,uint64_t v){
 for(unsigned i=0;i<8;++i){p[at+i]=(unsigned char)v;v>>=8;}
}
static void bump(unsigned char*p,unsigned at){w64(p,at,wide(p,at)+UINT64_C(1));}
static void finish(unsigned char*p,unsigned kind,unsigned reason,uint32_t result){
 w32(p,64,result);w64(p,66048,UINT64_C(0xd15c0ff000000000)+((uint64_t)kind<<16)+reason);
}
void probe(unsigned char*p){
 uint32_t raw=word(p,0),route=word(p,8),unknown=word(p,16);
 if(oracle_case==0){
  unsigned trips=3+word(p,4)%4,iteration=0;uint32_t history=raw,state=0;
  for(;;){
   w32(p,80,history);w32(p,208,state);bump(p,256);
   if(state>1){finish(p,0,2,99);return;}
   if(iteration==trips){w32(p,200,iteration);finish(p,0,0,history);return;}
   uint32_t next=history+UINT32_C(3);
   w32(p,96,history);w32(p,112,next);w32(p,512,history);bump(p,264);
   if(next>word(p,20)){w32(p,200,iteration);finish(p,0,1,history);return;}
   state=((route+iteration)%2)?1:0;history=next;++iteration;
  }
 }
 if(oracle_case==3){
  uint32_t history=(raw%2)+10;w32(p,80,history);w32(p,208,0);bump(p,256);
  uint32_t next=history+1;w32(p,96,history);w32(p,112,next);bump(p,264);w32(p,200,1);
  w32(p,80,20);w32(p,208,next);bump(p,256);finish(p,3,0,next);return;
 }
 uint32_t state;
 if(oracle_case==1){
  state=(route%2)?((raw%2)^3):unknown;w32(p,512,state);bump(p,256);
  uint32_t size=word(p,word(p,12)?512:20);w32(p,80,size);w32(p,96,state);
  if(size<4){w32(p,200,0);finish(p,1,1,state);return;}w32(p,200,1);
 }else{
  state=(route%2)?2:0;w32(p,512,state);w32(p,word(p,12)?512:1024,unknown);
  state=word(p,512);w32(p,96,state);bump(p,256);
 }
 w32(p,208,state);bump(p,264);
 if(state==0){bump(p,288);finish(p,oracle_case,0,raw^UINT32_C(0xf0f0f0f0));}
 else if(state==2){bump(p,272);finish(p,oracle_case,0,raw^UINT32_C(0x22));}
 else if(state==3){bump(p,280);finish(p,oracle_case,0,raw+UINT32_C(0x33));}
 else finish(p,oracle_case,2,state^UINT32_C(0xbad00000));
}
'''


def initial(trial):
 if not 0<=trial<16:raise ValueError('trial outside frozen domain')
 frame=bytearray(hashlib.shake_256(f'dispatch-corridor/20261114/{trial}'.encode()).digest(FRAME))
 for at,value in ((0,WORDS[trial]),(4,trial),(8,trial),(12,(trial//2)&1),(16,UNKNOWN[trial]),(20,LIMITS[trial])):
  frame[at:at+4]=value.to_bytes(4,'little')
 for at in (256,264,272,280,288):frame[at:at+8]=(M64-trial%3).to_bytes(8,'little')
 return bytes(frame)


def specification(source,name):
 arena=bytearray(source)
 if len(arena)!=FRAME or name not in NAMES:raise ValueError('invalid source domain')
 kind=NAMES.index(name)
 read=lambda at,n=4:int.from_bytes(arena[at:at+n],'little')
 def write(at,value,n=4):arena[at:at+n]=(value&((1<<(8*n))-1)).to_bytes(n,'little')
 def bump(at):write(at,read(at,8)+1,8)
 coverage=dict(header_visits=0,prefix_visits=0,dispatches=0,early_fault=False,partial_unknown_path=False)
 def finish(reason,result):
  write(64,result);write(66048,EXIT_BASE+(kind<<16)+reason,8);coverage['reason']=reason
  return bytes(arena),coverage
 raw,route,unknown=read(0),read(8),read(16)
 if kind==0:
  history,state,iteration=raw,0,0;trips=3+(read(4)&3)
  while True:
   write(80,history);write(208,state);bump(256);coverage['header_visits']+=1;coverage['dispatches']+=1
   if state not in (0,1):return finish(2,99)
   if iteration>=trips:write(200,iteration);return finish(0,history)
   next_value=(history+3)&M32
   write(96,history);write(112,next_value);write(512,history);bump(264);coverage['prefix_visits']+=1
   if next_value>read(20):
    write(200,iteration);coverage['early_fault']=True;return finish(1,history)
   state=(route+iteration)&1;history,iteration=next_value,iteration+1
 if kind==3:
  history=10+(raw&1);write(80,history);write(208,0);bump(256)
  next_value=history+1;write(96,history);write(112,next_value);bump(264);write(200,1)
  write(80,20);write(208,next_value);bump(256)
  coverage.update(header_visits=2,prefix_visits=1,dispatches=2)
  return finish(0,next_value)
 if kind==1:
  state=((raw&1)^3) if route&1 else unknown;coverage['partial_unknown_path']=not bool(route&1)
  write(512,state);bump(256);coverage['prefix_visits']=1
  size=read(512 if read(12) else 20);write(80,size);write(96,state)
  if size<4:write(200,0);coverage['early_fault']=True;return finish(1,state)
  write(200,1)
 else:
  state=2 if route&1 else 0;write(512,state);write(512 if read(12) else 1024,unknown)
  state=read(512);write(96,state);bump(256);coverage['prefix_visits']=1
 write(208,state);bump(264);coverage['dispatches']=1
 if state==0:bump(288);return finish(0,raw^0xf0f0f0f0)
 if state==2:bump(272);return finish(0,raw^0x22)
 if state==3:bump(280);return finish(0,raw+0x33)
 return finish(2,state^0xbad00000)


OFFSETS=(0,4,8,12,16,20,64,80,96,112,200,208,256,264,272,280,288,512,1024,66048)
def entry(extra=''):
 pointers='\n'.join(f'  %p{n}=getelementptr i8,ptr %frame,i64 {n}' for n in OFFSETS)
 return ('target datalayout = "e-p:64:64"\ndefine void @probe(ptr %frame) {\nentry:\n'+pointers+
  '\n  %raw=load i32,ptr %p0,align 1\n  %route=load i32,ptr %p8,align 1\n  %unknown=load i32,ptr %p16,align 1\n'+extra)

def bump_ir(at,label):
 return (f'  %{label}.old=load i64,ptr %p{at},align 1\n  %{label}.new=add i64 %{label}.old,1\n'
         f'  store i64 %{label}.new,ptr %p{at},align 1\n')

def finish_ir(kind,reason,value):
 return (f'  store i32 {value},ptr %p64,align 1\n'
         f'  store i64 {EXIT_BASE+(kind<<16)+reason},ptr %p66048,align 1\n  ret void\n')


def historical_loop():
 ir=entry('  %trip.seed=load i32,ptr %p4,align 1\n  %trip.bits=and i32 %trip.seed,3\n'
          '  %trips=add i32 %trip.bits,3\n  br label %header\n')
 ir+='''header:
  %state=phi i32 [0,%entry],[%next.state,%step]
  %history=phi i32 [%raw,%entry],[%new.history,%step]
  %iteration=phi i32 [0,%entry],[%next.iteration,%step]
  store i32 %history,ptr %p80,align 1
  store i32 %state,ptr %p208,align 1
'''+bump_ir(256,'header.count')+'''  switch i32 %state,label %error [i32 0,label %check i32 1,label %check]
check:
  %more=icmp ult i32 %iteration,%trips
  br i1 %more,label %route.block,label %done
route.block:
  %route.sum=add i32 %route,%iteration
  %route.bit=and i32 %route.sum,1
  %choose=icmp ne i32 %route.bit,0
  br i1 %choose,label %seed.a,label %seed.b
seed.a: br label %join
seed.b: br label %join
join:
  %next.state=phi i32 [1,%seed.a],[0,%seed.b]
  %carried=phi i32 [%history,%seed.a],[%history,%seed.b]
  %new.history=add i32 %history,3
  store i32 %history,ptr %p96,align 1
  store i32 %new.history,ptr %p112,align 1
  store i32 %carried,ptr %p512,align 1
'''+bump_ir(264,'prefix.count')+'''  %limit=load i32,ptr %p20,align 1
  %fits=icmp ule i32 %new.history,%limit
  br i1 %fits,label %step,label %fault
step:
  %next.iteration=add i32 %iteration,1
  br label %header
fault:
  %past=phi i32 [%history,%join]
  store i32 %iteration,ptr %p200,align 1
'''+finish_ir(0,1,'%past')+'done:\n  store i32 %iteration,ptr %p200,align 1\n'+finish_ir(0,0,'%history')+'error:\n'+finish_ir(0,2,'99')+'}\n'
 return ir


def case_tails(kind):
 return ('zero:\n'+bump_ir(288,'zero.count')+'  %zero.result=xor i32 %raw,4042322160\n'+finish_ir(kind,0,'%zero.result')+
 'two:\n'+bump_ir(272,'two.count')+'  %two.result=xor i32 %raw,34\n'+finish_ir(kind,0,'%two.result')+
 'three:\n'+bump_ir(280,'three.count')+'  %three.result=add i32 %raw,51\n'+finish_ir(kind,0,'%three.result')+
 'error:\n  %error.result=xor i32 %state,3134193664\n'+finish_ir(kind,2,'%error.result')+'}\n')


def bit_ordered_alias():
 return (entry('  %alias=load i32,ptr %p12,align 1\n  %aliases=icmp ne i32 %alias,0\n'
 '  %size=select i1 %aliases,ptr %p512,ptr %p20\n  %route.bit=and i32 %route,1\n'
 '  %choose=icmp ne i32 %route.bit,0\n  br i1 %choose,label %seed,label %other\n')+'''seed:
  %bit=and i32 %raw,1
  %choice=xor i32 %bit,3
  br label %head
other: br label %head
head:
  %state=phi i32 [%choice,%seed],[%unknown,%other]
  store i32 %state,ptr %p512,align 1
'''+bump_ir(256,'head.count')+'''  %limit=load i32,ptr %size,align 1
  store i32 %limit,ptr %p80,align 1
  store i32 %state,ptr %p96,align 1
  %fits=icmp uge i32 %limit,4
  br i1 %fits,label %dispatch,label %fault
fault:
  %fault.state=phi i32 [%state,%head]
  store i32 0,ptr %p200,align 1
'''+finish_ir(1,1,'%fault.state')+'''dispatch:
  store i32 1,ptr %p200,align 1
  store i32 %state,ptr %p208,align 1
'''+bump_ir(264,'dispatch.count')+'''  switch i32 %state,label %error [i32 0,label %zero i32 2,label %two i32 3,label %three]
'''+case_tails(1))


def unknown_post_store():
 return (entry('  %alias=load i32,ptr %p12,align 1\n  %aliases=icmp ne i32 %alias,0\n'
 '  %other.ptr=select i1 %aliases,ptr %p512,ptr %p1024\n  %route.bit=and i32 %route,1\n'
 '  %choose=icmp ne i32 %route.bit,0\n  br i1 %choose,label %seed,label %other\n')+'''seed: br label %head
other: br label %head
head:
  %stored=phi i32 [2,%seed],[0,%other]
  store i32 %stored,ptr %p512,align 1
  store i32 %unknown,ptr %other.ptr,align 1
  %state=load i32,ptr %p512,align 1
  store i32 %state,ptr %p96,align 1
'''+bump_ir(256,'head.count')+'''  store i32 %state,ptr %p208,align 1
'''+bump_ir(264,'dispatch.count')+'''  switch i32 %state,label %error [i32 0,label %zero i32 2,label %two i32 3,label %three]
'''+case_tails(2))


def historical_state():
 return (entry('  %low=and i32 %raw,1\n  %initial.history=add i32 %low,10\n  br label %header\n')+'''header:
  %state=phi i32 [0,%entry],[%next,%step]
  %history=phi i32 [%initial.history,%entry],[20,%step]
  store i32 %history,ptr %p80,align 1
  store i32 %state,ptr %p208,align 1
'''+bump_ir(256,'header.count')+'''  switch i32 %state,label %error [i32 0,label %seed i32 11,label %eleven i32 12,label %twelve i32 21,label %twentyone]
seed: br label %join
join:
  %next=add i32 %history,1
  store i32 %history,ptr %p96,align 1
  store i32 %next,ptr %p112,align 1
'''+bump_ir(264,'prefix.count')+'''  store i32 1,ptr %p200,align 1
  br label %step
step: br label %header
eleven:
'''+finish_ir(3,0,'11')+'twelve:\n'+finish_ir(3,0,'12')+'twentyone:\n'+finish_ir(3,0,'21')+'error:\n'+finish_ir(3,2,'99')+'}\n')


def sources():
 return dict(zip(NAMES,(historical_loop(),bit_ordered_alias(),unknown_post_store(),historical_state())))


def emit(out):
 out.mkdir(parents=True,exist_ok=False);irs=sources()
 candidates={NAMES[0]:[dict(source=s,successor_slot=0,path=['join','step','header']) for s in ('seed.a','seed.b')],
 NAMES[1]:[dict(source=s,successor_slot=0,path=['head','dispatch']) for s in ('seed','other')],
 NAMES[2]:[dict(source=s,successor_slot=0,path=['head']) for s in ('seed','other')],
 NAMES[3]:[dict(source='seed',successor_slot=0,path=['join','step','header'])]}
 for name,ir in irs.items():(out/(name+'.before.ll')).write_text(ir)
 old='  store i32 %history,ptr %p96,align 1\n';phi='  %past=phi i32 [%history,%join]\n'
 assert irs[NAMES[0]].count(old)==irs[NAMES[0]].count(phi)==1
 wrong=irs[NAMES[0]].replace(old,'  store i32 %new.history,ptr %p96,align 1\n').replace(phi,'  %past=phi i32 [%new.history,%join]\n')
 (out/'historical-loop.controlled-wrong-history.ll').write_text(wrong)
 wrong=irs[NAMES[3]]
 for leaf in ('eleven','twelve'):
  token='label %'+leaf;assert wrong.count(token)==1;wrong=wrong.replace(token,'label %twentyone')
 (out/'historical-state.controlled-wrong-target.ll').write_text(wrong)
 (out/'wrapper.c').write_text(WRAPPER);(out/'reference.c').write_text(C_SPEC)
 cases=[]
 for n,name in enumerate(NAMES):
  coverage=[specification(initial(t),name)[1] for t in range(16)]
  cases.append(dict(name=name,function='probe',candidates=candidates[name],expected_rewrites=(2,1,0,0)[n],
   expected_rejected=(0,1,2,1)[n],maximum_prefix_iterations=6 if n==0 else 1,
   maximum_header_visits=7 if n==0 else 2 if n==3 else 0,coverage=coverage))
 files={p.name:dict(size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(out.iterdir())}
 plan=dict(schema='dispatch-corridor-native-source-plan-v1',ready=False,cases=cases,files=files,
  source_generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
  input_hashes=[hashlib.sha256(initial(t)).hexdigest() for t in range(16)],
  domain=dict(fixtures=4,inputs_per_fixture=16,unique_module_inputs=64,unique_initial_arenas=16,widths=[32],host_opts=['O0','O2'],frame_bytes=FRAME),
  planned_counts=dict(before_C_matches=128,before_LLVM_matches=128,before_comparator_controls=3328,after_LLVM_matches=128,
   after_comparator_controls=1664,actual_fault_observations=64,specified_fault_detections=64),
  faults=[dict(name='wrong-history',source=NAMES[0],operand_edits=2,required_per_opt=16,required_field_offset=96),
   dict(name='wrong-target',source=NAMES[3],successor_edits=2,required_per_opt=16,required_field_offset=64)],
  thresholds=dict(startup_bytes=512*1024**2,running_floor_bytes=64*1024**2,cli_seconds=180),
  original_73dc_native_execution_claimed=False,
  scope='Emit-only local source/math. Before C/Python/LLVM calibration required before any newafter. Deliberatefault IR is never original73dc output. No skips/exclusions/normalization/CFFcompletion claim.')
 (out/'PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')


if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--emit',type=Path,required=True)
 emit(parser.parse_args().emit)
