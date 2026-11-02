"""Unicorn difftest for SSE instructions: all 128 bits of XMM0-XMM3 (the two
64-bit halves XMMn / XMMnH), RAX, the flags of comisd and the data page."""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, struct, sys

import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import *
X = dragon.x86
A = dragon.Architecture.X86_64
asm = dragon.Assembler(A, dragon.Configuration())
dis = dragon.Disassembler(A, dragon.Configuration())
CODE, DATA = 0x100000, 0x200000
XMM_OFF = 143   # kOffXMM0, 8 bytes each (low half)
UCX = [getattr(sys.modules['unicorn.x86_const'], f'UC_X86_REG_XMM{i}') for i in range(4)]
SPECIAL = [0.0, -0.0, 1.0, -1.5, 3.25, float('inf'), float('-inf'), float('nan'), 1e300, -7.0]
def rnd_double(rng):
    v = rng.choice(SPECIAL) if rng.random() < 0.5 else rng.uniform(-1e6, 1e6)
    return struct.unpack('<Q', struct.pack('<d', v))[0]
def xreg(i): return (XMM_OFF + 8 * i, 64, f'XMM{i}')
def xhreg(i): return getattr(X, f'XMM{i}H')
T = ['comisd xmm0, xmm1', 'ucomisd xmm0, xmm1', 'comisd xmm0, qword ptr [rbx]', 'ucomisd xmm1, qword ptr [rbx + 8]',
     'andpd xmm0, xmm1', 'andnpd xmm0, xmm1', 'orpd xmm0, xmm1', 'xorpd xmm0, xmm1', 'pxor xmm0, xmm0',
     'andps xmm2, xmm3', 'xorps xmm1, xmm1', 'andpd xmm0, xmmword ptr [rbx]', 'orps xmm1, xmmword ptr [rbx + 16]',
     'movapd xmm0, xmm1', 'movupd xmm2, xmm3', 'movaps xmm1, xmm0', 'movdqa xmm3, xmm2',
     'movupd xmm0, xmmword ptr [rbx]', 'movdqu xmm1, xmmword ptr [rbx + 8]', 'movaps xmm2, xmmword ptr [rbx + 16]',
     'movq xmm0, rax', 'movq rax, xmm1', 'movq xmm2, xmm3', 'movq xmm0, qword ptr [rbx + 8]', 'movq qword ptr [rbx + 8], xmm1',
     'movd xmm0, eax', 'movd eax, xmm1', 'movd xmm2, dword ptr [rbx + 4]', 'movd dword ptr [rbx + 4], xmm3',
     'addsd xmm0, qword ptr [rbx]', 'subsd xmm1, qword ptr [rbx + 8]', 'divsd xmm0, xmm1', 'divsd xmm2, qword ptr [rbx]',
     'cvtsi2sd xmm0, dword ptr [rbx]', 'cvtsi2sd xmm1, qword ptr [rbx + 8]',
     'movsd xmm0, qword ptr [rbx + 8]', 'movsd xmm0, xmm1', 'movsd qword ptr [rbx + 8], xmm2']
# packed SSE on the two halves
T += ['movups xmmword ptr [rbx + 16], xmm1', 'movaps xmmword ptr [rbx], xmm2', 'movdqu xmmword ptr [rbx + 8], xmm3',
      'movdqa xmmword ptr [rbx + 16], xmm0', 'movupd xmmword ptr [rbx], xmm1', 'movapd xmmword ptr [rbx + 16], xmm2',
      'pand xmm0, xmm1', 'pandn xmm0, xmm1', 'pand xmm0, xmmword ptr [rbx]', 'por xmm1, xmmword ptr [rbx + 16]',
      'pxor xmm2, xmmword ptr [rbx]', 'pandn xmm3, xmmword ptr [rbx + 16]', 'xorps xmm2, xmm3', 'andnps xmm1, xmm2']
