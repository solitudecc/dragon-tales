#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern uint32_t f(void *,void *,uint32_t);
static uint32_t get32(const unsigned char *p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i);return v;}
static void put64(unsigned char *p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=(unsigned char)(v>>(8*i));}
int main(void){
 unsigned char input[96],arena[64],out[80];const unsigned offsets[4]={32,0,1,2};
 while(fread(input,1,sizeof(input),stdin)==sizeof(input)){
  if(input[17]>=4)return 4;
  unsigned offset=offsets[input[17]];memcpy(arena,input+32,64);memcpy(arena+offset,input+20,4);
  uint32_t result=f(arena,arena+offset,get32(input));put64(out,result);memcpy(out+8,arena,64);put64(out+72,offset);
  if(fwrite(out,1,sizeof(out),stdout)!=sizeof(out))return 3;
 }
 return ferror(stdin)||fflush(stdout)?2:0;
}
