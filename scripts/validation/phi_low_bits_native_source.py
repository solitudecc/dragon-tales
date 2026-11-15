"""Independent u32-history PhiLowBits C/Python/LLVM source preparation.

Emission only: no product, compiler, emulator or native target is imported.
The framing derives from the frozen corridor campaign; the program models,
inputs and expectations below are a new domain with explicit uint32 wrapping.
"""
import argparse
import hashlib
import json
from pathlib import Path

FRAME, GUEST, REGS, ALIAS_REGS, EXIT, RETURN = 66112, 65536, 65536, 0x8fd0, 66048, 66104
M32, M64 = (1 << 32)-1, (1 << 64)-1
NAMES = ('phi-low-shared-step', 'phi-low-bad-child-step')
WORDS = (0, 1, M64, 1 << 63, (1 << 63)-1, 0xaaaaaaaaaaaaaaaa, 0x5555555555555555, 0xffffffff)
CONTROLS = (0, 24, 32, 1024, 1032, 1040, 1056, 1104, 1112, 1120, 1128,
            36864, 36871, ALIAS_REGS, ALIAS_REGS+143, REGS, REGS+48, REGS+143, REGS+511,
            EXIT, EXIT+8, EXIT+16, EXIT+24, EXIT+32, EXIT+40, EXIT+48, RETURN, FRAME-1)
FAULT = dict(name='bad-child-trace',source=NAMES[1],required_trials=[0,1,2,3,4,7],
             nontrigger_trials=[5,6],offset=1056,width=8)

# This framing is byte-compatible with the old arena, and uses the real SDK
# descriptor declarations. No old model or expected bytes are imported.
WRAPPER = r'''
#include <dragon/lifters/llvm/Execution.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
_Static_assert(sizeof(dragon_guest_state_v2_t)==32,"state ABI");
_Static_assert(offsetof(dragon_guest_state_v2_t,regs_bytes)==24,"register descriptor ABI");
_Static_assert(sizeof(dragon_guest_memory_v2_t)==16,"memory ABI");
_Static_assert(sizeof(dragon_exec_exit_v2_t)==56,"exit ABI");
_Static_assert(sizeof(dragon_exec_context_v2_t)==32,"context ABI");
unsigned oracle_case;
extern void view(dragon_guest_state_v2_t*,dragon_guest_memory_v2_t*,dragon_exec_context_v2_t*,dragon_exec_exit_v2_t*);
static uint64_t read64(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void write64(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
int main(int argc,char**argv){
 _Alignas(16) unsigned char frame[66112];size_t n;if(argc!=2)return 4;
 oracle_case=(unsigned)strtoul(argv[1],0,10);if(oracle_case>=2)return 4;
 while((n=fread(frame,1,sizeof frame,stdin))!=0){
  if(n!=sizeof frame)return 2;
  unsigned at=(read64(frame+8)&1)?0x8fd0:65536;uint64_t size=read64(frame+16);
  if(size<2048||size>65536)return 4;
  dragon_guest_state_v2_t s={2,1,512,frame+at};
  dragon_guest_memory_v2_t m={frame,size};dragon_exec_context_v2_t c={0,0,37,0};
  view(&s,&m,&c,(dragon_exec_exit_v2_t*)(void*)(frame+66048));
  if(s.abi_version!=2||s.architecture!=1||s.regs_size!=512||s.regs_bytes!=frame+at||
     m.data!=frame||m.size!=size||c.entries||c.entry_count||c.budget!=37||c.user_data)return 5;
  write64(frame+66104,read64(frame+at));
  if(fwrite(frame,1,sizeof frame,stdout)!=sizeof frame)return 3;
 }
 return ferror(stdin)||fflush(stdout)!=0;
}
'''

