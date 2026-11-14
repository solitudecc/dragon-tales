"""Emit-only independent batch-corridor C/Python/LLVM inputs.

No SDK, product output, compiler, emulator or native executable is loaded here.
The two implementations and Python model share only the declared byte protocol.
"""
import argparse
import hashlib
import json
from pathlib import Path

FRAME, GUEST, REGS, ALIAS_REGS, EXIT, RETURN = 66112, 65536, 65536, 0x8fd0, 66048, 66104
M64 = (1 << 64) - 1
NAMES = ('seventy-state-history', 'two-state-bound-order')
WORDS = (0, 1, M64, 1 << 63, (1 << 63)-1, 0xaaaaaaaaaaaaaaaa, 0x5555555555555555, 0xffffffff)
CONTROLS = (0, 24, 32, 1024, 1032, 1040, 1104, 1112, 1120, 1128,
            36864, 36871, ALIAS_REGS, ALIAS_REGS+143,
            REGS, REGS+48, REGS+143, REGS+511,
            EXIT, EXIT+8, EXIT+16, EXIT+24, EXIT+32, EXIT+40, EXIT+48, RETURN, FRAME-1)
FAULTS = {
    'wrong-history': dict(source=NAMES[0], trials=list(range(8)), offset=1104),
    'skip-size-error': dict(source=NAMES[0], trials=[6], offset=EXIT),
    'repeat-effect': dict(source=NAMES[0], trials=[0, 1, 2, 3, 4, 5, 7], offset=1032),
}

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

# Structured C, not a transcription of emitted basic-block order. Byte helpers
# avoid alignment/strict-aliasing assumptions even when RegBytes is Guest data.
C_SPEC = r'''
#include <dragon/lifters/llvm/Execution.h>
extern unsigned oracle_case;
static uint64_t rd(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void wr(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
static void bump(unsigned char*g,unsigned at){wr(g+at,rd(g+at)+UINT64_C(1));}
void view(dragon_guest_state_v2_t*s,dragon_guest_memory_v2_t*m,dragon_exec_context_v2_t*c,dragon_exec_exit_v2_t*e){
 (void)c;unsigned char*g=m->data,*r=s->regs_bytes;uint64_t raw=rd(g),history=rd(r+48),result=history;
 uint64_t reason=1,kind=3,source=4608,next=0xfeed,valid=1,ret=0xfeed,detail=0;
 if(oracle_case==0){
  uint64_t state=0,iteration=0,trips=75+(raw&3);
  for(;;){
   wr(g+1104,history);wr(g+1112,state);wr(g+1120,iteration);bump(g,1024);
   if(iteration>=trips){result=history;break;}
   if(m->size<36872){reason=5;kind=0;source=next=4353;valid=ret=0;detail=3;result=history;break;}
   wr(g+36864,(history^raw)+state);uint64_t fresh=rd(r+48);wr(g+1128,fresh);bump(g,1032);
   uint64_t advanced=history+UINT64_C(3);
   if(advanced>rd(g+24)){reason=6;kind=0;source=next=4354;valid=ret=0;result=history;break;}
   if(state>=70){reason=4;kind=1;source=4350;next=state;ret=0;detail=3;result=history;break;}
   state=(state+1)%70;history=advanced;++iteration;
  }
 }else{
  uint64_t state=rd(g+32)?((raw&1)+2):0;
  /* The source's pure bounds self-edge is never taken by these real inputs. */
  if(state>=4)return;
  wr(g+1104,history);wr(g+1112,state);wr(g+1120,0);bump(g,1024);
  if(m->size<36872){reason=5;kind=0;source=next=4353;valid=ret=0;detail=3;result=history;}
  else{
   wr(g+36864,state);uint64_t fresh=rd(r+48);wr(g+1128,fresh);bump(g,1032);
   if(state>rd(g+24)){reason=6;kind=0;source=next=4354;valid=ret=0;result=history;}
   else if(state==2)result=history^fresh;
   else if(state==3)result=history+fresh;
   else if(state==0)result=history^raw;
   else{reason=4;kind=1;source=4350;next=state;ret=0;detail=3;result=history;}
  }
 }
 bump(g,1040);e->reason=reason;e->transfer_kind=kind;e->source_pc=source;e->next_pc=next;
 e->next_pc_valid=valid;e->return_pc=ret;e->detail=detail;
 wr(r,result^raw^rd(g+1032));wr(r+128,next);wr(r+143,result+rd(r+48));
}
'''


