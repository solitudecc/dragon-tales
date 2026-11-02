"""Unicorn difftest for SSE instructions: all 128 bits of XMM0-XMM3 (the two
64-bit halves XMMn / XMMnH), RAX, the flags of comisd and the data page."""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, re, struct, sys

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
SPECIAL = [0.0, -0.0, 1.0, -1.5, 3.25, float('inf'), float('-inf'), float('nan'), 1e300, -7.0, 2**31-0.5, -2**31-0.75, float(2**31), float(2**63), -float(2**63)]
SPECIAL32 = [0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,
             0x7fc00001,0x7f800001,1,0x7fffff,0x800000,0x7f7fffff,
             0x4effffff,0x4f000000,0xcf000000,0xcf000001,0x5effffff,0x5f000000,0xdf000000]
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
T += ['cvtss2sd xmm0, xmm1','cvtss2sd xmm0, dword ptr [rbx]',
      'cvtsd2ss xmm0, xmm1','cvtsd2ss xmm0, qword ptr [rbx]']
INTEGER = ['packuswb xmm0, xmm1',
 'packusdw xmm0, xmm1',
 'packsswb xmm0, xmm1',
 'packssdw xmm0, xmm1',
 'pmuludq xmm0, xmm1',
 'pmullw xmm0, xmm1',
 'pmulld xmm0, xmm1',
 'pmulhw xmm0, xmm1',
 'pmulhuw xmm0, xmm1',
 'pmaddwd xmm0, xmm1',
 'psadbw xmm0, xmm1',
 'pshufb xmm0, xmm1',
 'ptest xmm0, xmm1',
 'paddusb xmm0, xmm1',
 'paddusw xmm0, xmm1',
 'psubusb xmm0, xmm1',
 'psubusw xmm0, xmm1',
 'paddsb xmm0, xmm1',
 'paddsw xmm0, xmm1',
 'psubsb xmm0, xmm1',
 'psubsw xmm0, xmm1',
 'pavgb xmm0, xmm1',
 'pavgw xmm0, xmm1',
 'pminub xmm0, xmm1',
 'pminsw xmm0, xmm1',
 'pminsb xmm0, xmm1',
 'pminuw xmm0, xmm1',
 'pminsd xmm0, xmm1',
 'pminud xmm0, xmm1',
 'pmaxub xmm0, xmm1',
 'pmaxsw xmm0, xmm1',
 'pmaxsb xmm0, xmm1',
 'pmaxuw xmm0, xmm1',
 'pmaxsd xmm0, xmm1',
 'pmaxud xmm0, xmm1',
 'pabsb xmm0, xmm1',
 'pabsw xmm0, xmm1',
 'pabsd xmm0, xmm1',
 'psignb xmm0, xmm1',
 'psignw xmm0, xmm1',
 'psignd xmm0, xmm1',
 'packuswb xmm0, xmmword ptr [rbx]',
 'packusdw xmm0, xmmword ptr [rbx]',
 'packsswb xmm0, xmmword ptr [rbx]',
 'packssdw xmm0, xmmword ptr [rbx]',
 'pmuludq xmm0, xmmword ptr [rbx]',
 'pmullw xmm0, xmmword ptr [rbx]',
 'pmulld xmm0, xmmword ptr [rbx]',
 'pmulhw xmm0, xmmword ptr [rbx]',
 'pmulhuw xmm0, xmmword ptr [rbx]',
 'pmaddwd xmm0, xmmword ptr [rbx]',
 'psadbw xmm0, xmmword ptr [rbx]',
 'pshufb xmm0, xmmword ptr [rbx]',
 'ptest xmm0, xmmword ptr [rbx]',
 'paddusb xmm0, xmmword ptr [rbx]',
 'paddusw xmm0, xmmword ptr [rbx]',
 'psubusb xmm0, xmmword ptr [rbx]',
 'psubusw xmm0, xmmword ptr [rbx]',
 'paddsb xmm0, xmmword ptr [rbx]',
 'paddsw xmm0, xmmword ptr [rbx]',
 'psubsb xmm0, xmmword ptr [rbx]',
 'psubsw xmm0, xmmword ptr [rbx]',
 'pavgb xmm0, xmmword ptr [rbx]',
 'pavgw xmm0, xmmword ptr [rbx]',
 'pminub xmm0, xmmword ptr [rbx]',
 'pminsw xmm0, xmmword ptr [rbx]',
 'pminsb xmm0, xmmword ptr [rbx]',
 'pminuw xmm0, xmmword ptr [rbx]',
 'pminsd xmm0, xmmword ptr [rbx]',
 'pminud xmm0, xmmword ptr [rbx]',
 'pmaxub xmm0, xmmword ptr [rbx]',
 'pmaxsw xmm0, xmmword ptr [rbx]',
 'pmaxsb xmm0, xmmword ptr [rbx]',
 'pmaxuw xmm0, xmmword ptr [rbx]',
 'pmaxsd xmm0, xmmword ptr [rbx]',
 'pmaxud xmm0, xmmword ptr [rbx]',
 'pabsb xmm0, xmmword ptr [rbx]',
 'pabsw xmm0, xmmword ptr [rbx]',
 'pabsd xmm0, xmmword ptr [rbx]',
 'psignb xmm0, xmmword ptr [rbx]',
 'psignw xmm0, xmmword ptr [rbx]',
 'psignd xmm0, xmmword ptr [rbx]',
 'palignr xmm0, xmm1, 0',
 'palignr xmm0, xmm1, 7',
 'palignr xmm0, xmm1, 16',
 'palignr xmm0, xmm1, 31',
 'palignr xmm0, xmm1, 32',
 'palignr xmm0, xmm1, 255',
 'palignr xmm0, xmmword ptr [rbx], 17',
 'blendps xmm0, xmm1, 5',
 'blendpd xmm0, xmmword ptr [rbx], 2',
 'pblendw xmm0, xmm1, 147',
 'pextrb eax, xmm1, 3',
 'pextrb byte ptr [rbx], xmm1, 3',
 'pinsrb xmm0, eax, 3',
 'pinsrb xmm0, byte ptr [rbx], 3',
 'pextrd eax, xmm1, 3',
 'pextrd dword ptr [rbx], xmm1, 3',
 'pinsrd xmm0, eax, 3',
 'pinsrd xmm0, dword ptr [rbx], 3',
 'pextrq rax, xmm1, 3',
 'pextrq qword ptr [rbx], xmm1, 3',
 'pinsrq xmm0, rax, 3',
 'pinsrq xmm0, qword ptr [rbx], 3',
 'pinsrw xmm0, word ptr [rbx], 7',
 'pmovsxbw xmm0, xmm1',
 'pmovsxbw xmm0, qword ptr [rbx]',
 'pmovsxbd xmm0, xmm1',
 'pmovsxbd xmm0, dword ptr [rbx]',
 'pmovsxbq xmm0, xmm1',
 'pmovsxbq xmm0, word ptr [rbx]',
 'pmovsxwd xmm0, xmm1',
 'pmovsxwd xmm0, qword ptr [rbx]',
 'pmovsxwq xmm0, xmm1',
 'pmovsxwq xmm0, dword ptr [rbx]',
 'pmovsxdq xmm0, xmm1',
 'pmovsxdq xmm0, qword ptr [rbx]',
 'pmovzxbw xmm0, xmm1',
 'pmovzxbw xmm0, qword ptr [rbx]',
 'pmovzxbd xmm0, xmm1',
 'pmovzxbd xmm0, dword ptr [rbx]',
 'pmovzxbq xmm0, xmm1',
 'pmovzxbq xmm0, word ptr [rbx]',
 'pmovzxwd xmm0, xmm1',
 'pmovzxwd xmm0, qword ptr [rbx]',
 'pmovzxwq xmm0, xmm1',
 'pmovzxwq xmm0, dword ptr [rbx]',
 'pmovzxdq xmm0, xmm1',
 'pmovzxdq xmm0, qword ptr [rbx]']
