/* Complete streaming SHA-256 validation fixture, following FIPS 180-4.
 * No host library calls: every byte copy and padding operation is Guest code.
 * This is test code, not a production cryptographic library.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct {
  u32 h[8];
  u64 bytes;
  u32 used;
  u8 block[64];
} Sha256;

static const u32 K[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
  0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
  0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
  0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
  0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
  0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
  0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
  0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
  0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32u-(n))))

__attribute__((noinline)) void sha256_compress(Sha256 *s, const u8 *p) {
  u32 w[64];
  u32 a,b,c,d,e,f,g,h,t1,t2;
  for (u32 i=0; i<16; ++i) {
    w[i]=((u32)p[4*i]<<24)|((u32)p[4*i+1]<<16)|
         ((u32)p[4*i+2]<<8)|(u32)p[4*i+3];
  }
  for (u32 i=16; i<64; ++i) {
    u32 x=w[i-15],y=w[i-2];
    u32 s0=ROR(x,7)^ROR(x,18)^(x>>3);
    u32 s1=ROR(y,17)^ROR(y,19)^(y>>10);
    w[i]=w[i-16]+s0+w[i-7]+s1;
  }
  a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];
  e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
  for (u32 i=0; i<64; ++i) {
    u32 big1=ROR(e,6)^ROR(e,11)^ROR(e,25);
    u32 choose=(e&f)^((~e)&g);
    u32 big0=ROR(a,2)^ROR(a,13)^ROR(a,22);
    u32 majority=(a&b)^(a&c)^(b&c);
    t1=h+big1+choose+K[i]+w[i];
    t2=big0+majority;
    h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
  }
  s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;
  s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

__attribute__((noinline)) void sha256_init(Sha256 *s) {
  s->h[0]=0x6a09e667u;s->h[1]=0xbb67ae85u;
  s->h[2]=0x3c6ef372u;s->h[3]=0xa54ff53au;
  s->h[4]=0x510e527fu;s->h[5]=0x9b05688cu;
  s->h[6]=0x1f83d9abu;s->h[7]=0x5be0cd19u;
  s->bytes=0;s->used=0;
}

__attribute__((noinline)) void sha256_update(Sha256 *s, const u8 *p, u32 n) {
  s->bytes+=(u64)n;
  while (n) {
    if (s->used==0 && n>=64) {
      sha256_compress(s,p);p+=64;n-=64;
    } else {
      u32 take=64-s->used;
      if (take>n) take=n;
      for (u32 i=0;i<take;++i) s->block[s->used+i]=p[i];
      s->used+=take;p+=take;n-=take;
      if (s->used==64) {sha256_compress(s,s->block);s->used=0;}
    }
  }
}

__attribute__((noinline)) void sha256_finish(Sha256 *s, u8 *out) {
  u64 bits=s->bytes*8u;
  u32 i=s->used;
  s->block[i++]=0x80;
  if (i>56) {
    while (i<64) s->block[i++]=0;
    sha256_compress(s,s->block);i=0;
  }
  while (i<56) s->block[i++]=0;
  for (u32 j=0;j<8;++j) s->block[63-j]=(u8)(bits>>(j*8u));
  sha256_compress(s,s->block);
  for (u32 j=0;j<8;++j) {
    out[4*j]=(u8)(s->h[j]>>24);out[4*j+1]=(u8)(s->h[j]>>16);
    out[4*j+2]=(u8)(s->h[j]>>8);out[4*j+3]=(u8)s->h[j];
  }
}

__attribute__((noinline)) u32 kernel(const u8 *p, u32 n, u8 *out, u32 chunk) {
  Sha256 s;
  sha256_init(&s);
  /* A zero-length update and chunked updates exercise both API paths. */
  sha256_update(&s,p,0);
  if (chunk==0) sha256_update(&s,p,n);
  else {
    u32 offset=0;
    while (offset<n) {
      u32 take=n-offset;
      if (take>chunk) take=chunk;
      sha256_update(&s,p+offset,take);offset+=take;
    }
  }
  sha256_finish(&s,out);
  return ((u32)out[0]<<24)|((u32)out[1]<<16)|((u32)out[2]<<8)|out[3];
}