def initial(trial):
    if not 0 <= trial < 8:
        raise ValueError('trial outside fixed domain')
    a = bytearray(hashlib.shake_256(('batch-corridor/20261115/'+str(trial)).encode()).digest(FRAME))
    def w(at, value): a[at:at+8] = (value & M64).to_bytes(8, 'little')
    alias = trial in (1, 2, 4, 6, 7)
    w(0, WORDS[trial]); w(8, int(alias)); w(16, 8191 if trial == 6 else 65536)
    w(24, 0 if trial == 5 else M64); w(32, 0 if trial == 7 else 1)
    regs = ALIAS_REGS if alias else REGS
    w(regs+32, 49152); w(regs+48, WORDS[(trial+2)%8]); w(regs+128, 4096)
    for off in (1024, 1032, 1040): w(off, M64-trial%3)
    return bytes(a)


def specification(source, name):
    if len(source) != FRAME or name not in NAMES:
        raise ValueError('wrong arena or fixture')
    a = bytearray(source)
    def rd(at): return int.from_bytes(a[at:at+8], 'little')
    def wr(at, value): a[at:at+8] = (value & M64).to_bytes(8, 'little')
    def bump(at): wr(at, rd(at)+1)
    raw, regs, size = rd(0), ALIAS_REGS if rd(8)&1 else REGS, rd(16)
    history, result = rd(regs+48), rd(regs+48)
    fields = [1, 3, 4608, 0xfeed, 1, 0xfeed, 0]
    coverage = dict(alias=regs==ALIAS_REGS, headers=0, effects=0, dispatches=0,
                    states=[], size_error=False, limit_error=False, noncandidate=False)
    seen = set()
    def size_error():
        fields[:] = [5, 0, 4353, 4353, 0, 0, 3]; coverage['size_error'] = True
    def limit_error():
        fields[:] = [6, 0, 4354, 4354, 0, 0, 0]; coverage['limit_error'] = True
    def observe(state, iteration):
        wr(1104, history); wr(1112, state); wr(1120, iteration); bump(1024)
        coverage['headers'] += 1
    def effect(value):
        wr(36864, value); wr(1128, rd(regs+48)); bump(1032); coverage['effects'] += 1
    if name == NAMES[0]:
        state, iteration, trips = 0, 0, 75+(raw&3)
        while True:
            observe(state, iteration)
            if iteration >= trips: result = history; break
            if size < 36872: size_error(); result = history; break
            effect((history ^ raw)+state)
            advanced = (history+3)&M64
            if advanced > rd(24): limit_error(); result = history; break
            coverage['dispatches'] += 1; seen.add(state)
            if state >= 70:
                fields[:] = [4, 1, 4350, state, 1, 0, 3]; result = history; break
            state = (state+1)%70; history = advanced; iteration += 1
    else:
        state = (raw&1)+2 if rd(32) else 0
        if state >= 4: raise ValueError('fixed domain unexpectedly enters the original bounds self loop')
        observe(state, 0); coverage['noncandidate'] = not bool(rd(32))
        if size < 36872: size_error()
        else:
            effect(state); fresh = rd(regs+48)
            if state > rd(24): limit_error()
            else:
                coverage['dispatches'] += 1; seen.add(state)
                if state == 2: result = history ^ fresh
                elif state == 3: result = history + fresh
                elif state == 0: result = history ^ raw
                else: fields[:] = [4, 1, 4350, state, 1, 0, 3]
    bump(1040)
    for n, value in enumerate(fields): wr(EXIT+8*n, value)
    wr(regs, result ^ raw ^ rd(1032)); wr(regs+128, fields[3]); wr(regs+143, result+rd(regs+48))
    wr(RETURN, rd(regs)); coverage['states'] = sorted(seen); coverage['exit_reason'] = fields[0]
    return bytes(a), coverage


GUEST_OFFSETS = (0, 8, 24, 32, 1024, 1032, 1040, 1104, 1112, 1120, 1128, 36864)


def prefix():
    return ('target datalayout = "e-p:64:64"\n'
            'define void @view(ptr %s,ptr %m,ptr %c,ptr %e){\nentry:\n'
            ' %regs.addr=getelementptr i8,ptr %s,i64 24\n %regs=load ptr,ptr %regs.addr,align 1\n'
            ' %g=load ptr,ptr %m,align 1\n %size.addr=getelementptr i8,ptr %m,i64 8\n'+
            ''.join(f' %g{at}=getelementptr i8,ptr %g,i64 {at}\n' for at in GUEST_OFFSETS)+
            ''.join(f' %r{at}=getelementptr i8,ptr %regs,i64 {at}\n' for at in (48, 128, 143))+
            ' %raw=load i64,ptr %g0,align 1\n %initial=load i64,ptr %r48,align 1\n')


