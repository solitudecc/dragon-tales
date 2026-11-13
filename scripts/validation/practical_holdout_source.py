"""Fresh bounded u32 business fixture and an independent mathematical model.

The C implementation stores eight words as explicit little-endian octets;
the specification uses integer words, arithmetic byte replacement and a
bit-string rotation. Constants alone depend on the seed. Neither variant
interprets a bytecode stream, allocates a heap object, or calls an unknown
external. ``out`` must designate sixteen writable bytes, as in the existing
Tigress function oracle. This module does not run a compiler or native code.
"""

import random
import struct

SEED = 2026111307
INPUTS = [0, 1, 2, 7, 0x7F, 0x80, 0x7FFF, 0x8000, 0x7FFFFFFF,
          0x80000000, 0xFFFFFFFE, 0xFFFFFFFF, 8, 15, 16, 31]
GUEST_STEP_LIMIT = 1_000_000
MASK = (1 << 32) - 1
BOUNDS = dict(initial_words=8, outer_rounds=4, inner_per_round=4,
              total_inner_iterations=16, manual_dispatch_iterations=62,
              manual_dispatch_hard_limit=64)
BUSINESS_BRANCH = "bit ((round + inner) & 7) of the original input x"
OPAQUE_PREDICATES = (
    "p*(p+1) is even modulo 2^32",
    "the square of an odd u32 is 1 modulo 8",
)


def _constants(seed):
    rng = random.Random(seed)
    return tuple(rng.getrandbits(32) | 1 for _ in range(6))


_HELPERS = """typedef unsigned int u32;
typedef unsigned char u8;
typedef char require_32_bit_unsigned_int[(sizeof(u32)==4)?1:-1];
static inline u32 rol(u32 v,u32 n) {
  u32 k=n&31u; return (v<<k)|(v>>((32u-k)&31u));
}
static inline u32 rd(const u8 *p) {
  return (u32)p[0]|((u32)p[1]<<8u)|((u32)p[2]<<16u)|((u32)p[3]<<24u);
}
static inline void wr(u8 *p,u32 v) {
  p[0]=(u8)v; p[1]=(u8)(v>>8u); p[2]=(u8)(v>>16u); p[3]=(u8)(v>>24u);
}
"""


