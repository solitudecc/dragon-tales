#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern uint32_t f(void *,uint32_t,_Bool);
static uint32_t get32(const unsigned char *p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i);return v;}
static void put64(unsigned char *p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=(unsigned char)(v>>(8*i));}
int main(void){
 unsigned char input[96],arena[64],out[80];
 while(fread(input,1,sizeof(input),stdin)==sizeof(input)){
  unsigned offset=input[17];if(offset>3)return 4;memcpy(arena,input+32,64);
  uint32_t result=f(arena+offset,get32(input),input[16]!=0);put64(out,result);memcpy(out+8,arena,64);put64(out+72,offset);
  if(fwrite(out,1,sizeof(out),stdout)!=sizeof(out))return 3;
 }
 return ferror(stdin)||fflush(stdout)?2:0;
}
