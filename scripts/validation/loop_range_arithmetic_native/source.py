"""Emit-only new arithmetic range fixtures and independent byte-state models.
No compiler, SDK, Guest executor, or product output is imported/executed here.
"""
from pathlib import Path
import hashlib,json,re
FRAME=66112
M64=(1<<64)-1
CASES=('sparse32','wide64')
WORDS=(0,1,2,3,7,8,0x7fffffff,0x80000000,0xffffffff,1<<63,M64,0xaaaaaaaaaaaaaaaa,1,7,0,3)
CONTROLS=(0,128,256,264,1024,1080,1200,1536,2048,3072,4008,4036,4096,8192,8200,32768,0x8fd0,0x8fd0+143,65536,65536+32,65536+128,66048,66056,66064,66072,66080,66088,66096,66104,FRAME-1)
C_COMMON=r'''
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t abi,arch,size; unsigned char *bytes; } State;
typedef struct { unsigned char *data; uint64_t size; } Memory;
typedef struct { uint64_t reason,kind,source,next,valid,ret,detail; } Exit;
typedef struct { void *entries; uint64_t count,budget; void *user; } Context;
_Static_assert(sizeof(State)==32&&sizeof(Memory)==16&&sizeof(Exit)==56&&sizeof(Context)==32,"ABI");
static uint64_t rd(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void wr(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
static void w32(unsigned char*p,uint32_t v){for(unsigned i=0;i<4;++i){p[i]=(unsigned char)v;v>>=8;}}
'''
WRAPPER=C_COMMON+r'''
#include <stdio.h>
extern void view(State*,Memory*,Context*,Exit*);
int main(void){_Alignas(16) unsigned char a[66112];size_t n;
 while((n=fread(a,1,sizeof a,stdin))!=0){if(n!=sizeof a)return 2;
  unsigned at=(rd(a+8)&1)?0x8fd0:65536;uint64_t size=rd(a+16);if(size<2048||size>65536)return 4;
  State s={2,1,512,a+at};Memory m={a,size};Context c={0,0,0,0};view(&s,&m,&c,(Exit*)(void*)(a+66048));
  if(s.abi!=2||s.arch!=1||s.size!=512||s.bytes!=a+at||m.data!=a||m.size!=size||c.entries||c.count||c.budget||c.user)return 5;
  wr(a+66104,rd(a+at));if(fwrite(a,1,sizeof a,stdout)!=sizeof a)return 3;
 }return ferror(stdin)||fflush(stdout)!=0;}
'''
CALLEE=C_COMMON+r'''
void ordinary(State*s,Memory*m,Context*c,Exit*e){(void)c;wr(m->data+1200,rd(m->data+1200)+1);
 wr(s->bytes,rd(s->bytes)^UINT64_C(0x13579bdf02468ace));wr(s->bytes+16,rd(s->bytes+16)+0x123);
 e->reason=5;e->kind=0;e->source=4096;e->next=0xf00d;e->valid=1;e->ret=0xbeef;e->detail=7;}
'''
def c_spec(case):
 if case not in CASES:raise ValueError('case')
 return C_COMMON+r'''
extern void ordinary(State*,Memory*,Context*,Exit*);
void view(State*s,Memory*m,Context*c,Exit*e){unsigned char*g=m->data,*r=s->bytes;
 uint64_t sp=rd(r+32),out=rd(r+48),pc=rd(r+128);if(pc!=4096)return;
 if(m->size<65536||!g||sp!=49152||out!=32768||rd(g+8192)!=4352||rd(g+8200)!=4608){ordinary(s,m,c,e);return;}
 wr(g+4096,out);wr(g+1080,rd(g+1080)+1);wr(g+2048,22);uint64_t state=0;
 for(;;){while(state>=2)++state;wr(g+1024,rd(g+1024)+1);uint64_t target=rd(g+8192+state*8);
  if(target==4352){wr(g+1536,state);uint64_t input=rd(g+128);wr(g+2048,input&7);
   uint64_t index=rd(g+2048),offset=4008+index*4+EXTRA;
   w32(g+offset,8192);w32(g+rd(g+4096),123);OVERFLOW;state=1;
  }else if(target==4608){wr(g+2048,(rd(g+2048)+1)&7);uint64_t input=rd(g+128),index=rd(g+2048);
   uint64_t offset=4008+index*4+EXTRA;w32(g+offset,8192);w32(g+rd(g+4096),321);return;
  }else{wr((unsigned char*)e,target);return;}
 }
}
'''.replace('EXTRA','0' if case=='sparse32' else '(input&3)').replace('OVERFLOW','w32(g+3072,(uint32_t)input*UINT32_C(2147483649))' if case=='sparse32' else 'wr(g+3072,input*UINT64_C(9223372036854775809))')