def source(seed, wrong=False, manual=False):
    """Return plain or hand-obfuscated raw C; ``wrong`` flips only return bit0."""
    c = _constants(seed)
    head = _HELPERS
    if manual:
        head += """static inline u32 ad(u32 a,u32 b) { return (a^b)+((a&b)<<1u); }
static inline u32 xr(u32 a,u32 b) { return (a|b)-(a&b); }
static inline u32 enc(u32 n) { return (n^0x6a09e667u)+0x9e3779b9u; }
"""
    setup = f"""__attribute__((noinline)) u32 kernel(u32 x,u32 *out) {{
  u8 work[32]; u8 *dst=(u8 *)out;
  const u32 c0={c[0]}u, c1={c[1]}u, c2={c[2]}u;
  const u32 c3={c[3]}u, c4={c[4]}u, c5={c[5]}u;
  u32 acc=x^c2, carry=(x>>3u)+c4;
  u32 r=0u, j=0u, count=0u, idx=0u, mate=0u;
  u32 v=0u, w=0u, t=0u, next=0u, k=0u;
  /* Initialize every octet before any indexed read, including dead paths. */
  for(u32 i=0u;i<8u;++i) {{
    u32 lane=(x+c0*(i+1u))^(c1+i*0x01010101u);
    wr(work+4u*i,rol(lane,3u+5u*i));
  }}
"""
    finish = """  /* Four complete LE words precede the final single-octet patch. */
  u32 o0=rd(work+4u*(x&7u))^acc;
  u32 o1=carry+rd(work+4u*((x>>5u)&7u));
  u32 o2=rol(o0+rd(work+4u*((x>>9u)&7u)),x>>27u)^o1;
  u32 o3=acc^carry^rd(work+4u*((x>>16u)&7u))^x;
  wr(dst,o0); wr(dst+4u,o1); wr(dst+8u,o2); wr(dst+12u,o3);
  dst[(x>>28u)&15u]^=(u8)(carry>>((x&3u)*8u));
  u32 result=(rd(dst+4u*((x>>30u)&3u))+rol(o2^c4,13u))^(o3+c5);
""" + ("  return result^1u;\n}\n" if wrong else "  return result;\n}\n")
    if not manual:
        body = """  /* Four rounds; each chooses one through four real data updates. */
  for(r=0u;r<4u;++r) {
    count=1u+((x>>(2u*r))&3u);
    for(j=0u;j<count;++j) {
      idx=((x>>(4u*r))+3u*r+j)&7u;
      mate=(idx+1u+((x>>(r+1u))&1u))&7u;
      v=rd(work+4u*idx); w=rd(work+4u*mate);
      t=rol(v+acc,7u*r+5u*j+(x>>27u))^(w+c5);
      /* This is a business branch, not an opaque predicate. */
      if((x>>((r+j)&7u))&1u) {
        acc+=t+(x^(c1+17u*r+j));
        next=t^rol(w,r+j+1u);
      } else {
        acc^=rol(t^(x+c2+j),r+2u);
        next=t+(w^c3);
      }
      wr(work+4u*idx,next);
      /* Preserve three octets and change exactly one selected octet. */
      work[4u*idx+((r+2u*j)&3u)]=(u8)((acc>>((j&3u)*8u))^
          (x>>((r&3u)*8u))^(37u*r+11u*j));
      carry=rol(carry+rd(work+4u*idx),v>>27u)^(acc+idx*0x045d9f3bu);
    }
    /* Feed the final round state back through a different array lane. */
    k=(r+(x>>3u))&7u;
    wr(work+4u*k,rd(work+4u*k)^rol(carry,r+1u));
    acc=rol(acc+rd(work+4u*((k+3u)&7u)),5u*r+11u);
  }
"""
    else:
        body = """  volatile u32 state=enc(0u);
  /* Fixed kernel basic blocks, with encoded storage and contiguous cases. */
  for(u32 steps=0u;steps<64u;++steps) {
    switch((state-0x9e3779b9u)^0x6a09e667u) {
    case 0u:
      if(r==4u) { state=enc(6u); break; }
      count=1u+((x>>((2u*r)&31u))&3u); j=0u; state=enc(1u); break;
    case 1u:
      if(j==count) { state=enc(5u); break; }
      idx=ad(x>>((4u*r)&31u),ad(3u*r,j))&7u;
      mate=ad(ad(idx,1u),(x>>((r+1u)&31u))&1u)&7u;
      v=rd(work+4u*idx); w=rd(work+4u*mate);
      t=xr(rol(ad(v,acc),ad(ad(7u*r,5u*j),x>>27u)),ad(w,c5));
      state=enc(((x>>((r+j)&7u))&1u)?2u:3u); break;
    case 2u:
      acc=ad(acc,ad(t,xr(x,ad(ad(c1,17u*r),j))));
      next=xr(t,rol(w,ad(ad(r,j),1u))); state=enc(4u); break;
    case 3u:
      acc=xr(acc,rol(xr(t,ad(ad(x,c2),j)),ad(r,2u)));
      next=ad(t,xr(w,c3)); state=enc(4u); break;
    case 4u: {
      wr(work+4u*idx,next);
      work[4u*idx+((r+2u*j)&3u)]=(u8)((acc>>((j&3u)*8u))^
          (x>>((r&3u)*8u))^(37u*r+11u*j));
      carry=xr(rol(ad(carry,rd(work+4u*idx)),v>>27u),ad(acc,idx*0x045d9f3bu));
      ++j; u32 p=xr(acc,carry);
      state=enc(((p*(p+1u))&1u)==0u?1u:6u); break;
    }
    case 5u: {
      k=ad(r,x>>3u)&7u;
      wr(work+4u*k,xr(rd(work+4u*k),rol(carry,ad(r,1u))));
      acc=rol(ad(acc,rd(work+4u*((k+3u)&7u))),ad(5u*r,11u));
      ++r; u32 q=xr(acc,carry)|1u;
      state=enc(((q*q)&7u)==1u?0u:6u); break;
    }
    case 6u: goto finish;
    default: goto finish;
    }
  }
finish: ;
"""
    return head + setup + body + finish


