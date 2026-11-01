"""Unicorn difftest for SSE instructions on the modelled low 64 bits of XMM."""
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
T = ['comisd xmm0, xmm1', 'ucomisd xmm0, xmm1', 'comisd xmm0, qword ptr [rbx]', 'ucomisd xmm1, qword ptr [rbx + 8]',
     'andpd xmm0, xmm1', 'andnpd xmm0, xmm1', 'orpd xmm0, xmm1', 'xorpd xmm0, xmm1', 'pxor xmm0, xmm0',
     'andps xmm2, xmm3', 'xorps xmm1, xmm1', 'andpd xmm0, xmmword ptr [rbx]', 'orps xmm1, xmmword ptr [rbx + 16]',
     'movapd xmm0, xmm1', 'movupd xmm2, xmm3', 'movaps xmm1, xmm0', 'movdqa xmm3, xmm2',
     'movupd xmm0, xmmword ptr [rbx]', 'movdqu xmm1, xmmword ptr [rbx + 8]', 'movaps xmm2, xmmword ptr [rbx + 16]',
     'movq xmm0, rax', 'movq rax, xmm1', 'movq xmm2, xmm3', 'movq xmm0, qword ptr [rbx + 8]', 'movq qword ptr [rbx + 8], xmm1',
     'movd xmm0, eax', 'movd eax, xmm1', 'movd xmm2, dword ptr [rbx + 4]', 'movd dword ptr [rbx + 4], xmm3',
     'addsd xmm0, qword ptr [rbx]', 'subsd xmm1, qword ptr [rbx + 8]', 'divsd xmm0, xmm1', 'divsd xmm2, qword ptr [rbx]',
     'cvtsi2sd xmm0, dword ptr [rbx]', 'cvtsi2sd xmm1, qword ptr [rbx + 8]']
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
        page = b''.join(struct.pack('<Q', rnd_double(rng)) for _ in range(32))
        rax = rng.getrandbits(64)
        uc = Uc(UC_ARCH_X86, UC_MODE_64); uc.mem_map(CODE, 0x1000); uc.mem_map(DATA, 0x1000)
        uc.mem_write(CODE, code); uc.mem_write(DATA, page)
        uc.reg_write(UC_X86_REG_RAX, rax); uc.reg_write(UC_X86_REG_RBX, DATA)
        for i in range(4): uc.reg_write(UCX[i], (hi[i] << 64) | xmm[i])
        uc.emu_start(CODE, CODE + len(code), count=1)
        ex = dragon.SymbolicExecutor()
        ex.set_register(X.RAX, rax); ex.set_register(X.RBX, DATA)
        for i in range(4): ex.set_register(xreg(i), xmm[i])
        for f in ('CF', 'PF', 'AF', 'ZF', 'SF', 'OF'): ex.set_register(getattr(X, f), 0)
        ex.map_memory(DATA, page)
        ex.run(blk)
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        checks = [('RAX', ex.concrete_register(X.RAX), uc.reg_read(UC_X86_REG_RAX))]
        checks += [(f'XMM{i}', ex.concrete_register(xreg(i)), uc.reg_read(UCX[i]) & (2**64 - 1)) for i in range(4)]
        if 'comisd' in text:
            checks += [(f, ex.concrete_register(getattr(X, f)), (fl >> b) & 1) for f, b in (('ZF', 6), ('PF', 2), ('CF', 0), ('OF', 11), ('SF', 7), ('AF', 4))]
        checks += [(f'mem+{o}', ex.concrete_memory(DATA + o, 64), int.from_bytes(uc.mem_read(DATA + o, 8), 'little')) for o in (0, 8, 16)]
        isnan = lambda v: (v >> 52) & 0x7ff == 0x7ff and v & ((1 << 52) - 1)
        nan_unknown[0] += sum(1 for n, g, w in checks if g is None and n.startswith("XMM") and isnan(w))
        wrong = [f'{n}: dragon={g if g is None else hex(g)} unicorn={hex(w)}' for n, g, w in checks if g != w and not (g is None and n.startswith("XMM") and isnan(w))]
        if wrong: bad = wrong; break
    if bad: diff += 1; print('DIFF', text, bad[:3])
    else: ok += 1
print(f'{ok} ok, {diff} diff, {unl} unlifted; NaN results left unknown: {nan_unknown[0]}')
