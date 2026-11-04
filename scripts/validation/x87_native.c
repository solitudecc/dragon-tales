#include <stdint.h>
#include <stdio.h>
struct __attribute__((packed)) Raw80 { uint64_t lo; uint16_t hi; };
static void round_cases(void) {
 struct Raw80 values[]={{0xc000000000000000ULL,0x3fff},{0xc000000000000000ULL,0xbfff},{0x8000000000000001ULL,0x3fff},{0x8000000000000000ULL,0x7ffe},{0x8000000000000000ULL,1}};
 for (int mode=0;mode<2;mode++) for (int rc=0;rc<4;rc++) for (int i=0;i<5;i++) {
  uint16_t cw=0x37f|(rc<<10),sw; uint64_t result=0xaaaaaaaaaaaaaaaaULL;
  if (!mode) __asm__ volatile("fninit; fldcw %2; fldt %3; fistpl %0; fnstsw %1":"=m"(result),"=m"(sw):"m"(cw),"m"(values[i]):"st","memory");
  else __asm__ volatile("fninit; fldcw %2; fldt %3; fstpl %0; fnstsw %1":"=m"(result),"=m"(sw):"m"(cw),"m"(values[i]):"st","memory");
  printf("%d %d %016llx %04x %016llx %04x\n",mode,rc,(unsigned long long)values[i].lo,values[i].hi,(unsigned long long)result,sw);
 }
}

static void control_cases(void){
 uint16_t words[]={0,0xffff,0x37f,0xb7f,0xf7f},sw,got; struct Raw80 v={0,0};
 for(int i=0;i<5;i++){__asm__ volatile("fninit;fldcw %1;fnstcw %0;fninit":"=m"(got):"m"(words[i]):"memory");printf("CW %04x %04x\n",words[i],got);}
 __asm__ volatile("fninit;fstpt %0;fnstsw %1;fninit":"=m"(v),"=m"(sw)::"st","memory");printf("UF %016llx %04x SW %04x\n",(unsigned long long)v.lo,v.hi,sw);
 __asm__ volatile("fninit;fld1;fld1;fld1;fld1;fld1;fld1;fld1;fld1;fld1;fnstsw %1;fstpt %0;fninit":"=m"(v),"=m"(sw)::"st","memory");printf("OF %016llx %04x SW %04x\n",(unsigned long long)v.lo,v.hi,sw);
 struct Raw80 huge={0x8000000000000000ULL,0x7ffe};uint32_t integer;uint16_t unmask=0x37e;
 __asm__ volatile("fninit;fldt %3;fistpl %2;fldcw %1;fnstsw %0;fninit":"=m"(sw):"m"(unmask),"m"(integer),"m"(huge):"st","memory");printf("UNMASK %04x\n",sw);
 struct Raw80 tiny[]={ {0xfffffffffffff7ffULL,0x3c00},{0xfffffffffffff800ULL,0x3c00},{0xfffffffffffff801ULL,0x3c00} };uint64_t result;
 for(int i=0;i<3;i++){__asm__ volatile("fninit;fldt %2;fstpl %0;fnstsw %1":"=m"(result),"=m"(sw):"m"(tiny[i]):"st","memory");printf("TINY %016llx %04x\n",(unsigned long long)result,sw);}
 __asm__ volatile("fninit");
}