for lane in 'bwdq':
    T += [f'padd{lane} xmm0, xmm1', f'psub{lane} xmm0, xmm1']
    if lane != 'q':
        T += [f'pcmpeq{lane} xmm0, xmm1', f'pcmpgt{lane} xmm0, xmm1']
T += ['paddd xmm2, xmmword ptr [rbx + 16]', 'psubq xmm1, xmmword ptr [rbx]', 'pcmpeqb xmm0, xmm0', 'pcmpeqd xmm3, xmmword ptr [rbx]']
for u in ('bw', 'wd', 'dq', 'qdq'):
    T += [f'punpckl{u} xmm0, xmm1', f'punpckh{u} xmm0, xmm1']
T += ['punpcklqdq xmm2, xmmword ptr [rbx]', 'punpcklbw xmm1, xmmword ptr [rbx + 16]', 'unpcklps xmm0, xmm1',
      'unpckhps xmm0, xmm1', 'unpcklpd xmm0, xmm1', 'unpckhpd xmm2, xmm3',
      'pshufd xmm0, xmm1, 0x1b', 'pshufd xmm2, xmm2, 0', 'pshufd xmm3, xmmword ptr [rbx + 16], 0xe4',
      'pshuflw xmm0, xmm1, 0x1b', 'pshufhw xmm0, xmm1, 0xb1', 'pshuflw xmm2, xmmword ptr [rbx], 0x55',
      'shufps xmm0, xmm1, 0x4e', 'shufps xmm2, xmm2, 0x93', 'shufpd xmm0, xmm1, 1', 'shufpd xmm0, xmm1, 2',
      'shufpd xmm3, xmm1, 3']
T += ['psllw xmm0, 3', 'pslld xmm0, 31', 'psllq xmm0, 63', 'psllq xmm1, 64', 'psrlw xmm0, 16', 'psrlw xmm0, 15',
      'psrld xmm0, 7', 'psrlq xmm0, 64', 'psrlq xmm2, 1', 'psraw xmm0, 9', 'psraw xmm0, 200', 'psrad xmm0, 40',
      'psrad xmm3, 1', 'psllq xmm0, xmm1', 'pslld xmm0, xmm1', 'psllw xmm2, xmm1', 'psrld xmm0, xmm1',
      'psrlq xmm3, xmm1', 'psrad xmm0, xmm1', 'psraw xmm2, xmm1', 'psrlw xmm0, xmmword ptr [rbx]']
T += [f'pslldq xmm0, {n}' for n in (1, 3, 8, 11, 15, 16)] + [f'psrldq xmm1, {n}' for n in (2, 5, 8, 9, 15, 17)]
T += ['pmovmskb eax, xmm1', 'pmovmskb eax, xmm0', 'pextrw eax, xmm1, 5', 'pextrw eax, xmm2, 0', 'pinsrw xmm0, eax, 6',
      'pinsrw xmm3, eax, 0', 'movhps xmm0, qword ptr [rbx + 8]', 'movhps qword ptr [rbx + 8], xmm1',
      'movlps xmm0, qword ptr [rbx]', 'movlpd qword ptr [rbx + 16], xmm2', 'movhpd xmm1, qword ptr [rbx]',
      'movhpd qword ptr [rbx], xmm3', 'movlpd xmm2, qword ptr [rbx + 16]', 'movlps qword ptr [rbx + 8], xmm0',
      'movhlps xmm0, xmm1', 'movlhps xmm0, xmm1', 'movhlps xmm2, xmm2',
      'movss xmm0, dword ptr [rbx + 4]', 'movss dword ptr [rbx + 4], xmm1', 'movss xmm0, xmm1']
