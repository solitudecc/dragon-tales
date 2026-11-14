"""Emit-only independent register-snapshot / physical Guest-Reg alias oracle.
No product, compiler, LLVM parser or native execution is imported or invoked.
"""
import argparse
import hashlib
import json
from pathlib import Path

FRAME = 66112
GUEST = 65536
REGS = 65536
ALIAS_REGS = 0x8fd0
EXIT = 66048
RETURN = 66104
M64 = (1 << 64) - 1
NAMES = ('snapshot', 'fresh-load', 'unknown-call')
WORDS = (0, 1, M64, 1 << 63, (1 << 63) - 1, 0xaaaaaaaaaaaaaaaa, 0x5555555555555555, 0xffffffff)
CONTROLS = (0, 1024, 1032, 1040, 1048, 1056, 8192, 8200, 32768, 36864,
            ALIAS_REGS, ALIAS_REGS + 143, REGS, REGS + 48, REGS + 143, REGS + 511,
            EXIT, EXIT + 8, EXIT + 16, EXIT + 24, EXIT + 32, EXIT + 40, EXIT + 48, RETURN, FRAME - 1)
FAULTS = (('snapshot-as-fresh', 'snapshot'), ('fresh-as-snapshot', 'fresh-load'), ('skip-alias-write', 'snapshot'))

C_TYPES = r'''
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t abi,arch,size; unsigned char *bytes; } State;
typedef struct { unsigned char *data; uint64_t size; } Memory;
typedef struct { uint64_t reason,kind,source,next,valid,ret,detail; } Exit;
typedef struct { void *entries; uint64_t count,budget; void *user; } Context;
_Static_assert(sizeof(State)==32 && sizeof(Memory)==16 && sizeof(Exit)==56 && sizeof(Context)==32,"ABI");
_Static_assert(offsetof(State,bytes)==24,"register descriptor offset");
static uint64_t rd(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void wr(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
static void bump(unsigned char*p,unsigned at){wr(p+at,rd(p+at)+1);}
'''
WRAPPER = C_TYPES + r'''
#include <stdio.h>
#include <stdlib.h>
unsigned oracle_case;
extern void view(State*,Memory*,Context*,Exit*);
void opaque_effect(unsigned char*g){bump(g,1048);wr(g+8200,UINT64_C(0xbad0000000000001));}
int main(int argc,char**argv){
 _Alignas(16) unsigned char frame[66112];size_t n;if(argc!=2)return 4;
 oracle_case=(unsigned)strtoul(argv[1],0,10);if(oracle_case>=3)return 4;
 while((n=fread(frame,1,sizeof frame,stdin))!=0){
  if(n!=sizeof frame)return 2;
  unsigned at=(rd(frame+8)&1)?0x8fd0:65536;uint64_t size=rd(frame+16);
  if(size>65536 || size<2048)return 4;
  State s={2,1,512,frame+at};Memory m={frame,size};Context c={0,0,0,0};
  view(&s,&m,&c,(Exit*)(void*)(frame+66048));
  if(s.abi!=2||s.arch!=1||s.size!=512||s.bytes!=frame+at||m.data!=frame||m.size!=size||
     c.entries||c.count||c.budget||c.user)return 5;
  wr(frame+66104,rd(frame+at));
  if(fwrite(frame,1,sizeof frame,stdout)!=sizeof frame)return 3;
 }
 return ferror(stdin)||fflush(stdout)!=0;
}
'''
# Independent structured C, no LLVM text or product result is read.
C_SPEC = C_TYPES + r'''
extern unsigned oracle_case;
void view(State*s,Memory*m,Context*c,Exit*e){
 (void)c;unsigned char*g=m->data,*r=s->bytes;
 uint64_t sp=rd(r+32),pc=rd(r+128),old=rd(r+48),x=rd(g),size=m->size;
 unsigned reason=1,kind=3,detail=0;uint64_t source=4608,next=0xfeed,valid=1,ret=0xfeed;
 if(pc!=4096){reason=5;kind=0;source=pc;next=pc;valid=0;ret=0;detail=5;goto finish;}
 if(size<8208 || sp!=49152 || old!=32768 || rd(g+8192)!=4352 || rd(g+8200)!=4608)bump(g,1032);
 uint64_t cell=sp-48;
 if(size<36872 || size<4 || cell>size-4){reason=5;kind=0;source=4097;next=4097;valid=0;ret=0;detail=3;goto finish;}
 for(unsigned j=0;j<4;++j)g[cell+j]=0;
 wr(g+36864,8192);
 for(unsigned iteration=0;;++iteration){
  bump(g,1024);
  if(iteration==3)break;
  uint32_t index=0;for(unsigned j=0;j<4;++j)index|=(uint32_t)g[cell+j]<<(8*j);
  if(index>1){reason=5;kind=0;source=4350;next=4350;valid=0;ret=0;detail=3;goto finish;}
  bump(g,1056);uint64_t target=rd(g+8192+8*(uint64_t)index);
  if(target==4352){
   bump(g,1040);uint64_t output=oracle_case==1?rd(r+48):old;
   if(size<8 || output>size-8){reason=5;kind=0;source=4353;next=4353;valid=0;ret=0;detail=3;goto finish;}
   wr(g+output,(x^UINT64_C(0x9e3779b97f4a7c15))+iteration);
   if(oracle_case==2){bump(g,1048);wr(g+8200,UINT64_C(0xbad0000000000001));}
   for(unsigned j=0;j<4;++j)g[cell+j]=(unsigned char)(j==0);
  }else if(target==4608){for(unsigned j=0;j<4;++j)g[cell+j]=0;}
  else {reason=4;kind=1;source=4350;next=target;valid=1;ret=0;detail=3;goto finish;}
 }
finish:
 wr(g+1064,rd(g+1064)+1);
 e->reason=reason;e->kind=kind;e->source=source;e->next=next;e->valid=valid;e->ret=ret;e->detail=detail;
 wr(r,x^((uint64_t)reason<<56)^rd(g+1056));wr(r+128,next);
}
'''