static void exchange_cases(void){struct Raw80 a,b;uint16_t sw;
 for(int which=0;which<3;which++){
 if(which==0)__asm__ volatile("fninit;fld1;fldz;ffree %%st(0);fxch %%st(1);fnstsw %2;fstpt %0;fstpt %1;fninit":"=m"(a),"=m"(b),"=m"(sw)::"st","memory");
 if(which==1)__asm__ volatile("fninit;fld1;fldz;ffree %%st(1);fxch %%st(1);fnstsw %2;fstpt %0;fstpt %1;fninit":"=m"(a),"=m"(b),"=m"(sw)::"st","memory");
 if(which==2)__asm__ volatile("fninit;fld1;fldz;ffree %%st(0);ffree %%st(1);fxch %%st(1);fnstsw %2;fstpt %0;fstpt %1;fninit":"=m"(a),"=m"(b),"=m"(sw)::"st","memory");
 printf("%d %016llx %04x %016llx %04x %04x\n",which,(unsigned long long)a.lo,a.hi,(unsigned long long)b.lo,b.hi,sw);
 }
 struct Raw80 v[]={ {0x8000000000000000ULL,0},{0x1234,0x3fff},{0x1234,0x7fff},{1,0},{0xc000000000000001ULL,0x7fff},
                    {0x4474d2addbb03b5bULL,0x7a5c} };
 for(unsigned i=0;i<sizeof(v)/sizeof(v[0]);i++){__asm__ volatile("fninit;fldt %1;fxam;fnstsw %0;fninit":"=m"(sw):"m"(v[i]):"st","memory");printf("XAM %016llx %04x %04x\n",(unsigned long long)v[i].lo,v[i].hi,sw);}
}

static void special_cases(void) {
 struct Raw80 input={0x8000000000001234ULL,0x7fff},out;uint32_t f32;uint64_t f64;uint16_t sw;uint32_t in32=0x7f801234;uint64_t in64=0x7ff0000000001234ULL;
 __asm__ volatile("fninit;flds %1;fstpt %0":"=m"(out):"m"(in32):"st","memory");printf("SNAN_LOAD32 %016llx %04x\n",(unsigned long long)out.lo,out.hi);
 __asm__ volatile("fninit;fldl %1;fstpt %0":"=m"(out):"m"(in64):"st","memory");printf("SNAN_LOAD64 %016llx %04x\n",(unsigned long long)out.lo,out.hi);
 __asm__ volatile("fninit;fldt %2;fstps %0;fnstsw %1":"=m"(f32),"=m"(sw):"m"(input):"st","memory");printf("SNAN_STORE32 %08x %04x\n",f32,sw);
 __asm__ volatile("fninit;fldt %2;fstpl %0;fnstsw %1":"=m"(f64),"=m"(sw):"m"(input):"st","memory");printf("SNAN_STORE64 %016llx %04x\n",(unsigned long long)f64,sw);
 input.lo=0x8000000000000000ULL;
 __asm__ volatile("fninit;fldt %2;fisttpl %0;fnstsw %1":"=m"(f32),"=m"(sw):"m"(input):"st","memory");printf("FISTTP_INF32 %08x %04x\n",f32,sw);
 __asm__ volatile("fninit;fldt %2;fisttpq %0;fnstsw %1":"=m"(f64),"=m"(sw):"m"(input):"st","memory");printf("FISTTP_INF64 %016llx %04x\n",(unsigned long long)f64,sw);
 uint64_t flags;
 __asm__ volatile("fninit;fld1;pushq $0x8d7;popfq;fcomi %%st(0),%%st;pushfq;popq %0;fninit":"=r"(flags)::"st","cc","memory");printf("FCOMI_FLAGS %llx\n",(unsigned long long)(flags&0x8d5));
}
static void compare_cases(void) {
 struct Raw80 inputs[]={{0xc000000000000001ULL,0x7fff},{0x8000000000000001ULL,0x7fff},{1,0},{0,0},{0x1234,0x3fff},{0x8000000000000000ULL,0}};
 uint16_t sw;uint32_t denormal=1;
 for (int i=0;i<6;++i) {
  __asm__ volatile("fninit;fld1;fldt %1;fcom %%st(1);fnstsw %0;fninit":"=m"(sw):"m"(inputs[i]):"st","memory");printf("FCOM %d %04x\n",i,sw);
  __asm__ volatile("fninit;fld1;fldt %1;fucom %%st(1);fnstsw %0;fninit":"=m"(sw):"m"(inputs[i]):"st","memory");printf("FUCOM %d %04x\n",i,sw);
  __asm__ volatile("fninit;fldt %2;fcoms %1;fnstsw %0;fninit":"=m"(sw):"m"(denormal),"m"(inputs[i]):"st","memory");printf("FCOM_MEM_DENORM %d %04x\n",i,sw);
 }
}
int main(void) { round_cases();control_cases();exchange_cases();special_cases();compare_cases();return 0; }
