"""Independent finite temporal-counterexample reference; not derived from IR."""
import hashlib
from protocol import FRAME,M64,C_COMMON,WRAPPER,CALLEE,CONTROLS
VALUES=(0,1,7,1<<32,(1<<63)|7,0xffffffff,8,M64,0,1,7,0,0,2,3,5)
C_SPEC=C_COMMON+r'''
extern void ordinary(State*,Memory*,Context*,Exit*);
void view(State*s,Memory*m,Context*c,Exit*e){unsigned char*g=m->data,*r=s->bytes;
 uint64_t sp=rd(r+32),out=rd(r+48),pc=rd(r+128);if(pc!=4096)return;
 if(m->size<65536||!g||sp!=49152||out!=32768||rd(g+8192)!=4352||rd(g+8200)!=4608){ordinary(s,m,c,e);return;}
 wr(g+4096,out);if((uint32_t)rd(g+2048)>=8){wr((unsigned char*)e,5);return;}
 uint64_t target=rd(g+8192);if(target!=4352){wr((unsigned char*)e,target);return;}
 for(unsigned iteration=0;;++iteration){uint64_t fresh=rd(g+2048);wr(g+1552,fresh);
  wr(g+2048,iteration==0?29:0);uint64_t current=rd(g+2048);wr(g+1560,current);
  if((uint32_t)current>=8)continue;
  uint32_t offset=(uint32_t)fresh*UINT32_C(3);w32(g+4008+(uint64_t)offset,8192);w32(g+rd(g+4096),123);break;
 }
 target=rd(g+8200);if(target!=4608)wr((unsigned char*)e,target);
}
'''
def initial(t):
 if not 0<=t<16:raise ValueError('domain')
 a=bytearray(hashlib.shake_256(('temporal-real-counterexample/20261116/'+str(t)).encode()).digest(FRAME))
 def w(at,v):a[at:at+8]=(v&M64).to_bytes(8,'little')
 alias=t in (2,4,9,14);regs=0x8fd0 if alias else 65536
 w(8,int(alias));w(16,65535 if t==11 else 65536);w(2048,VALUES[t]);w(8192,0xdead if t==9 else 4352);w(8200,4608)
 w(regs+32,49153 if t==8 else 49152);w(regs+48,32769 if t==10 else 32768);w(regs+128,4097 if t==12 else 4096)
 return bytes(a)
def specification(source):
 a=bytearray(source)
 def rd(at):return int.from_bytes(a[at:at+8],'little')
 def wr(at,v,n=8):a[at:at+n]=(v%(1<<(8*n))).to_bytes(n,'little')
 regs=0x8fd0 if rd(8)&1 else 65536;cov=dict(accepted=False,fallback=False,unsupported=False,precondition_exit=False,alias=regs==0x8fd0)
 if rd(regs+128)!=4096:cov['unsupported']=True
 elif rd(16)<65536 or rd(regs+32)!=49152 or rd(regs+48)!=32768 or rd(8192)!=4352 or rd(8200)!=4608:
  cov['fallback']=True;wr(1200,rd(1200)+1);wr(regs,rd(regs)^0x13579bdf02468ace);wr(regs+16,rd(regs+16)+0x123)
  for i,v in enumerate((5,0,4096,0xf00d,1,0xbeef,7)):wr(66048+8*i,v)
 else:
  wr(4096,rd(regs+48))
  if rd(2048)%(1<<32)>=8:cov['precondition_exit']=True;wr(66048,5)
  else:
   cov['accepted']=True
   old=rd(2048);wr(1552,old);wr(2048,29);wr(1560,29)
   fresh=rd(2048);wr(1552,fresh);wr(2048,0);wr(1560,0)
   # Two iterations are independent of the IR's PHI representation.
   index=fresh%(1<<32);stride=(index+index+index)%(1<<32)
   wr(4008+stride,8192,4);pointer=rd(4096);assert pointer==32
   wr(pointer,123,4)
 wr(66104,rd(regs));return bytes(a),cov