T += [text for text in INTEGER if text not in T]
FLOAT = []
for suffix in ('ss','sd','ps','pd'):
    memory = 'dword' if suffix == 'ss' else 'qword' if suffix == 'sd' else 'xmmword'
    for operation in ('add','sub','mul','div','min','max','sqrt'):
        FLOAT += [f'{operation}{suffix} xmm0, xmm1', f'{operation}{suffix} xmm0, {memory} ptr [rbx]']
    for predicate in ('eq','lt','le','unord','neq','nlt','nle','ord'):
        FLOAT += [f'cmp{predicate}{suffix} xmm0, xmm1']
for suffix in ('ss','sd'):
    memory='dword' if suffix == 'ss' else 'qword'
    FLOAT += [f'{op}{suffix} xmm0, xmm1' for op in ('comi','ucomi')]
    FLOAT += [f'{op}{suffix} xmm0, {memory} ptr [rbx]' for op in ('comi','ucomi')]
    for gpr,integer_memory in [('eax','dword'),('rax','qword')]:
        FLOAT += [f'cvtsi2{suffix} xmm0, {gpr}',f'cvtsi2{suffix} xmm0, {integer_memory} ptr [rbx]']
        FLOAT += [f'cvtt{suffix}2si {gpr}, xmm1',f'cvtt{suffix}2si {gpr}, {memory} ptr [rbx]']