def initial(trial):
    if not 0 <= trial < 8:
        raise ValueError('trial outside frozen domain')
    arena = bytearray(hashlib.shake_256(('register-snapshot/20261115/'+str(trial)).encode()).digest(FRAME))
    def w(at, value): arena[at:at+8] = (value & M64).to_bytes(8, 'little')
    alias = trial in (1, 2, 4, 6, 7)
    w(0, WORDS[trial]); w(8, int(alias)); w(16, 8191 if trial == 7 else 65536)
    w(8192, 0xdeadbeef if trial == 6 else 4352); w(8200, 4608)
    w(36864, 0xcafebabedead0000)
    at = ALIAS_REGS if alias else REGS
    w(at+32, 49160 if trial == 4 else 49152)
    w(at+48, M64 if trial == 5 else 32768); w(at+128, 4096)
    for off in (1024, 1032, 1040, 1048, 1056, 1064): w(off, M64-trial%3)
    return bytes(arena)


def specification(source, name):
    if len(source) != FRAME or name not in NAMES:
        raise ValueError('wrong arena or fixture')
    a = bytearray(source)
    def r(at, n=8): return int.from_bytes(a[at:at+n], 'little')
    def w(at, value, n=8): a[at:at+n] = (value & ((1 << (n*8))-1)).to_bytes(n, 'little')
    def bump(at): w(at, r(at)+1)
    regs = ALIAS_REGS if r(8)&1 else REGS
    sp, pc, old, raw, size = r(regs+32), r(regs+128), r(regs+48), r(0), r(16)
    coverage = dict(alias=regs==ALIAS_REGS, regs_at=regs, guard_hit=False, fallback=False,
                    dispatches=0, headers=0, outputs=0, calls=0)
    fields = [1, 3, 4608, 0xfeed, 1, 0xfeed, 0]
    def finish():
        bump(1064)
        for i, value in enumerate(fields): w(EXIT+8*i, value)
        w(regs, raw ^ (fields[0] << 56) ^ r(1056)); w(regs+128, fields[3])
        w(RETURN, r(regs)); coverage['exit_reason'] = fields[0]
        return bytes(a), coverage
    def error(pc):
        fields[:] = [5, 0, pc, pc, 0, 0, 3]
        return finish()
    if pc != 4096:
        fields[:] = [5, 0, pc, pc, 0, 0, 5]
        return finish()
    coverage['guard_hit'] = size >= 8208 and sp == 49152 and old == 32768 and r(8192) == 4352 and r(8200) == 4608
    if not coverage['guard_hit']:
        coverage['fallback'] = True; bump(1032)
    cell = (sp-48) & M64
    if size < 36872 or size < 4 or cell > size-4:
        return error(4097)
    w(cell, 0, 4); w(36864, 8192)
    for iteration in range(4):
        coverage['headers'] += 1; bump(1024)
        if iteration == 3: break
        index = r(cell, 4)
        if index > 1: return error(4350)
        bump(1056); coverage['dispatches'] += 1
        target = r(8192+8*index)
        if target == 4352:
            bump(1040)
            output = r(regs+48) if name == 'fresh-load' else old
            if size < 8 or output > size-8: return error(4353)
            w(output, ((raw ^ 0x9e3779b97f4a7c15)+iteration) & M64); coverage['outputs'] += 1
            if name == 'unknown-call':
                bump(1048); w(8200, 0xbad0000000000001); coverage['calls'] += 1
            w(cell, 1, 4)
        elif target == 4608: w(cell, 0, 4)
        else:
            fields[:] = [4, 1, 4350, target, 1, 0, 3]
            return finish()
    return finish()


