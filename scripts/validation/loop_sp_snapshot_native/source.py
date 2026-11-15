"""Finite exact NoFrame fixture plus independent byte-packed C/Python model."""
from pathlib import Path
import hashlib,json,re

FRAME=66112
CONTROLS=(0,32,64,80,96,112,200,256,264,2048,8704,66048,FRAME-1)
M64=(1<<64)-1
FIXTURE_SHA='4445ecfae163b3cf2e95b148296e4df06cc2a891b43df5e985476768b4fde483'
WORDS=(0,M64,M64,1,2,3,7,1<<63,0xaaaaaaaaaaaaaaaa)
LABELS=('accepted-zero','accepted-max','accepted-reg-guest-alias','bad-SP','bad-output','bad-table0','bad-table1','bad-PC','bad-size')
C_COMMON=r'''
#include <dragon/lifters/llvm/Execution.h>
static uint64_t rd(const unsigned char*p){uint64_t v=0;for(unsigned i=8;i;--i)v=v*256+p[i-1];return v;}
static void wr(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;++i){p[i]=(unsigned char)v;v>>=8;}}
static void w32(unsigned char*p,uint32_t v){for(unsigned i=0;i<4;++i){p[i]=(unsigned char)v;v>>=8;}}
'''
CALLEE=C_COMMON+r'''
void ordinary(dragon_guest_state_v2_t*s,dragon_guest_memory_v2_t*m,dragon_exec_context_v2_t*c,dragon_exec_exit_v2_t*e){
 (void)c;wr(m->data+1200,rd(m->data+1200)+UINT64_C(1));
 wr(s->regs_bytes,rd(s->regs_bytes)^UINT64_C(0x13579bdf02468ace));
 wr(s->regs_bytes+16,rd(s->regs_bytes+16)+UINT64_C(0x123));
 e->reason=5;e->transfer_kind=0;e->source_pc=4096;e->next_pc=0xf00d;e->next_pc_valid=1;e->return_pc=0xbeef;e->detail=7;
}
'''
C_SPEC=C_COMMON+r'''
extern void ordinary(dragon_guest_state_v2_t*,dragon_guest_memory_v2_t*,dragon_exec_context_v2_t*,dragon_exec_exit_v2_t*);
void view(dragon_guest_state_v2_t*s,dragon_guest_memory_v2_t*m,dragon_exec_context_v2_t*c,dragon_exec_exit_v2_t*e){
 unsigned char*g=m->data,*r=s->regs_bytes;
 uint64_t sp=rd(r+32),out=rd(r+48),pc=rd(r+128);
 if(pc!=4096)return;
 if(m->size<65536||!g||sp!=49152||out!=32768||rd(g+8192)!=4352||rd(g+8200)!=4608){ordinary(s,m,c,e);return;}
 wr(g+4096,out);wr(g+2048,22);uint64_t state=0;
 for(;;){
  while(state>=2)++state;
  uint64_t target=rd(g+8192+state*8);
  if(target==4352){
   wr(g+1536,state);uint64_t bounded=rd(g+128)&UINT64_C(7);wr(g+2048,bounded);
   uint64_t index=rd(g+2048);w32(g+4008+index*4,8192);uint64_t pointer=rd(g+4096);w32(g+pointer,123);state=1;
  }else if(target==4608){
   uint64_t index=rd(g+2048);w32(g+4008+index*4,8192);uint64_t pointer=rd(g+4096);w32(g+pointer,321);return;
  }else{wr((unsigned char*)e,target);return;}
 }
}
'''

def initial(t):
 if not 0<=t<9:raise ValueError('outside nine fixed frames')
 a=bytearray(hashlib.shake_256(('loop-SP/no-frame/20261115/'+str(t)).encode()).digest(FRAME))
 def w(at,v):a[at:at+8]=(v&M64).to_bytes(8,'little')
 w(8,int(t==2));w(16,65535 if t==8 else 65536);w(128,WORDS[t]);w(8192,0xdead if t==5 else 4352);w(8200,0xbeef if t==6 else 4608)
 regs=0x8fd0 if t==2 else 65536
 w(regs+32,49153 if t==3 else 49152);w(regs+48,32769 if t==4 else 32768);w(regs+128,4097 if t==7 else 4096)
 return bytes(a)