C_SPEC = r'''
#include <dragon/lifters/llvm/Execution.h>
extern unsigned oracle_case;
static uint64_t rd(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void wr(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
static void bump(unsigned char*g,unsigned at){wr(g+at,rd(g+at)+UINT64_C(1));}
void view(dragon_guest_state_v2_t*s,dragon_guest_memory_v2_t*m,dragon_exec_context_v2_t*c,dragon_exec_exit_v2_t*e){
 (void)c;unsigned char*g=m->data,*r=s->regs_bytes;
 uint64_t raw=rd(g),state=0,iteration=0,trips=75+(raw&3);
 uint32_t history=(uint32_t)rd(r+48),child=history;
 uint64_t result=(uint64_t)history,reason=1,kind=3,source=4608,next=0xfeed,valid=1,ret=0xfeed,detail=0;
 for(;;){
  /* History and trace are independently zero-extended from the u32 value. */
  wr(g+1056,(uint64_t)history);wr(g+1104,(uint64_t)history);wr(g+1112,state);wr(g+1120,iteration);bump(g,1024);
  if(iteration>=trips){result=(uint64_t)history;break;}
  if(m->size<36872){reason=5;kind=0;source=next=4353;valid=ret=0;detail=3;result=(uint64_t)history;break;}
  wr(g+36864,((uint64_t)history^raw)+state);uint64_t fresh=rd(r+48);wr(g+1128,fresh);bump(g,1032);
  uint32_t step=(uint32_t)(history+UINT32_C(3));
  if((uint64_t)step>rd(g+24)){reason=6;kind=0;source=next=4354;valid=ret=0;result=(uint64_t)history;break;}
  if(state>=70){reason=4;kind=1;source=4350;next=state;ret=0;detail=3;result=(uint64_t)history;break;}
  /* The bad fixture's child is deliberately unobservable in the original. */
  child=oracle_case?(uint32_t)(child+UINT32_C(4)):step;
  state=(state+1)%70;history=step;++iteration;
 }
 (void)child;bump(g,1040);e->reason=reason;e->transfer_kind=kind;e->source_pc=source;e->next_pc=next;
 e->next_pc_valid=valid;e->return_pc=ret;e->detail=detail;
 wr(r,result^raw^rd(g+1032));wr(r+128,next);wr(r+143,result+rd(r+48));
}
'''


def initial(trial):
    if not 0 <= trial < 8: raise ValueError('trial outside new fixed domain')
    a=bytearray(hashlib.shake_256(('phi-low-bits/u32/20261115/'+str(trial)).encode()).digest(FRAME))
    def w(at,value): a[at:at+8]=(value&M64).to_bytes(8,'little')
    alias=trial in (1,2,4,6,7)
    w(0,WORDS[trial]);w(8,int(alias));w(16,8191 if trial==6 else 65536)
    w(24,0 if trial==5 else M64);w(32,1)
    regs=ALIAS_REGS if alias else REGS
    w(regs+32,49152);w(regs+48,WORDS[(trial+2)%8]);w(regs+128,4096)
    for at in (1024,1032,1040):w(at,M64-trial%3)
    return bytes(a)


def specification(source,name):
    if len(source)!=FRAME or name not in NAMES:raise ValueError('wrong new source domain')
    a=bytearray(source)
    def rd(at):return int.from_bytes(a[at:at+8],'little')
    def wr(at,value):a[at:at+8]=(value&M64).to_bytes(8,'little')
    def bump(at):wr(at,rd(at)+1)
    raw,regs,size=rd(0),ALIAS_REGS if rd(8)&1 else REGS,rd(16)
    history=rd(regs+48)&M32;child=history;state=iteration=0;trips=75+(raw&3)
    fields=[1,3,4608,0xfeed,1,0xfeed,0];result=history;seen=set()
    coverage=dict(alias=regs==ALIAS_REGS,headers=0,effects=0,dispatches=0,states=[],
                  size_error=False,limit_error=False,history_wraps=0,child_wraps=0,child_disagrees=False)
    while True:
        wr(1056,history);wr(1104,history);wr(1112,state);wr(1120,iteration);bump(1024);coverage['headers']+=1
        if iteration>=trips:result=history;break
        if size<36872:
            fields=[5,0,4353,4353,0,0,3];result=history;coverage['size_error']=True;break
        wr(36864,(history^raw)+state);wr(1128,rd(regs+48));bump(1032);coverage['effects']+=1
        step=(history+3)&M32
        if step>rd(24):
            fields=[6,0,4354,4354,0,0,0];result=history;coverage['limit_error']=True;break
        coverage['dispatches']+=1;seen.add(state)
        if state>=70:fields=[4,1,4350,state,1,0,3];result=history;break
        next_child=(child+4)&M32 if name==NAMES[1] else step
        coverage['history_wraps']+=step<history;coverage['child_wraps']+=next_child<child
        child=next_child;history=step;state=(state+1)%70;iteration+=1
    coverage['child_disagrees']=child!=history;coverage['states']=sorted(seen)
    coverage['parent_low']=history;coverage['child_low']=child;coverage['exit_reason']=fields[0]
    bump(1040)
    for n,value in enumerate(fields):wr(EXIT+8*n,value)
    wr(regs,result^raw^rd(1032));wr(regs+128,fields[3]);wr(regs+143,result+rd(regs+48));wr(RETURN,rd(regs))
    return bytes(a),coverage