def bump_ir(at, label):
    return f'  %{label}.p = getelementptr i8, ptr %guest, i64 {at}\n  %{label}.v = load i64, ptr %{label}.p, align 1\n  %{label}.n = add i64 %{label}.v, 1\n  store i64 %{label}.n, ptr %{label}.p, align 1\n'


def finish_ir(reason, kind, source, next_pc, valid, ret, detail):
    vals = [reason, kind, source, next_pc, valid, ret, detail]
    text = bump_ir(1064, 'finish.count')
    for i, v in enumerate(vals):
        text += f'  %exit.{i} = getelementptr i8, ptr %exit, i64 {i*8}\n  store i64 {v}, ptr %exit.{i}, align 1\n'
    text += f'  %return.count.p = getelementptr i8, ptr %guest, i64 1056\n  %return.count = load i64, ptr %return.count.p, align 1\n  %return.a = xor i64 %raw, {reason << 56}\n  %return.b = xor i64 %return.a, %return.count\n  store i64 %return.b, ptr %regs, align 1\n  store i64 {next_pc}, ptr %pcaddr, align 1\n  ret void\n'
    return text


def function(name, fixture, guarded, fault=None):
    text = f'define void @{name}(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {{\nentry:\n'
    text += '''  %slot = alloca i64
  %pcslot = alloca i64
  %regdesc = getelementptr i8, ptr %state, i64 24
  %regs = load ptr, ptr %regdesc, align 1
  %guest = load ptr, ptr %memory, align 1
  %spaddr = getelementptr i8, ptr %regs, i64 32
  %initialsp = load i64, ptr %spaddr, align 1
  store i64 %initialsp, ptr %slot
  %output.addr = getelementptr i8, ptr %regs, i64 48
  %initial.output = load i64, ptr %output.addr, align 1
  %pcaddr = getelementptr i8, ptr %regs, i64 128
  %initialpc = load i64, ptr %pcaddr, align 1
  store i64 %initialpc, ptr %pcslot
  %pc = load i64, ptr %pcslot
'''
    # A guard miss calls a separate complete ordinary implementation. Its entry
    # records the call, without changing fallback's required call+ret shape.
    if not guarded: text += bump_ir(1032, 'ordinary.count')
    text += '  switch i64 %pc, label %unsupported [ i64 4096, label %'+('bounds' if guarded else 'accepted')+' ]\n'
    if guarded:
        text += '''bounds:
  %sizeaddr.guard = getelementptr i8, ptr %memory, i64 8
  %size.guard = load i64, ptr %sizeaddr.guard, align 1
  %large = icmp uge i64 %size.guard, 8208
  %nonnull = icmp ne ptr %guest, null
  %valid = and i1 %large, %nonnull
  br i1 %valid, label %spcheck, label %fallback
spcheck:
  %spok = icmp eq i64 %initialsp, 49152
  br i1 %spok, label %outputcheck, label %fallback
outputcheck:
  %outputok = icmp eq i64 %initial.output, 32768
  br i1 %outputok, label %table0, label %fallback
table0:
  %t0p = getelementptr i8, ptr %guest, i64 8192
  %t0 = load i64, ptr %t0p, align 1
  %t0ok = icmp eq i64 %t0, 4352
  br i1 %t0ok, label %table1, label %fallback
table1:
  %t1p = getelementptr i8, ptr %guest, i64 8200
  %t1 = load i64, ptr %t1p, align 1
  %t1ok = icmp eq i64 %t1, 4608
  br i1 %t1ok, label %accepted, label %fallback
'''
    text += '''accepted:
  br label %prologue
prologue:
  %sp0 = load i64, ptr %slot
  %frame = sub i64 %sp0, 64
  store i64 %frame, ptr %slot
  %stateoff = add i64 %frame, 16
  %statep = getelementptr i8, ptr %guest, i64 %stateoff
  %sizeaddr = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizeaddr, align 1
  %has.alias = icmp uge i64 %size, 36872
  %has.state = icmp uge i64 %size, 4
  %last = sub i64 %size, 4
  %state.fits = icmp ule i64 %stateoff, %last
  %fits.a = and i1 %has.alias, %has.state
  %fits = and i1 %fits.a, %state.fits
  br i1 %fits, label %initialize, label %memory.error
initialize:
  store i32 0, ptr %statep, align 1
  %aliasp = getelementptr i8, ptr %guest, i64 36864
'''
    if not (guarded and fault == 'skip-alias-write'): text += '  store i64 8192, ptr %aliasp, align 1\n'
    text += '''  %raw = load i64, ptr %guest, align 1
  br label %controller
controller:
  %iteration = phi i32 [ 0, %initialize ], [ %next.a, %a.store ], [ %next.b, %b ]
'''+bump_ir(1024, 'header.count')+'''  %done = icmp eq i32 %iteration, 3
  br i1 %done, label %finished, label %state.check
state.check:
  %index = load i32, ptr %statep, align 1
  %inrange = icmp ule i32 %index, 1
  br i1 %inrange, label %read, label %state.error
read:
  %wide = zext i32 %index to i64
  %scaled = shl i64 %wide, 3
  %address = add i64 %scaled, 8192
  %targetp = getelementptr i8, ptr %guest, i64 %address
'''+bump_ir(1056, 'dispatch.count')+'''  %target = load i64, ptr %targetp, align 1
  switch i64 %target, label %unknown [ i64 4352, label %a i64 4608, label %b ]
a:
'''+bump_ir(1040, 'a.count')
    fresh = fixture == 'fresh-load'
    if guarded and fault == 'snapshot-as-fresh': fresh = True
    if guarded and fault == 'fresh-as-snapshot': fresh = False
    if fresh: text += '  %future.output = load i64, ptr %output.addr, align 1\n'
    output = '%future.output' if fresh else '%initial.output'
    text += f'''  %outputp = getelementptr i8, ptr %guest, i64 {output}
  %out.has = icmp uge i64 %size, 8
  %out.last = sub i64 %size, 8
  %out.fits = icmp ule i64 {output}, %out.last
  %out.valid = and i1 %out.has, %out.fits
  br i1 %out.valid, label %a.store, label %output.error
a.store:
  %mix = xor i64 %raw, 11400714819323198485
  %iteration.wide = zext i32 %iteration to i64
  %value = add i64 %mix, %iteration.wide
  store i64 %value, ptr %outputp, align 1
'''
    if fixture == 'unknown-call': text += '  call void @opaque_effect(ptr %guest)\n'
    text += '''  store i32 1, ptr %statep, align 1
  %next.a = add i32 %iteration, 1
  br label %controller
b:
  store i32 0, ptr %statep, align 1
  %next.b = add i32 %iteration, 1
  br label %controller
finished:
'''+finish_ir(1, 3, 4608, 65261, 1, 65261, 0)
    # raw is loaded before all terminal paths so entry/pre-prologue errors can
    # still report the same independent data word.
    text += 'memory.error:\n'+finish_ir(5, 0, 4097, 4097, 0, 0, 3)
    text += 'state.error:\n'+finish_ir(5, 0, 4350, 4350, 0, 0, 3)
    text += 'output.error:\n'+finish_ir(5, 0, 4353, 4353, 0, 0, 3)
    text += 'unknown:\n'+finish_ir(4, 1, 4350, '%target', 1, 0, 3)
    text += 'unsupported:\n'+finish_ir(5, 0, '%initialpc', '%initialpc', 0, 0, 5)
    if guarded: text += 'fallback:\n  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)\n  ret void\n'
    text += '}\n'
    # Finish blocks have function-local names. Give each its own generated SSA
    # prefix; LLVM values have function scope, not basic-block scope.
    lines = text.splitlines(); current = ''; result = []
    finish_blocks = {'finished', 'memory.error', 'state.error', 'output.error', 'unknown', 'unsupported'}
    for line in lines:
        if line.endswith(':'): current = line[:-1]
        if current in finish_blocks:
            for token in ('finish.count', 'exit.', 'return.count', 'return.a', 'return.b'):
                line = line.replace('%'+token, '%'+current+'.'+token)
        result.append(line)
    result = '\n'.join(result)+'\n'
    result = result.replace('  %raw = load i64, ptr %guest, align 1\n', '')
    result = result.replace('  %guest = load ptr, ptr %memory, align 1\n',
                            '  %guest = load ptr, ptr %memory, align 1\n  %raw = load i64, ptr %guest, align 1\n')
    return result