def bump(at, name):
    return (f' %{name}.old=load i64,ptr %g{at},align 1\n %{name}.new=add i64 %{name}.old,1\n'
            f' store i64 %{name}.new,ptr %g{at},align 1\n')


def epilogue(incomings):
    # Each incoming is (block, result, reason,kind,source,next,valid,ret,detail).
    ir = 'finish:\n'
    for field, at in zip(('result', 'reason', 'kind', 'source', 'next', 'valid', 'ret', 'detail'), range(1, 9)):
        ir += f' %{field}=phi i64 '+','.join(f'[{row[at]},%{row[0]}]' for row in incomings)+'\n'
    ir += bump(1040, 'finish.count')
    for at, value in enumerate(('reason', 'kind', 'source', 'next', 'valid', 'ret', 'detail')):
        ir += f' %e{at}=getelementptr i8,ptr %e,i64 {at*8}\n store i64 %{value},ptr %e{at},align 1\n'
    return (ir+' %mix=xor i64 %result,%raw\n %effects=load i64,ptr %g1032,align 1\n'
            ' %rax=xor i64 %mix,%effects\n store i64 %rax,ptr %regs,align 1\n'
            ' store i64 %next,ptr %r128,align 1\n %fresh.final=load i64,ptr %r48,align 1\n'
            ' %vector=add i64 %result,%fresh.final\n store i64 %vector,ptr %r143,align 1\n ret void\n}\n')


def seventy_state():
    seeds = [(f'seed{n}', (n+1)%70) for n in range(70)]
    # seed0 has two distinct actual successor slots to the same original head.
    slots = [item for item in seeds for _ in range(2 if item[0] == 'seed0' else 1)]
    ir = prefix()+' %trip.bits=and i64 %raw,3\n %trips=add i64 %trip.bits,75\n'
    ir += ' %route.bit=and i64 %raw,1\n %route=icmp ne i64 %route.bit,0\n br label %head\nhead:\n'
    ir += ' %state=phi i32 [0,%entry]'+''.join(f',[{state},%{seed}]' for seed,state in slots)+'\n'
    ir += ' %history=phi i64 [%initial,%entry]'+''.join(f',[%advanced,%{seed}]' for seed,_ in slots)+'\n'
    ir += ' %iteration=phi i64 [0,%entry]'+''.join(f',[%stepped,%{seed}]' for seed,_ in slots)+'\n'
    ir += (' %advanced=add i64 %history,3\n %stepped=add i64 %iteration,1\n'
           ' store i64 %history,ptr %g1104,align 1\n %state64=zext i32 %state to i64\n'
           ' store i64 %state64,ptr %g1112,align 1\n store i64 %iteration,ptr %g1120,align 1\n')
    ir += bump(1024, 'header.count')+' %done=icmp uge i64 %iteration,%trips\n br i1 %done,label %success,label %sizecheck\n'
    ir += ('sizecheck:\n %logical.size=load i64,ptr %size.addr,align 1\n'
           ' %sized=icmp uge i64 %logical.size,36872\n br i1 %sized,label %effect,label %sizeerror\n'
           'effect:\n %mixed=xor i64 %history,%raw\n %output=add i64 %mixed,%state64\n'
           ' store i64 %output,ptr %g36864,align 1\n %fresh=load i64,ptr %r48,align 1\n'
           ' store i64 %fresh,ptr %g1128,align 1\n')
    ir += bump(1032, 'body.count')+' %limit=load i64,ptr %g24,align 1\n %limited=icmp ule i64 %advanced,%limit\n'
    ir += ' br i1 %limited,label %dispatch,label %limiterror\ndispatch:\n switch i32 %state,label %stateerror ['
    ir += ''.join(f'i32 {n},label %seed{n} ' for n in range(70))+']\n'
    for seed,_ in seeds:
        ir += seed+(': br i1 %route,label %head,label %head\n' if seed == 'seed0' else ': br label %head\n')
    ir += 'success: br label %finish\nsizeerror: br label %finish\n'
    ir += 'limiterror:\n %past=phi i64 [%history,%effect]\n br label %finish\nstateerror: br label %finish\n'
    rows = [('success','%history',1,3,4608,65261,1,65261,0),
            ('sizeerror','%history',5,0,4353,4353,0,0,3),
            ('limiterror','%past',6,0,4354,4354,0,0,0),
            ('stateerror','%history',4,1,4350,'%state64',1,0,3)]
    candidates = [dict(source=seed,successor_slot=slot,path=['head','sizecheck','effect','dispatch'])
                  for seed,_ in reversed(seeds) for slot in reversed(range(2 if seed=='seed0' else 1))]
    return ir+epilogue(rows), candidates


