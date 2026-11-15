#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern uint32_t f(void*,void*,uint64_t,uint64_t,_Bool);
static uint64_t get(const unsigned char*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i));}
int main(void){
 unsigned char input[96],arena[64],out[80];
 while(fread(input,1,sizeof(input),stdin)==sizeof(input)){
  memcpy(arena,input+32,64);unsigned offset=input[17]?0:32;
  memcpy(arena+offset,input+20,4);
  uint32_t result=f(arena,arena+offset,get(input),get(input+8),input[16]!=0);
  put(out,result);memcpy(out+8,arena,64);put(out+72,offset);
  if(fwrite(out,1,sizeof(out),stdout)!=sizeof(out))return 3;
 }
 return ferror(stdin)||fflush(stdout)?2:0;
}
