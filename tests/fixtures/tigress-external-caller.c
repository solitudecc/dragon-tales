/* CASE is selected by the validation driver before Tigress preprocessing. */
typedef unsigned int u32;
typedef unsigned char u8;
typedef u32 (*Callback)(u32, u32 *);
extern u32 api_value(u32);
extern u32 api_write(u32 *, u32);
extern void *memcpy(void *, const void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern u32 api_invoke(Callback, u32, u32 *);
extern u32 api_sum(u32, ...);
extern double api_fp(double);
extern void api_stop(u32) __attribute__((noreturn));

static __attribute__((noinline)) u32 callback(u32 x, u32 *out) {
  out[0]^=x;out[1]+=13u;return out[0]+13u;
}

__attribute__((noinline)) u32 kernel(u32 x, u32 *out, u32 (*fn)(u32)) {
  for (u32 i=0;i<8;++i) out[i]=0;
#if CASE == 0
  u32 r=api_value(x);out[0]=r;out[1]=r==0?17u:29u;return out[1];
#elif CASE == 1
  out[0]=x+5u;
  u32 before=out[0];
  u32 r=api_write(out,x);
  out[2]=out[0]==before?31u:47u;
  return out[0]+out[1]+out[2]+r;
#elif CASE == 2
  u8 src[32];
  for (u32 i=0;i<32;++i) src[i]=(u8)(x+i);
  u32 n=x&15u;
  void *p=memcpy(out,src,n);
  out[7]=p==(void *)out;
  return ((u8 *)out)[n?n-1:0]+out[7];
#elif CASE == 3
  u8 a[4],b[4];
  for (u32 i=0;i<4;++i) a[i]=(u8)(x>>(24u-i*8u));
  b[0]=0x80;b[1]=0;b[2]=0;b[3]=1;
  int r=memcmp(a,b,4);
  out[0]=r<0?0xffffffffu:(r>0?1u:0u);return out[0];
#elif CASE == 4
  char s[32];u32 n=x&15u;
  for (u32 i=0;i<n;++i) s[i]=(char)(1u+(x+i)%255u);
  s[n]=0;out[0]=(u32)strlen(s);return out[0]==n?7u:9u;
#elif CASE == 5
  u32 r=fn(x);out[0]=r;out[1]=r==0?17u:29u;return out[1];
#elif CASE == 6
  out[0]=5;out[1]=1;
  u32 r=api_invoke(callback,x,out);out[3]=r;return r+out[2];
#elif CASE == 7
  u32 r=api_sum(8u,x,1u,2u,3u,4u,5u,6u,7u);
  out[0]=r;return r^0x13579bdfu;
#elif CASE == 8
  double y=api_fp((double)(x&0xffffu)*0.25);
  out[0]=(u32)(y*4.0);out[1]=out[0]==(x&0xffffu)+2u;return out[0];
#elif CASE == 9
  return api_value(x); /* C O2 may become an external tail jump. */
#elif CASE == 10
  api_stop(x); /* Observe its unknown boundary, do not invent a return. */
#else
#error Unknown external API scenario
#endif
}
