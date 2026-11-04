/* Controlled test library. These are real Guest implementations, not effects
 * injected into Dragon. Standard APIs can also be checked against host libc. */
#include <stdarg.h>
typedef unsigned int u32;
typedef unsigned char u8;
typedef u32 (*Callback)(u32, u32 *);
__attribute__((noinline)) u32 api_value(u32 x) {return x&1u?0xffffffffu:0u;}
__attribute__((noinline)) u32 api_write(u32 *out,u32 x) {
  out[0]=(out[0]+x)^0xa5a5a5a5u;out[1]=x+1u;return x?1u:0u;
}
#ifndef USE_SYSTEM_LIBC
__attribute__((noinline)) void *memcpy(void *d,const void *s,unsigned long n) {
  for(unsigned long i=0;i<n;++i) ((u8 *)d)[i]=((const u8 *)s)[i];return d;
}
__attribute__((noinline)) int memcmp(const void *a,const void *b,unsigned long n) {
  for(unsigned long i=0;i<n;++i) {
    if(((const u8 *)a)[i]!=((const u8 *)b)[i]) return (int)((const u8 *)a)[i]-(int)((const u8 *)b)[i];
  }
  return 0;
}
__attribute__((noinline)) unsigned long strlen(const char *s) {
  unsigned long n=0;while(s[n])++n;return n;
}
#endif
__attribute__((noinline)) u32 api_invoke(Callback callback,u32 x,u32 *out) {
  out[1]+=3u;u32 r=callback(x,out);out[2]^=r;return r+7u;
}
__attribute__((noinline)) u32 api_sum(u32 n,...) {
  va_list args;va_start(args,n);u32 result=0;
  for(u32 i=0;i<n;++i) result+=va_arg(args,u32);
  va_end(args);return result;
}
__attribute__((noinline)) double api_fp(double x) {return x+0.5;}
__attribute__((noinline,noreturn)) void api_stop(u32 x) {
  (void)x;for(;;) __asm__ volatile("nop");
}