OFFSETS=(0,8,24,1024,1032,1040,1056,1104,1112,1120,1128,36864)


def prefix():
    return ('target datalayout = "e-p:64:64"\ndefine void @view(ptr %s,ptr %m,ptr %c,ptr %e){\nentry:\n'
            ' %regs.addr=getelementptr i8,ptr %s,i64 24\n %regs=load ptr,ptr %regs.addr,align 1\n'
            ' %g=load ptr,ptr %m,align 1\n %size.addr=getelementptr i8,ptr %m,i64 8\n'+
            ''.join(f' %g{at}=getelementptr i8,ptr %g,i64 {at}\n' for at in OFFSETS)+
            ''.join(f' %r{at}=getelementptr i8,ptr %regs,i64 {at}\n' for at in (48,128,143))+
            ' %raw=load i64,ptr %g0,align 1\n %initial64=load i64,ptr %r48,align 1\n'
            ' %initial.low=trunc i64 %initial64 to i32\n %shared.init=freeze i32 %initial.low\n'
            ' %wide.init=zext i32 %shared.init to i64\n')


def bump_ir(at,name):
    return (f' %{name}.old=load i64,ptr %g{at},align 1\n %{name}.new=add i64 %{name}.old,1\n'
            f' store i64 %{name}.new,ptr %g{at},align 1\n')


def epilogue():
    rows=[('success','%parent',1,3,4608,65261,1,65261,0),('sizeerror','%parent',5,0,4353,4353,0,0,3),
          ('limiterror','%past',6,0,4354,4354,0,0,0),('stateerror','%parent',4,1,4350,'%state64',1,0,3)]
    ir='finish:\n'
    for index,field in enumerate(('result','reason','kind','source','next','valid','ret','detail'),1):
        ir+=f' %{field}=phi i64 '+','.join(f'[{row[index]},%{row[0]}]' for row in rows)+'\n'
    ir+=bump_ir(1040,'finish.count')
    for index,field in enumerate(('reason','kind','source','next','valid','ret','detail')):
        ir+=f' %e{index}=getelementptr i8,ptr %e,i64 {8*index}\n store i64 %{field},ptr %e{index},align 1\n'
    return (ir+' %mix=xor i64 %result,%raw\n %effects=load i64,ptr %g1032,align 1\n'
            ' %rax=xor i64 %mix,%effects\n store i64 %rax,ptr %regs,align 1\n'
            ' store i64 %next,ptr %r128,align 1\n %fresh.final=load i64,ptr %r48,align 1\n'
            ' %vector=add i64 %result,%fresh.final\n store i64 %vector,ptr %r143,align 1\n ret void\n}\n')