def specification(source):
 if len(source)!=FRAME:raise ValueError('wrong arena')
 a=bytearray(source)
 def rd(at):return int.from_bytes(a[at:at+8],'little')
 def wr(at,value,n=8):a[at:at+n]=(value&((1<<(8*n))-1)).to_bytes(n,'little')
 regs=0x8fd0 if rd(8)&1 else 65536;size=rd(16)
 sp,out,pc=rd(regs+32),rd(regs+48),rd(regs+128)
 coverage=dict(alias=regs==0x8fd0,accepted=False,fallback=False,unsupported=False,A=0,B=0)
 if pc!=4096:coverage['unsupported']=True
 elif size<65536 or sp!=49152 or out!=32768 or rd(8192)!=4352 or rd(8200)!=4608:
  coverage['fallback']=True;wr(1200,rd(1200)+1);wr(regs,rd(regs)^0x13579bdf02468ace);wr(regs+16,rd(regs+16)+0x123)
  for i,v in enumerate((5,0,4096,0xf00d,1,0xbeef,7)):wr(66048+i*8,v)
 else:
  coverage['accepted']=True;wr(4096,out);wr(2048,22);wr(1536,0)
  bounded=rd(128)&7;wr(2048,bounded);wr(4008+rd(2048)*4,8192,4);wr(rd(4096),123,4);coverage['A']=1
  wr(4008+rd(2048)*4,8192,4);wr(rd(4096),321,4);coverage['B']=1
 wr(66104,rd(regs));return bytes(a),coverage

def raw(fixture):
 raw=Path(fixture).read_bytes();assert hashlib.sha256(raw).hexdigest()==FIXTURE_SHA
 text=raw.decode().split('std::string text=R"IR(',1)[1].split(')IR";',1)[0]
 replacements={'INITIAL_STATE':'0','B_STATE':'0','; FIRST_CANDIDATE':'','; FIRST_READ':'','INDEX_MASK':'7','; INITIAL_INDEX':'store i64 22, ptr %indexcell, align 1','; MASK_STORE':'store i64 %bounded, ptr %indexcell, align 1','; EXTRA_EFFECT':'','; RESTORE_EFFECT':'','; AFTER_B_LOAD':'','%frame = sub i64 %sp0, 64':'%frame = add i64 %sp0, 0'}
 for old,new in replacements.items():assert old in text;text=text.replace(old,new)
 assert text.count('[ 0, %b.end ], ')==1;text=text.replace('[ 0, %b.end ], ','')
 assert text.count('b.end:\n  br label %head')==1;text=text.replace('b.end:\n  br label %head','b.end:\n  ret void')
 assert not re.search(r'INITIAL_|B_STATE|INDEX_MASK|; (FIRST_|EXTRA_|RESTORE_|AFTER_)',text)
 return text

def emit(out,fixture,wrapper_source):
 out=Path(out);out.mkdir()
 text=raw(fixture)
 (out/'before.ll').write_text(text);(out/'reference.c').write_text(C_SPEC);(out/'callee.c').write_text(CALLEE)
 (out/'wrapper.c').write_text(Path(wrapper_source).read_text().split("WRAPPER = r'''",1)[1].split("'''",1)[0])
 # Only controlled source mutations, never edits to a product output.
 assert text.count('store i32 321, ptr %output.b, align 1')==1
 (out/'fault-output.ll').write_text(text.replace('store i32 321, ptr %output.b, align 1','store i32 322, ptr %output.b, align 1'))
 assert text.count('  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)')==1
 (out/'fault-fallback.ll').write_text(text.replace('  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)',''))
 frames=[]
 for t,label in enumerate(LABELS):
  source=initial(t);wanted,coverage=specification(source)
  (out/f'input-{t}.bin').write_bytes(source);(out/f'expected-{t}.bin').write_bytes(wanted)
  frames.append(dict(trial=t,label=label,coverage=coverage))
 (out/'DOMAIN.json').write_text(json.dumps(frames,indent=2,sort_keys=True)+'\n')

if __name__=='__main__':
 import sys
 emit(*sys.argv[1:])