T += [f'cmp{p}sd xmm0, xmm1' for p in ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')]
T += ['cmpltsd xmm2, qword ptr [rbx]', 'cmpnlesd xmm3, xmm0']
SMALL_COUNT = ('psllq xmm0, xmm1', 'pslld xmm0, xmm1', 'psllw xmm2, xmm1', 'psrld xmm0, xmm1', 'psrlq xmm3, xmm1',
               'psrad xmm0, xmm1', 'psraw xmm2, xmm1')
rng = random.Random(5)
ok = diff = unl = 0
nan_unknown = [0]
for text in T:
    code = bytes(asm.assemble(text))
    blk = dis.lift_trace(dis.disassemble(code, CODE).instructions())
    if blk.unlifted():
        unl += 1; print('UNLIFTED', text); continue
    bad = None
    for trial in range(60):
        xmm = [rnd_double(rng) for _ in range(4)]
        hi = [rng.getrandbits(64) for _ in range(4)]
        if text in SMALL_COUNT:
            xmm[1] = rng.randrange(0, 70)
        if text.startswith('cmp') and rng.random() < 0.3:
            xmm[1] = xmm[0]
        if text.startswith('pcmp') and rng.random() < 0.5:
            # some equal lanes
            m = rng.getrandbits(64)
            xmm[1] = (xmm[0] & m) | (xmm[1] & ~m & (2**64 - 1))
            hi[1] = (hi[0] & m) | (hi[1] & ~m & (2**64 - 1))
        page = b''.join(struct.pack('<Q', rnd_double(rng)) for _ in range(32))
        rax = rng.getrandbits(64)
        uc = Uc(UC_ARCH_X86, UC_MODE_64); uc.mem_map(CODE, 0x1000); uc.mem_map(DATA, 0x1000)
        uc.mem_write(CODE, code); uc.mem_write(DATA, page)
        uc.reg_write(UC_X86_REG_RAX, rax); uc.reg_write(UC_X86_REG_RBX, DATA)
        for i in range(4): uc.reg_write(UCX[i], (hi[i] << 64) | xmm[i])
        uc.emu_start(CODE, CODE + len(code), count=1)
        ex = dragon.SymbolicExecutor()
        ex.set_register(X.RAX, rax); ex.set_register(X.RBX, DATA)
        for i in range(4):
            ex.set_register(xreg(i), xmm[i])
            ex.set_register(xhreg(i), hi[i])
        for f in ('CF', 'PF', 'AF', 'ZF', 'SF', 'OF'): ex.set_register(getattr(X, f), 0)
        ex.map_memory(DATA, page)
        ex.run(blk)
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        checks = [('RAX', ex.concrete_register(X.RAX), uc.reg_read(UC_X86_REG_RAX))]
        checks += [(f'XMM{i}', ex.concrete_register(xreg(i)), uc.reg_read(UCX[i]) & (2**64 - 1)) for i in range(4)]
        checks += [(f'XMM{i}H', ex.concrete_register(xhreg(i)), uc.reg_read(UCX[i]) >> 64) for i in range(4)]
        if 'comisd' in text:
            checks += [(f, ex.concrete_register(getattr(X, f)), (fl >> b) & 1) for f, b in (('ZF', 6), ('PF', 2), ('CF', 0), ('OF', 11), ('SF', 7), ('AF', 4))]
        checks += [(f'mem+{o}', ex.concrete_memory(DATA + o, 64), int.from_bytes(uc.mem_read(DATA + o, 8), 'little')) for o in (0, 8, 16, 24)]
        isnan = lambda v: (v >> 52) & 0x7ff == 0x7ff and v & ((1 << 52) - 1)
        nan_unknown[0] += sum(1 for n, g, w in checks if g is None and n.startswith("XMM") and not n.endswith("H") and isnan(w))
        wrong = [f'{n}: dragon={g if g is None else hex(g)} unicorn={hex(w)}' for n, g, w in checks if g != w and not (g is None and n.startswith("XMM") and not n.endswith("H") and isnan(w))]
        if wrong: bad = wrong; break
    if bad: diff += 1; print('DIFF', text, bad[:3])
    else: ok += 1
print(f'{ok} ok, {diff} diff, {unl} unlifted; NaN results left unknown: {nan_unknown[0]}')