def llvm_source(bad_child=False):
    seeds=[(f'seed{n}',(n+1)%70) for n in range(70)]
    slots=[entry for entry in seeds for _ in range(2 if entry[0]=='seed0' else 1)]
    ir=prefix()+' %trip.bits=and i64 %raw,3\n %trips=add i64 %trip.bits,75\n'
    ir+=' %route.bit=and i64 %raw,1\n %route=icmp ne i64 %route.bit,0\n br label %head\nhead:\n'
    ir+=' %state=phi i32 [0,%entry]'+''.join(f',[{value},%{seed}]' for seed,value in slots)+'\n'
    ir+=' %parent=phi i64 [%wide.init,%entry]'+''.join(f',[%wide.next,%{seed}]' for seed,_ in slots)+'\n'
    child_next='%child.next' if bad_child else '%primary.step'
    ir+=' %child=phi i32 [%shared.init,%entry]'+''.join(f',[{child_next},%{seed}]' for seed,_ in slots)+'\n'
    ir+=' %iteration=phi i64 [0,%entry]'+''.join(f',[%stepped,%{seed}]' for seed,_ in slots)+'\n'
    ir+=' %parent.low=trunc i64 %parent to i32\n %primary.step=add i32 %parent.low,3\n'
    if bad_child:ir+=' %child.next=add i32 %child,4\n'
    ir+=(' %wide.next=zext i32 %primary.step to i64\n %stepped=add i64 %iteration,1\n'
         ' %low.trace=zext i32 %parent.low to i64\n store i64 %low.trace,ptr %g1056,align 1\n'
         ' store i64 %parent,ptr %g1104,align 1\n %state64=zext i32 %state to i64\n'
         ' store i64 %state64,ptr %g1112,align 1\n store i64 %iteration,ptr %g1120,align 1\n')
    ir+=bump_ir(1024,'header.count')+' %done=icmp uge i64 %iteration,%trips\n br i1 %done,label %success,label %sizecheck\n'
    ir+=('sizecheck:\n %logical.size=load i64,ptr %size.addr,align 1\n %sized=icmp uge i64 %logical.size,36872\n'
         ' br i1 %sized,label %effect,label %sizeerror\neffect:\n %mixed=xor i64 %parent,%raw\n'
         ' %output=add i64 %mixed,%state64\n store i64 %output,ptr %g36864,align 1\n'
         ' %fresh=load i64,ptr %r48,align 1\n store i64 %fresh,ptr %g1128,align 1\n')
    ir+=bump_ir(1032,'body.count')+' %limit=load i64,ptr %g24,align 1\n %limited=icmp ule i64 %wide.next,%limit\n'
    ir+=' br i1 %limited,label %dispatch,label %limiterror\ndispatch:\n switch i32 %state,label %stateerror ['
    ir+=''.join(f'i32 {n},label %seed{n} ' for n in range(70))+']\n'
    for seed,_ in seeds:ir+=seed+(': br i1 %route,label %head,label %head\n' if seed=='seed0' else ': br label %head\n')
    ir+='success: br label %finish\nsizeerror: br label %finish\nlimiterror:\n %past=phi i64 [%parent,%effect]\n br label %finish\nstateerror: br label %finish\n'
    return ir+epilogue()


def emit(out):
    out.mkdir(parents=True,exist_ok=False)
    modules={NAMES[0]:llvm_source(False),NAMES[1]:llvm_source(True)}
    for name,ir in modules.items():
        (out/(name+'.ll')).write_text(ir)
        query=dict(function='view',explicit_truncs=['parent.low'])
        (out/(name+'.candidates.json')).write_text(json.dumps(query,indent=2)+'\n')
    bad=modules[NAMES[1]];old='%low.trace=zext i32 %parent.low to i64';new='%low.trace=zext i32 %child to i64'
    if bad.count(old)!=1:raise ValueError('trace fault sentinel not unique')
    (out/'bad-child-trace.fault.ll').write_text(bad.replace(old,new))
    (out/'wrapper.c').write_text(WRAPPER);(out/'reference.c').write_text(C_SPEC)
    for trial in range(8):(out/('input-'+str(trial)+'.bin')).write_bytes(initial(trial))
    files={p.name:dict(size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(out.iterdir())}
    plan=dict(schema='phi-low-bits-native-source-v1',source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              names=list(NAMES),frame=FRAME,trials=8,controls=list(CONTROLS),fault=FAULT,files=files,
              precision=[dict(name=NAMES[0],rewritten_truncs=1,rewritten_uses=2),dict(name=NAMES[1],rewritten_truncs=0,byte_equal=True)],
              scope='new u32 program domain; independent C/Python model; candidate manifest not a certificate; zero native/product/emulator execution')
    (out/'PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--out',required=True,type=Path);emit(parser.parse_args().out)