def initial(t):
 if not 0<=t<16:raise ValueError('outside fixed domain')
 a=bytearray(hashlib.shake_256(('range-arithmetic/20261116/'+str(t)).encode()).digest(FRAME))
 def w(at,v):a[at:at+8]=(v&M64).to_bytes(8,'little')
 alias=t in (2,5,9,13);r=0x8fd0 if alias else 65536
 w(8,int(alias));w(16,65535 if t==15 else 65536);w(128,WORDS[t]);w(8192,0xdead if t==14 else 4352);w(8200,4608)
 w(r+32,49153 if t==12 else 49152);w(r+48,32769 if t==13 else 32768);w(r+128,4097 if t==11 else 4096)
 for at in (1024,1080,1200):w(at,M64-(t%3))
 return bytes(a)

def specification(source,case):
 if len(source)!=FRAME or case not in CASES:raise ValueError('domain')
 a=bytearray(source)
 def rd(at):return int.from_bytes(a[at:at+8],'little')
 def wr(at,value,n=8):a[at:at+n]=(value% (1<<(8*n))).to_bytes(n,'little')
 regs=0x8fd0 if rd(8)&1 else 65536;coverage=dict(accepted=False,fallback=False,unsupported=False,alias=regs==0x8fd0,A=0,B=0)
 if rd(regs+128)!=4096:coverage['unsupported']=True
 elif rd(16)<65536 or rd(regs+32)!=49152 or rd(regs+48)!=32768 or rd(8192)!=4352 or rd(8200)!=4608:
  coverage['fallback']=True;wr(1200,rd(1200)+1);wr(regs,rd(regs)^0x13579bdf02468ace);wr(regs+16,rd(regs+16)+0x123)
  for i,v in enumerate((5,0,4096,0xf00d,1,0xbeef,7)):wr(66048+8*i,v)
 else:
  coverage['accepted']=True;wr(4096,rd(regs+48));wr(1080,rd(1080)+1);wr(2048,22)
  for state in (0,1):
   wr(1024,rd(1024)+1);x=rd(128)
   if state==0:wr(1536,0);wr(2048,x%8)
   else:wr(2048,(rd(2048)+1)%8)
   index=rd(2048);stride=index+index+index+index;extra=0 if case=='sparse32' else x%4
   wr(4008+stride+extra,8192,4);wr(rd(4096),123 if state==0 else 321,4);coverage['AB'[state]]=1
   if state==0:
    bits=32 if case=='sparse32' else 64;v=x% (1<<bits)
    # Independent modular model: a multiply by 2^(W-1)+1 is shift+add.
    wr(3072,((v<<(bits-1))+v)%(1<<bits),bits//8)
 wr(66104,rd(regs));return bytes(a),coverage

def llvm_source(case,fixture):
 if case not in CASES:raise ValueError('case')
 text=Path(fixture).read_text().split('std::string text=R"IR(',1)[1].split(')IR";',1)[0]
 assert text.count('[ 0, %b.end ], ')==1;text=text.replace('[ 0, %b.end ], ','')
 text=text.replace('b.end:\n  br label %head','b.end:\n  ret void')
 text=text.replace('  %index.b = load i64, ptr %indexcell, align 1','  %old.index.b = load i64, ptr %indexcell, align 1\n  %next.index.b = add i64 %old.index.b, 1\n  %bounded.b = and i64 %next.index.b, 7\n  store i64 %bounded.b, ptr %indexcell, align 1\n  %index.b = load i64, ptr %indexcell, align 1')
 for suffix in ('a','b'):
  if case=='sparse32':
   code=f'''  %ax.{suffix} = trunc i64 %index.{suffix} to i32
  %ab.{suffix} = and i32 %ax.{suffix}, 4
  %ao.{suffix} = or i32 %ax.{suffix}, 4
  %af.{suffix} = xor i32 %ab.{suffix}, 4
  %ac.{suffix} = and i32 %ax.{suffix}, -5
  %am0.{suffix} = mul i32 %ab.{suffix}, %ao.{suffix}
  %am1.{suffix} = mul i32 %ac.{suffix}, %af.{suffix}
  %at.{suffix} = add i32 %am0.{suffix}, %am1.{suffix}
  %scaled.{suffix} = zext i32 %at.{suffix} to i64'''
  else:
   code=f'''  %tagp.{suffix} = getelementptr i8, ptr %guest, i64 128
  %tag.{suffix} = load i64, ptr %tagp.{suffix}, align 1
  %low.{suffix} = and i64 %tag.{suffix}, 3
  %shift.{suffix} = shl i64 %index.{suffix}, 2
  %join.{suffix} = or disjoint i64 %shift.{suffix}, %low.{suffix}
  %scaled.{suffix} = and i64 %join.{suffix}, 4294967295'''
  old=f'  %scaled.{suffix} = shl i64 %index.{suffix}, 2';assert text.count(old)==1;text=text.replace(old,code)
 def bump(name,at):return f'  %{name}.p = getelementptr i8, ptr %guest, i64 {at}\n  %{name}.old = load i64, ptr %{name}.p, align 1\n  %{name}.next = add i64 %{name}.old, 1\n  store i64 %{name}.next, ptr %{name}.p, align 1\n'
 text=text.replace('  %scale = shl i64 %statevalue, 3',bump('dispatch.counter',1024)+'  %scale = shl i64 %statevalue, 3')
 text=text.replace('  store i64 %snapshot, ptr %cell, align 1','  store i64 %snapshot, ptr %cell, align 1\n'+bump('seed.counter',1080))
 overflow=('  %overflow.input = trunc i64 %data to i32\n  %overflow.result = mul i32 %overflow.input, 2147483649\n  %overflow.p = getelementptr i8, ptr %guest, i64 3072\n  store i32 %overflow.result, ptr %overflow.p, align 1\n' if case=='sparse32' else '  %overflow.result = mul i64 %data, 9223372036854775809\n  %overflow.p = getelementptr i8, ptr %guest, i64 3072\n  store i64 %overflow.result, ptr %overflow.p, align 1\n')
 text=text.replace('  br label %a.end',overflow+'  br label %a.end')
 return text

def emit(out,fixture):
 out=Path(out);out.mkdir();(out/'wrapper.c').write_text(WRAPPER);(out/'callee.c').write_text(CALLEE)
 cases=[];faults=[]
 for case in CASES:
  text=llvm_source(case,fixture);(out/(case+'.before.ll')).write_text(text);(out/(case+'.reference.c')).write_text(c_spec(case));cases.append(dict(name=case,expect_closed=True,profile='Default' if case=='sparse32' else 'Expanded'))
  mutations={'address-plus-one':('%offset.a = add i64 %scaled.a, 4008','%offset.a = add i64 %scaled.a, 4009'),'skip-A-store':('  store i32 8192, ptr %work.a, align 1',''), 'double-seed':('%seed.counter.next = add i64 %seed.counter.old, 1','%seed.counter.next = add i64 %seed.counter.old, 2'),'old-for-fresh-B':('%index.b = load i64, ptr %indexcell, align 1','%index.b = add i64 %old.index.b, 0'),'drop-fallback':('  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)','')}
  for name,(old,new) in mutations.items():
   assert text.count(old)==1;p=out/(case+'.fault-'+name+'.ll');p.write_text(text.replace(old,new));faults.append(dict(case=case,name=name,file=p.name,specified_trials=list(range(12,16)) if name=='drop-fallback' else list(range(11))))
  for t in range(16):
   src=initial(t);want,coverage=specification(src,case);(out/f'input-{t}.bin').write_bytes(src);(out/f'{case}.expected-{t}.bin').write_bytes(want)
 (out/'DOMAIN.json').write_text(json.dumps([dict(case=case,trial=t,coverage=specification(initial(t),case)[1]) for case in CASES for t in range(16)],indent=2,sort_keys=True)+'\n')
 files=[dict(relative_path=str(p.relative_to(out)),size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(out.iterdir()) if p.is_file()]
 (out/'PLAN.json').write_text(json.dumps(dict(schema='loop-range-arithmetic-native-emit-v1',status='emitted-only-not-calibrated',cases=cases,faults=faults,source_fixtures=2,inputs=16,unique_module_inputs=32,arena_bytes=FRAME,controls=CONTROLS,files=files,fixture_sha256=hashlib.sha256(Path(fixture).read_bytes()).hexdigest(),source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),new_build_API_or_Guest_calls=0),indent=2,sort_keys=True)+'\n')
if __name__=='__main__':
 import sys
 emit(*sys.argv[1:])