def llvm_source(fixture, fault=None):
    # The data word is captured once in entry; it is never a register fact.
    text = ('target datalayout = "e-p:64:64"\ndeclare void @opaque_effect(ptr)\n'+
            function('ordinary', fixture, False)+function('view', fixture, True, fault))
    return text


def emit(out):
    out.mkdir(parents=True, exist_ok=False)
    (out/'wrapper.c').write_text(WRAPPER); (out/'reference.c').write_text(C_SPEC)
    cases = []
    for name in NAMES:
        (out/(name+'.before.ll')).write_text(llvm_source(name))
        cases.append(dict(name=name, file=name+'.before.ll', expect_closed=name=='snapshot',
                          request=dict(view_symbol='view', table_offset=8192, targets=[4352, 4608])))
    faults = []
    for name, source in FAULTS:
        filename=source+'.controlled-'+name+'.ll';(out/filename).write_text(llvm_source(source, name))
        faults.append(dict(name=name, source=source, file=filename, specified_trials=[1, 2] if name!='skip-alias-write' else [0, 1, 2, 3]))
    for trial in range(8): (out/('input-'+str(trial)+'.bin')).write_bytes(initial(trial))
    for name in NAMES:
        for trial in range(8): (out/(name+'.expected-'+str(trial)+'.bin')).write_bytes(specification(initial(trial), name)[0])
    files={p.name:dict(size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(out.iterdir())}
    plan=dict(schema='register-context-native-emitted-v1',cases=cases,faults=faults,files=files,
              source_generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),frame_bytes=FRAME,
              source_fixtures=3,unique_module_inputs=24,unique_initial_arenas=8,inputs=8,
              full_domain='Whole physical 66112-byte arena; active RegBytes may alias Guest; inactive RegBytes also compared; Exit all7fields and guest return included. Descriptor fields validated unchanged by wrapper, no ASLR pointer normalization.',
              controls_offsets=list(CONTROLS),native_executions=0)
    (out/'PLAN.json').write_text(json.dumps(plan,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(emitted=str(out),plan_sha256=hashlib.sha256((out/'PLAN.json').read_bytes()).hexdigest(),native_executions=0)))


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--emit',type=Path,required=True);emit(ap.parse_args().emit)