def _rotate_word(value, count):
    """A permutation of 32 written bits, not C's shift/OR implementation."""
    bits = format(value & MASK, '032b')
    cut = count % 32
    return int(bits[cut:] + bits[:cut], 2)


def _octet_replacement(word, lane, byte):
    scale = 256 ** lane
    return (word % scale) + (byte % 256) * scale + (word // (scale * 256)) * (scale * 256)


def specification(seed, x):
    """Independent u32-word business specification; return (u32, 16 LE bytes)."""
    x = x & MASK
    c0, c1, c2, c3, c4, c5 = _constants(seed)
    words = []
    for lane in range(8):
        left = (x + c0 + lane * c0) % (1 << 32)
        right = (c1 + lane * 0x01010101) % (1 << 32)
        words.append(_rotate_word(left ^ right, 3 + 5 * lane))
    acc, carry = x ^ c2, ((x // 8) + c4) & MASK
    for round_number in range(4):
        iterations = 1 + (x // (4 ** round_number)) % 4
        for inner in range(iterations):
            slot = ((x // (16 ** round_number)) % 8 + 3 * round_number + inner) % 8
            adjacent = (slot + 1 + (x // (2 ** (round_number + 1))) % 2) % 8
            old, neighbor = words[slot], words[adjacent]
            turn = 7 * round_number + 5 * inner + x // (1 << 27)
            mix = _rotate_word(acc + old, turn) ^ ((c5 + neighbor) & MASK)
            if (x // (2 ** ((round_number + inner) % 8))) % 2:
                adjust = x ^ ((c1 + 17 * round_number + inner) & MASK)
                acc = (mix + adjust + acc) & MASK
                replacement = mix ^ _rotate_word(neighbor, round_number + inner + 1)
            else:
                adjust = (x + c2 + inner) & MASK
                acc = acc ^ _rotate_word(mix ^ adjust, round_number + 2)
                replacement = (mix + (neighbor ^ c3)) & MASK
            byte_lane = (round_number + 2 * inner) % 4
            patch = ((acc // (256 ** (inner % 4))) ^
                     (x // (256 ** (round_number % 4))) ^
                     (37 * round_number + 11 * inner)) % 256
            words[slot] = _octet_replacement(replacement, byte_lane, patch)
            carry = (_rotate_word(carry + words[slot], old // (1 << 27)) ^
                     ((acc + slot * 0x045D9F3B) & MASK)) & MASK
        feedback = (round_number + x // 8) % 8
        words[feedback] ^= _rotate_word(carry, round_number + 1)
        acc = _rotate_word(acc + words[(feedback + 3) % 8], 5 * round_number + 11)
    first = words[x % 8] ^ acc
    second = (carry + words[(x // 32) % 8]) & MASK
    third = _rotate_word(first + words[(x // 512) % 8], x // (1 << 27)) ^ second
    fourth = acc ^ carry ^ words[(x // 65536) % 8] ^ x
    result_words = [first, second, third, fourth]
    changed_word, changed_lane = divmod((x // (1 << 28)) % 16, 4)
    patch = (carry // (256 ** (x % 4))) % 256
    result_words[changed_word] ^= patch << (8 * changed_lane)
    selected = result_words[x // (1 << 30)]
    result = (((selected + _rotate_word(third ^ c4, 13)) & MASK) ^
              ((fourth + c5) & MASK)) & MASK
    return result, struct.pack('<4I', *result_words)
