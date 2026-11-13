/* Independent unsigned-integer sequential C reference, no LLVM/dragon API. */
#include <stdint.h>
#include <string.h>
typedef unsigned __int128 U128;
extern uint64_t business_calls;
extern void prefix_unknown(unsigned char*,unsigned char*,unsigned char*);
extern void side_unknown(unsigned char*,unsigned char*,unsigned char*,uint64_t);
static uint64_t rd(unsigned char*p){uint64_t x;memcpy(&x,p,8);return x;}
static void wr(unsigned char*p,uint64_t x){memcpy(p,&x,8);}
static U128 loadword(unsigned char*p,unsigned w){U128 v=0;memcpy(&v,p,w/8);return v;}
static void storeword(unsigned char*p,unsigned w,U128 v){memcpy(p,&v,w/8);}
uint64_t reference(unsigned char*r,unsigned char*descriptor,unsigned char*exit,unsigned char*c,
                   unsigned width,unsigned bits,unsigned arithmetic,unsigned mode){
 unsigned char*g;memcpy(&g,descriptor,8);uint64_t size=rd(descriptor+8);
 uint64_t x=rd(r),history=rd(r+8),remaining=rd(r+40),iteration=0,acc=x,last=0;
 U128 old=loadword(r+64,width);prefix_unknown(r,g,c);
 uint64_t target=rd(c),replacement=rd(c+8),trigger=rd(c+16),state=0;
 unsigned kind=0,tag=0;
 for(unsigned visits=0;visits<100;visits++){
  U128 fresh=loadword(r+64,width);unsigned count=width-bits;
  if(mode==1||mode==2)count=(rd(r+80)&1)?width-1:(mode==2?0:width-2);
  U128 part=fresh>>count;
  if(arithmetic && ((fresh>>(width-1))&1)){
   /* Explicit sign fill avoids implementation-defined signed right shifts. */
   U128 high=(U128)-1;
   if(count)part|=high<<(width-count);
  }
  if(width<128)part&=(((U128)1<<width)-1);
  uint64_t value=(uint64_t)part;
  if(width==32 && arithmetic && (value&0x80000000ULL))value|=0xffffffff00000000ULL;
  if(mode==3)value+=2*(uint64_t)(old>>(width-1));
  state=value+42+(arithmetic?(1u<<(bits-1)):0);
  uint64_t ledger=rd(g+128),observe=history^state^ledger;
  wr(g+128,ledger+observe);
  if(remaining==0){kind=1;tag=1;break;}
  uint64_t running=acc+observe+rd(r+48)+iteration,nnext=remaining-1,inext=iteration+1;
  storeword(r+64,width,fresh^((U128)1<<(width-1)));wr(r+80,rd(r+80)^1);
  if(mode==6 && iteration==trigger){
   side_unknown(r,g,c,iteration);remaining=nnext;iteration=inext;history=observe;acc=running;continue;
  }
  if(state>=64){kind=5;tag=11;last=state;acc=running;remaining=nnext;iteration=inext;break;}
  uint64_t address=512+state*8;
  if(address+8>size){kind=5;tag=12;last=address;acc=running;remaining=nnext;iteration=inext;break;}
  if(mode==7)side_unknown(r,g,c,iteration);
  uint64_t stored=iteration>=trigger?replacement:target;
  if(mode==4)wr(g+address,stored);
  uint64_t loaded=rd(g+address);
  if(mode==5)wr(g+address,stored);
  uint64_t hnext=observe^loaded,anext=running+loaded;
  remaining=nnext;iteration=inext;history=hnext;acc=anext;last=loaded;old=fresh;
  if(loaded==43)continue;
  if(loaded==45){side_unknown(r,g,c,inext-1);continue;}
  if(loaded==44){kind=1;tag=2;}else if(loaded==777){kind=1;tag=3;}else{kind=5;tag=13;}
  break;
 }
 if(!kind)return 0xdeadbeef;
 uint64_t answer=acc^history^state^last^iteration^x^tag;
 wr(r,answer);wr(r+8,history);wr(r+16,state);wr(r+24,iteration);wr(r+32,acc);wr(r+40,remaining);wr(r+48,last);
 const uint64_t effects[]={kind,0,0xe000+tag,last,kind==1,x,tag};
 for(unsigned j=0;j<7;j++)wr(exit+j*8,effects[j]);
 return answer;
}
