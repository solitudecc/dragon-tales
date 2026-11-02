"""Differential test: dragon AArch64 IGNIL lifter (run concretely through the
SymbolicExecutor) vs. Unicorn, one instruction at a time, random states.

For every template: assemble with dragon's Assembler, lift with lift_trace,
seed both sides with the same random X0-X30/SP/NZCV and the same random data
page, execute, and compare X0-X30, SP, NZCV and the data page. Branches compare
the taken target instead (Unicorn's PC vs. the successor whose guard is true).
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, re, sys
from collections import defaultdict


import dragon
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import *

CODE = 0x100000          # where the instruction lives
DATA = 0x200000          # random data page; base registers point here
DATA_SIZE = 0x2000
BASE = DATA + 0x1000     # value given to memory base registers (x9, x10, sp)
TRIALS = int(os.environ.get('TRIALS', 40))

A = dragon.arm
XREGS = [getattr(A, f'X{i}') for i in range(31)]
UC_X = [globals()[f'UC_ARM64_REG_X{i}'] for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
FLAGS = [(A.NF, 31), (A.ZF, 30), (A.CF, 29), (A.VF, 28)]
UC_V = [globals()[f'UC_ARM64_REG_Q{i}'] for i in range(32)]
VHALVES = [[(getattr(A,f'V{i}')[0]+8*half,64,f'V{i}.{half}')
            for half in range(2)] for i in range(32)]

asm = dragon.Assembler(dragon.Architecture.ARM64, dragon.Configuration())
dis = dragon.Disassembler(dragon.Architecture.ARM64, dragon.Configuration())

# Interesting values make carry/overflow edges likely.
EDGES = [0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
         0xffffffff, 0x100000000, 0x7fffffffffffffff, 0x8000000000000000,
         0xffffffffffffffff, 0xfffffffffffffffe]
FLOAT64 = [0,0x8000000000000000,0x3ff0000000000000,0xbfd0000000000000,
           0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000001,0x7ff0000000000001,
           1,0xfffffffffffff,0x10000000000000,0x7fefffffffffffff,
           0x41dfffffffffffff,0x41e0000000000000,0xc1e0000000000000,
           0x43dfffffffffffff,0x43e0000000000000,0xc3e0000000000000,
           0x43efffffffffffff,0x43f0000000000000]
FLOAT32 = [0,0x80000000,0x3f800000,0xbe800000,0x7f800000,0xff800000,
           0x7fc00001,0x7f800001,1,0x7fffff,0x800000,0x7f7fffff,
           0x4effffff,0x4f000000,0xcf000000,0x5effffff,0x5f000000,
           0xdf000000,0x5f7fffff,0x5f800000]


def rand64(rng):
    return rng.choice(EDGES) if rng.random() < 0.35 else rng.getrandbits(64)


def run_case(text, rng, mem_regs, idx=None, trial=0):
    code = bytes(asm.assemble(text))
    assert len(code) == 4, (text, code)
    g = dis.disassemble(code, CODE)
    insns = g.instructions()
    blk = dis.lift_trace(insns)
    if blk.unlifted():
        return 'unlifted', None

    regs = [rand64(rng) for _ in range(31)]
    for r in mem_regs:
        regs[r] = BASE + rng.randrange(-0x100, 0x100, 16)
    if idx == "w":   # w10: small signed low half, garbage upper half
        regs[10] = (rand64(rng) & ~0xffffffff) | (rng.randrange(-8 if "sxtw" in text else 0, 8) & 0xffffffff)
    elif idx == "x":
        regs[10] = rng.randrange(-8, 8) & (2**64 - 1)
    regs[5] = CODE + 0x200       # br/blr x5
    regs[30] = CODE + 0x300      # ret
    sp = BASE + rng.randrange(-0x100, 0x100, 16)
    nzcv = rng.getrandbits(4) << 28
    vectors = [rng.getrandbits(128) for _ in range(32)]
    floating = text.startswith(('fadd','fsub','fmul','fdiv','fcmp','fcsel','frint','fcvt','fsqrt','fneg','fabs'))
    fp_registers = re.findall(r'\b([sd])\d+\b',text)
    fp_width = 32 if fp_registers and fp_registers[-1] == 's' else 64
    result_width = 32 if fp_registers and fp_registers[0] == 's' else 64
    if floating:
        edges = FLOAT32 if fp_width == 32 else FLOAT64
        mask = (1 << fp_width)-1
        for index in range(3):
            value = edges[(trial+index) % len(edges)] if trial < len(edges) else rng.getrandbits(fp_width)
            vectors[index] = (vectors[index] & ~mask) | value
    page = bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))

    # -- Unicorn
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(CODE, 0x1000)  # covers every branch target used
    uc.mem_map(DATA, DATA_SIZE)
    uc.mem_write(CODE, code)
    uc.mem_write(DATA, page)
    for i, v in enumerate(regs):
        uc.reg_write(UC_X[i], v)
    uc.reg_write(UC_ARM64_REG_SP, sp)
    uc.reg_write(UC_ARM64_REG_NZCV, nzcv)
    for reg, value in zip(UC_V,vectors): uc.reg_write(reg,value)
    uc.emu_start(CODE, CODE + 4, count=1)
    exp_regs = [uc.reg_read(r) for r in UC_X]
    exp_sp = uc.reg_read(UC_ARM64_REG_SP)
    exp_nzcv = uc.reg_read(UC_ARM64_REG_NZCV)
    exp_pc = uc.reg_read(UC_ARM64_REG_PC)
    exp_page = bytes(uc.mem_read(DATA, DATA_SIZE))
    exp_vectors = [uc.reg_read(reg) for reg in UC_V]

    # -- dragon
    ex = dragon.SymbolicExecutor()
    for reg, v in zip(XREGS, regs):
        ex.set_register(reg, v)
    ex.set_register(A.SP, sp)
    ex.set_register(A.XZR, 0)
    for halves, value in zip(VHALVES,vectors):
        ex.set_register(halves[0],value & ((1<<64)-1))
        ex.set_register(halves[1],value >> 64)
    for reg, bit in FLAGS:
        ex.set_register(reg, (nzcv >> bit) & 1)
    ex.map_memory(DATA, page)
    res = ex.run(blk)

    diffs = []
    if not res.ok if hasattr(res, 'ok') else False:
        diffs.append(f'run status {res.status} {res.error}')

    insn = insns[0]
    if insn.is_jump or insn.is_return or insn.is_call:
        taken = None
        # The state is fully concrete, so each guard is decided: the taken
        # edge is the one the executor can prove.
        for s in res.successors:
            if s.condition is None or ex.proves(s.condition):
                taken = s.address
        if taken != exp_pc and not (insn.is_call and taken == exp_pc):
            diffs.append(f'target dragon={taken if taken is None else hex(taken)} unicorn={exp_pc:#x}')
    for i, reg in enumerate(XREGS):
        got = ex.concrete_register(reg)
        if got != exp_regs[i]:
            diffs.append(f'X{i}: dragon={got if got is None else hex(got)} unicorn={exp_regs[i]:#x}')
    got = ex.concrete_register(A.SP)
    if got != exp_sp:
        diffs.append(f'SP: dragon={got if got is None else hex(got)} unicorn={exp_sp:#x}')
    for reg, bit in FLAGS:
        got = ex.concrete_register(reg)
        want = (exp_nzcv >> bit) & 1
        if got is None or (got & 1) != want:
            diffs.append(f'{reg[2]}: dragon={got} unicorn={want}')
    for i,(halves, value) in enumerate(zip(VHALVES,exp_vectors)):
        for half,reg in enumerate(halves):
            got = ex.concrete_register(reg)
            want = (value >> (half*64)) & ((1<<64)-1)
            if got != want:
                nan_result = text.startswith(('fadd','fsub','fmul','fdiv','frint','fsqrt','fcvt d','fcvt s'))
                if got is None and half == 0 and nan_result:
                    if result_width == 32 and (want & 0x7f800000) == 0x7f800000 and want & 0x7fffff:
                        if ex.concrete_register((reg[0]+4,32,'upper32')) == want >> 32: continue
                    elif result_width == 64 and (want & 0x7ff0000000000000) == 0x7ff0000000000000 and want & ((1<<52)-1):
                        continue
                diffs.append(f'V{i}.{half}: dragon={got} unicorn={want:#x}')
    for off in range(0, DATA_SIZE, 8):
        got = ex.concrete_memory(DATA + off, 64)
        want = int.from_bytes(exp_page[off:off + 8], 'little')
        if got != want:
            diffs.append(f'mem[{DATA + off:#x}]: dragon={got if got is None else hex(got)} unicorn={want:#x}')
            break
    return ('ok' if not diffs else 'diff'), (diffs, regs, sp, nzcv)


# (template, registers holding a data address)
T = []
for w, r in (('x', 64), ('w', 32)):
    T += [(f'movz {w}0, #0x1234, lsl #16', []), (f'movn {w}0, #0x1234', []),
          (f'movk {w}0, #0xbeef, lsl #16', []), (f'mov {w}0, {w}1', []),
          (f'mov {w}0, #-2', [])]
    for op in ('add', 'sub', 'adds', 'subs'):
        T += [(f'{op} {w}0, {w}1, {w}2', []), (f'{op} {w}0, {w}1, #0xabc', []),
              (f'{op} {w}0, {w}1, #0xabc, lsl #12', []),
              (f'{op} {w}0, {w}1, {w}2, lsl #3', []), (f'{op} {w}0, {w}1, {w}2, lsr #7', []),
              (f'{op} {w}0, {w}1, {w}2, asr #5', [])]
    T += [(f'cmp {w}1, {w}2', []), (f'cmp {w}1, #5', []), (f'cmn {w}1, {w}2', []),
          (f'cmn {w}1, #1', []), (f'tst {w}1, {w}2', []), (f'tst {w}1, #0xff', [])]
    for op in ('and', 'orr', 'eor', 'ands'):
        T += [(f'{op} {w}0, {w}1, {w}2', []), (f'{op} {w}0, {w}1, #0xff00', []),
              (f'{op} {w}0, {w}1, {w}2, lsl #4', []), (f'{op} {w}0, {w}1, {w}2, asr #3', []),
              (f'{op} {w}0, {w}1, {w}2, ror #9', [])]
    T += [(f'add {w}0, {"sp" if w == "x" else "wsp"}, #16', []),
          (f'sub {"sp" if w == "x" else "wsp"}, {"sp" if w == "x" else "wsp"}, #32', [])]
    T += [(f'ldr {w}0, [x9]', [9]), (f'ldr {w}0, [x9, #8]', [9]), (f'ldr {w}0, [x9, #16]!', [9]),
          (f'ldr {w}0, [x9], #-8', [9]), (f'str {w}1, [x9, #24]', [9]),
          (f'str {w}1, [x9, #-16]!', [9]), (f'str {w}1, [x9], #8', [9]),
          (f'ldp {w}0, {w}1, [x9, #16]', [9]), (f'stp {w}2, {w}3, [x9, #-32]!', [9]),
          (f'ldp {w}0, {w}1, [x9], #16', [9]), (f'stp {w}2, {w}3, [x9], #16', [9]),
          (f'ldr {w}0, [x9, x10]', [9], 'x'), (f'ldur {w}0, [x9, #-3]', [9]),
          (f'ldr {w}0, [x9, x10, lsl #{3 if w == "x" else 2}]', [9], 'x'),
          (f'ldr {w}0, [x9, w10, uxtw]', [9], 'w'), (f'ldr {w}0, [x9, w10, sxtw]', [9], 'w'),
          (f'ldr {w}0, [x9, w10, sxtw #{3 if w == "x" else 2}]', [9], 'w'),
          (f'str {w}1, [x9, x10]', [9], 'x'), (f'str {w}1, [x9, w10, sxtw #{3 if w == "x" else 2}]', [9], 'w'),
          (f'stur {w}1, [x9, #-5]', [9]), (f'ldur {w}0, [x9, #7]', [9])]
    for op in ('add', 'sub', 'adds', 'subs'):
        for ext in ('uxtb', 'uxth', 'sxtb', 'sxth', 'uxtw', 'sxtw'):
            T += [(f'{op} {w}0, {w}1, w2, {ext} #2', []), (f'{op} {w}0, {w}1, w2, {ext}', [])]
        if w == 'x':
            T += [(f'{op} x0, x1, x2, sxtx #1', []), (f'{op} x0, x1, x2, uxtx #4', [])]
    T += [(f'add {"sp" if w == "x" else "wsp"}, {"sp" if w == "x" else "wsp"}, w2, uxtb', [])]
T += [('ldrb w0, [x9, #3]', [9]), ('ldrh w0, [x9, #6]', [9]), ('ldrsb x0, [x9, #3]', [9]),
      ('ldrsb w0, [x9, #3]', [9]), ('ldrsh x0, [x9, #6]', [9]), ('ldrsh w0, [x9, #6]', [9]),
      ('ldrsw x0, [x9, #4]', [9]), ('strb w1, [x9, #5]', [9]), ('strh w1, [x9, #6]', [9]),
      ('ldrb w0, [x9, w10, uxtw]', [9], 'w'), ('ldrh w0, [x9, w10, uxtw #1]', [9], 'w'),
      ('ldrsb x0, [x9, x10]', [9], 'x'), ('ldrsh w0, [x9, w10, sxtw #1]', [9], 'w'),
      ('ldrsw x0, [x9, w10, sxtw #2]', [9], 'w'), ('strb w1, [x9, x10]', [9], 'x'),
      ('strh w1, [x9, w10, uxtw #1]', [9], 'w'), ('ldurb w0, [x9, #-1]', [9]),
      ('ldurh w0, [x9, #3]', [9]), ('ldursb x0, [x9, #-7]', [9]), ('ldursh w0, [x9, #9]', [9]),
      ('ldursw x0, [x9, #-12]', [9]), ('sturb w1, [x9, #-1]', [9]), ('sturh w1, [x9, #3]', [9]),
      ('stp x29, x30, [sp, #-16]!', []), ('ldp x29, x30, [sp], #16', []),
      ('adr x0, #0x40', []), ('adrp x0, #0x3000', []),
      ('b #0x40', []), ('bl #0x40', []), ('ret', []), ('br x5', []), ('blr x5', []),
      ('cbz x1, #0x20', []), ('cbnz w1, #0x20', []), ('tbz x1, #3, #0x20', []),
      ('tbnz w1, #31, #0x20', []), ('tbz x1, #63, #0x20', [])]
T += [(f'b.{c} #0x40', []) for c in ('eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc',
                                     'hi', 'ls', 'ge', 'lt', 'gt', 'le')]

# integer data processing (liftIntOp)
for w in ('x', 'w'):
    n = 64 if w == 'x' else 32
    T += [(f'bic {w}0, {w}1, {w}2', []), (f'bic {w}0, {w}1, {w}2, lsl #5', []),
          (f'bics {w}0, {w}1, {w}2, asr #3', []), (f'orn {w}0, {w}1, {w}2', []),
          (f'mvn {w}0, {w}1', []), (f'eon {w}0, {w}1, {w}2, ror #7', []),
          (f'lsl {w}0, {w}1, #5', []), (f'lsr {w}0, {w}1, #7', []), (f'asr {w}0, {w}1, #9', []),
          (f'ror {w}0, {w}1, #11', []), (f'ubfx {w}0, {w}1, #4, #8', []),
          (f'sbfx {w}0, {w}1, #3, #12', []), (f'ubfiz {w}0, {w}1, #6, #10', []),
          (f'sbfiz {w}0, {w}1, #2, #7', []), (f'bfi {w}0, {w}1, #5, #9', []),
          (f'bfxil {w}0, {w}1, #3, #6', []), (f'sxtb {w}0, w1', []), (f'sxth {w}0, w1', []),
          (f'uxtb w0, w1', []), (f'uxth w0, w1', []),
          (f'lsl {w}0, {w}1, {w}2', []), (f'lsr {w}0, {w}1, {w}2', []),
          (f'asr {w}0, {w}1, {w}2', []), (f'ror {w}0, {w}1, {w}2', []),
          (f'mul {w}0, {w}1, {w}2', []), (f'madd {w}0, {w}1, {w}2, {w}3', []),
          (f'msub {w}0, {w}1, {w}2, {w}3', []), (f'mneg {w}0, {w}1, {w}2', []),
          (f'udiv {w}0, {w}1, {w}2', []), (f'sdiv {w}0, {w}1, {w}2', []),
          (f'udiv {w}0, {w}1, {w}zr', []), (f'sdiv {w}0, {w}1, {w}zr', []),
          (f'extr {w}0, {w}1, {w}2, #13', []),
          (f'rev {w}0, {w}1', []), (f'rev16 {w}0, {w}1', [])]
    for cc in ('eq', 'ne', 'hs', 'lt', 'gt', 'hi', 'le'):
        T += [(f'csel {w}0, {w}1, {w}2, {cc}', []), (f'csinc {w}0, {w}1, {w}2, {cc}', []),
              (f'csinv {w}0, {w}1, {w}2, {cc}', []), (f'csneg {w}0, {w}1, {w}2, {cc}', []),
              (f'cset {w}0, {cc}', []), (f'csetm {w}0, {cc}', []),
              (f'ccmp {w}1, {w}2, #5, {cc}', []), (f'ccmp {w}1, #17, #10, {cc}', []),
              (f'ccmn {w}1, {w}2, #3, {cc}', []), (f'ccmn {w}1, #4, #12, {cc}', [])]
T += [('sxtw x0, w1', []), ('rev32 x0, x1', []), ('smaddl x0, w1, w2, x3', []),
      ('umaddl x0, w1, w2, x3', []), ('smsubl x0, w1, w2, x3', []), ('umsubl x0, w1, w2, x3', []),
      ('smull x0, w1, w2', []), ('umull x0, w1, w2', []), ('smulh x0, x1, x2', []),
      ('umulh x0, x1, x2', [])]

# register aliasing: Rd == Rn / Rd == Rm / Rn == Rm (flags must read the
# operands, not the result)
for w in ('x', 'w'):
    for op in ('adds', 'subs', 'add', 'sub'):
        T += [(f'{op} {w}1, {w}1, #1', []), (f'{op} {w}1, {w}1, {w}2', []),
              (f'{op} {w}2, {w}1, {w}2', []), (f'{op} {w}1, {w}1, {w}1', []),
              (f'{op} {w}1, {w}1, {w}2, lsl #3', [])]
    T += [(f'ands {w}1, {w}1, {w}2', []), (f'bics {w}1, {w}1, {w}2', []),
          (f'cmp {w}1, {w}1', []), (f'cmn {w}1, {w}1', []),
          (f'ccmp {w}1, {w}1, #5, ne', []), (f'csinc {w}1, {w}1, {w}1, eq', []),
          (f'madd {w}1, {w}1, {w}1, {w}1', []), (f'sdiv {w}1, {w}1, {w}2', []),
          (f'adds {w}1, {w}1, w2, sxtb #2', []), (f'subs {w}2, {w}1, w2, uxth', [])]

for shape in ('b','h','s','d','q'):
    size = {'b':1,'h':2,'s':4,'d':8,'q':16}[shape]
    for op in ('ldr','str'):
        T += [(f'{op} {shape}0, [x9, #{size}]',[9]),
              (f'{op} {shape}0, [x9, #-16]!',[9]),
              (f'{op} {shape}0, [x9], #16',[9]),
              (f'{op} {shape}0, [x9, x10, lsl #{size.bit_length()-1}]',[9],'x'),
              (f'{op} {shape}0, [x9, w10, sxtw #{size.bit_length()-1}]',[9],'w')]
    T += [(f'ldur {shape}0, [x9, #-7]',[9]), (f'stur {shape}0, [x9, #3]',[9])]
    if shape in ('s','d','q'):
        for op in ('ldp','stp'):
            T += [(f'{op} {shape}0, {shape}1, [x9, #{size*2}]',[9]),
                  (f'{op} {shape}0, {shape}1, [x9, #-32]!',[9]),
                  (f'{op} {shape}0, {shape}1, [x9], #32',[9])]
for shape, gpr in (('s','w'),('d','x')):
    T += [(f'fmov {shape}0, {gpr}1',[]),(f'fmov {gpr}0, {shape}1',[]),
          (f'fmov {shape}0, {shape}1',[]),(f'fmov {shape}0, {shape}0',[])]
    T += [(f'fmov {shape}0, #{value}',[]) for value in ('1.0','-2.0','0.125','31.0')]
T += [('fmov v0.d[1], x1',[]),('fmov x0, v1.d[1]',[])]
for bits, suffix in ((8,'b'),(16,'h'),(32,'s'),(64,'d')):
    gpr = 'x' if bits == 64 else 'w'
    for width in (64,128):
        if width == bits: continue
        shape = f'v0.{width//bits}{suffix}'
        T += [(f'dup {shape}, {gpr}1',[]), (f'dup {shape}, v1.{suffix}[1]',[])]
    T += [(f'ins v0.{suffix}[1], {gpr}1',[]), (f'ins v0.{suffix}[1], v1.{suffix}[0]',[]),
          (f'umov {gpr}0, v1.{suffix}[1]',[])]
for width in (8,16):
    T += [(f'movi v0.{width}b, #0x81',[])]
    for op in ('and','orr','eor','bic','orn'):
        T += [(f'{op} v0.{width}b, v1.{width}b, v2.{width}b',[]),
              (f'{op} v1.{width}b, v1.{width}b, v1.{width}b',[])]
    T += [(f'not v0.{width}b, v1.{width}b',[])]
for shape in ('4h','8h','2s','4s'):
    for op in ('movi','mvni'):
        T += [(f'{op} v0.{shape}, #0x81, lsl #8',[])]
for shape in ('2s','4s'):
    for op in ('movi','mvni'):
        for shift in (8,16): T += [(f'{op} v0.{shape}, #0x12, msl #{shift}',[])]
T += [('movi v0.2d, #0xff00ff00ff00ff00',[]),('movi d0, #0xff00ff00ff00ff00',[])]
for w in ('x','w'):
    T += [(f'{op} {w}0, {w}1',[]) for op in ('clz','rbit')]
    T += [(f'{op} {w}1, {w}1',[]) for op in ('clz','rbit')]
for shape in ('s','d'):
    for op in ('fadd','fsub','fmul','fdiv'):
        T += [(f'{op} {shape}0, {shape}1, {shape}2',[]),
              (f'{op} {shape}1, {shape}1, {shape}2',[])]
    for op in ('fneg','fabs','frintm','frintp','frintz','fsqrt'):
        T += [(f'{op} {shape}0, {shape}1',[])]
    for op in ('fcmp','fcmpe'):
        T += [(f'{op} {shape}0, {shape}1',[]),(f'{op} {shape}0, #0.0',[])]
    for condition in ('eq','ne','lt','le','gt','ge','vs','vc'):
        T += [(f'fcsel {shape}0, {shape}1, {shape}2, {condition}',[])]
    for gpr in ('x','w'):
        for op in ('scvtf','ucvtf'): T += [(f'{op} {shape}0, {gpr}1',[])]
        for op in ('fcvtzs','fcvtzu'): T += [(f'{op} {gpr}0, {shape}1',[])]
T += [('fcvt d0, s1',[]),('fcvt s0, d1',[])]

rng = random.Random(1234)
summary = defaultdict(list)
for item in T:
    text, mem = item[0], item[1]
    if _os.environ.get('ONLY') and _os.environ['ONLY'] not in text: continue
    idx = item[2] if len(item) > 2 else None
    kinds = defaultdict(int)
    first = None
    for trial in range(TRIALS):
        try:
            kind, info = run_case(text, rng, mem, idx, trial)
        except Exception as e:
            kinds['harness'] += 1
            print("HARNESS", text, e); break
        kinds[kind] += 1
        if kind == 'unlifted':
            break
        if kind == 'diff' and first is None:
            first = info
    if kinds['harness']:
        summary['harness'].append(text)
    elif kinds['diff']:
        diffs, regs, sp, nzcv = first
        summary['diff'].append(text)
        print(f'DIFF  {text:32s} {kinds["diff"]}/{sum(kinds.values())}  e.g. {"; ".join(diffs[:3])}')
        print(f'      inputs: x1={regs[1]:#x} x2={regs[2]:#x} nzcv={nzcv >> 28:04b}')
    elif kinds['unlifted']:
        summary['unlifted'].append(text)
    else:
        summary['ok'].append(text)
print(f"\n{len(summary['ok'])} ok, {len(summary['diff'])} diff, "
      f"{len(summary['unlifted'])} unlifted (reported in Block.unlifted): "
      + ', '.join(summary['unlifted']))
print(f"{len(summary['harness'])} harness errors")
if summary['harness'] or summary['unlifted'] or any(
        text not in ('bl #0x40','blr x5','ret') for text in summary['diff']):
    sys.exit(1)