FLOAT += ['cvtps2pd xmm0, xmm1','cvtps2pd xmm0, qword ptr [rbx]',
          'cvtpd2ps xmm0, xmm1','cvtpd2ps xmm0, xmmword ptr [rbx]',
          'cvtss2sd xmm0, xmm1','cvtss2sd xmm0, dword ptr [rbx]',
          'cvtsd2ss xmm0, xmm1','cvtsd2ss xmm0, qword ptr [rbx]']
T += [text for text in FLOAT if text not in T]
SMALL_COUNT = ('psllq xmm0, xmm1', 'pslld xmm0, xmm1', 'psllw xmm2, xmm1', 'psrld xmm0, xmm1', 'psrlq xmm3, xmm1',
               'psrad xmm0, xmm1', 'psraw xmm2, xmm1')
rng = random.Random(5)
ok = diff = unl = 0
nan_unknown = [0]
for text in T:
    if os.environ.get('ONLY') and os.environ['ONLY'] not in text: continue
    code = bytes(asm.assemble(text))
    blk = dis.lift_trace(dis.disassemble(code, CODE).instructions())
    if blk.unlifted():
        unl += 1; print('UNLIFTED', text); continue
    bad = None
    mnemonic=text.split()[0]
    result32=mnemonic.endswith(('ss','ps')) or mnemonic == 'cvtpd2ps'
    fp32input=(mnemonic.endswith(('ss','ps')) and not mnemonic.startswith('cvtsi')) or mnemonic in ('cvtss2sd','cvtps2pd','cvttss2si')
    destination_match=re.match(r'\w+\s+xmm(\d+)\b',text)
    destination=int(destination_match[1]) if destination_match else None
    nan_operation=mnemonic.startswith(('add','sub','mul','div','sqrt')) or mnemonic in ('cvtss2sd','cvtsd2ss','cvtps2pd','cvtpd2ps')
    packed=mnemonic.endswith(('ps','pd')) and not mnemonic.startswith(('comi','ucomi'))
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
        if fp32input:
            def fbits(index):
                return SPECIAL32[(trial+index)%len(SPECIAL32)] if trial<len(SPECIAL32) else rng.getrandbits(32)
            xmm=[fbits(i*4) | (fbits(i*4+1)<<32) for i in range(4)]
            hi=[fbits(i*4+2) | (fbits(i*4+3)<<32) for i in range(4)]
            page=b''.join(struct.pack('<I',fbits(i+1)) for i in range(64))
        if text in INTEGER:
            edges=[0,0xffffffffffffffff,0x8080808080808080,0x7f7f7f7f7f7f7f7f,
                   0x80007fff80007fff,0x800000007fffffff,0x000100ff0100ff00,0x0123456789abcdef]
            xmm=[edges[(trial+i)%len(edges)] if trial<8 else rng.getrandbits(64) for i in range(4)]
            hi=[edges[(trial+i+3)%len(edges)] if trial<8 else rng.getrandbits(64) for i in range(4)]
            if trial<8: page=b''.join(struct.pack('<Q',edges[(trial+i)%len(edges)]) for i in range(32))
        rax = rng.getrandbits(64)
        if mnemonic.startswith('cvtsi'):
            integers=[0,1,0xffffffffffffffff,0x1000001,0x80000000,0x7fffffff,0x8000000000000000,0x7fffffffffffffff]
            if trial<len(integers): rax=integers[trial]
            page=struct.pack('<Q',rax)+page[8:]
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
        result=ex.run(blk)
        if not result.ok: bad=["executor failed"]; break
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        checks = [('RAX', ex.concrete_register(X.RAX), uc.reg_read(UC_X86_REG_RAX))]
        if result32:
            checks += [(f'XMM{i}.lane{lane}',ex.concrete_register((XMM_OFF+i*8+lane*4 if lane<2 else 271+i*8+(lane-2)*4,32,'lane')),
                        (uc.reg_read(UCX[i]) >> (lane*32)) & 0xffffffff) for i in range(4) for lane in range(4)]
        else:
            checks += [(f'XMM{i}', ex.concrete_register(xreg(i)), uc.reg_read(UCX[i]) & (2**64 - 1)) for i in range(4)]
            checks += [(f'XMM{i}H', ex.concrete_register(xhreg(i)), uc.reg_read(UCX[i]) >> 64) for i in range(4)]
        if 'comisd' in text or 'comiss' in text or text.startswith('ptest'):
            checks += [(f, ex.concrete_register(getattr(X, f)), (fl >> b) & 1) for f, b in (('ZF', 6), ('PF', 2), ('CF', 0), ('OF', 11), ('SF', 7), ('AF', 4))]
        checks += [(f'mem+{o}', ex.concrete_memory(DATA + o, 64), int.from_bytes(uc.mem_read(DATA + o, 8), 'little')) for o in (0, 8, 16, 24)]
        def nan_result(name,got,want):
            if got is not None or not nan_operation or destination is None: return False
            base=f'XMM{destination}'
            if result32:
                if not name.startswith(base+'.lane'): return False
                lane=int(name.rsplit('lane',1)[1])
                active=lane < (2 if mnemonic=='cvtpd2ps' else 4 if packed else 1)
                return active and want & 0x7f800000 == 0x7f800000 and bool(want & 0x7fffff)
            if name not in ([base,base+'H'] if packed else [base]): return False
            return want & 0x7ff0000000000000 == 0x7ff0000000000000 and bool(want & ((1<<52)-1))
        nan_unknown[0] += sum(1 for n,g,w in checks if nan_result(n,g,w))
        wrong = [f'{n}: dragon={g if g is None else hex(g)} unicorn={hex(w)}'
                 for n,g,w in checks if g != w and not nan_result(n,g,w)]
        if wrong: bad = wrong; break
    if bad: diff += 1; print('DIFF', text, bad[:3])
    else: ok += 1
print(f'{ok} ok, {diff} diff, {unl} unlifted; NaN results left unknown: {nan_unknown[0]}')
if diff or unl: sys.exit(1)