def two_state():
    ir = prefix()+(' %take.raw=load i64,ptr %g32,align 1\n %take=icmp ne i64 %take.raw,0\n'
                   ' br i1 %take,label %seed,label %other\nseed:\n %bit=and i64 %raw,1\n'
                   ' %choice=add nsw i64 %bit,2\n br label %head\nother: br label %head\nhead:\n'
                   ' %state=phi i64 [%choice,%seed],[0,%other],[%state,%head]\n'
                   ' %bounded=icmp ult i64 %state,4\n br i1 %bounded,label %sizecheck,label %head\nsizecheck:\n'
                   ' store i64 %initial,ptr %g1104,align 1\n store i64 %state,ptr %g1112,align 1\n'
                   ' store i64 0,ptr %g1120,align 1\n')
    ir += bump(1024, 'header.count')+(' %logical.size=load i64,ptr %size.addr,align 1\n'
         ' %sized=icmp uge i64 %logical.size,36872\n br i1 %sized,label %effect,label %sizeerror\neffect:\n'
         ' store i64 %state,ptr %g36864,align 1\n %fresh=load i64,ptr %r48,align 1\n'
         ' store i64 %fresh,ptr %g1128,align 1\n')
    ir += bump(1032, 'body.count')+(' %limit=load i64,ptr %g24,align 1\n %limited=icmp ule i64 %state,%limit\n'
         ' br i1 %limited,label %dispatch,label %limiterror\ndispatch:\n'
         ' switch i64 %state,label %stateerror [i64 0,label %zero i64 2,label %two i64 3,label %three]\n'
         'zero:\n %zero.result=xor i64 %initial,%raw\n br label %finish\n'
         'two:\n %two.result=xor i64 %initial,%fresh\n br label %finish\n'
         'three:\n %three.result=add i64 %initial,%fresh\n br label %finish\n'
         'sizeerror: br label %finish\nlimiterror: br label %finish\nstateerror: br label %finish\n')
    rows = [(b,v,1,3,4608,65261,1,65261,0) for b,v in
            [('zero','%zero.result'),('two','%two.result'),('three','%three.result')]]
    rows += [('sizeerror','%initial',5,0,4353,4353,0,0,3),
             ('limiterror','%initial',6,0,4354,4354,0,0,0),
             ('stateerror','%initial',4,1,4350,'%state',1,0,3)]
    return ir+epilogue(rows), [dict(source='seed',successor_slot=0,path=['head','sizecheck','effect','dispatch'])]


def sources():
    return dict(zip(NAMES, (seventy_state(), two_state())))


def emit(out):
    out.mkdir(parents=True, exist_ok=False)
    generated = sources()
    for name, (ir, candidates) in generated.items():
        (out/(name+'.ll')).write_text(ir)
        (out/(name+'.candidates.json')).write_text(json.dumps(dict(function='view',candidates=candidates),indent=2)+'\n')
    ir = generated[NAMES[0]][0]
    replacements = {
        'wrong-history': ('store i64 %history,ptr %g1104,align 1','store i64 %advanced,ptr %g1104,align 1'),
        'skip-size-error': ('%sized=icmp uge i64 %logical.size,36872','%sized=icmp uge i64 %logical.size,0'),
        'repeat-effect': ('%body.count.new=add i64 %body.count.old,1','%body.count.new=add i64 %body.count.old,2'),
    }
    for name,(old,new) in replacements.items():
        if ir.count(old)!=1: raise ValueError(('fault sentinel not unique',name))
        (out/(name+'.fault.ll')).write_text(ir.replace(old,new))
    (out/'wrapper.c').write_text(WRAPPER); (out/'reference.c').write_text(C_SPEC)
    for t in range(8): (out/('input-'+str(t)+'.bin')).write_bytes(initial(t))
    files = {p.name:dict(size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
             for p in sorted(out.iterdir())}
    plan = dict(schema='dispatch-corridor-batch-source-v1', source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                frame=FRAME, names=list(NAMES), trials=8, cases=[dict(name=n,candidates=len(v[1])) for n,v in generated.items()],
                faults=FAULTS, controls=list(CONTROLS), files=files,
                scope='new independent C/Python/LLVM domain; no product, native or emulator executions')
    (out/'PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--out',required=True,type=Path)
    emit(parser.parse_args().out)
