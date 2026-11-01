"""Differential test: dragon x86-64 IGNIL lifter (run concretely through the
SymbolicExecutor) vs. Unicorn, one instruction at a time, random states.

Compares the 16 GPRs, CF/PF/AF/ZF/SF/OF and a data page; branches compare the
taken target. Flags the Intel SDM calls undefined for an instruction are not
compared (dragon may leave them UNDEF or compute anything).
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, re, sys
from collections import defaultdict


import dragon
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import *

CODE, DATA, DATA_SIZE = 0x100000, 0x200000, 0x2000
BASE = DATA + 0x1000
TRIALS = int(os.environ.get('TRIALS', 40))
ONLY = os.environ.get('ONLY')

X = dragon.x86
NAMES = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI',
         'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15']
GPR = [getattr(X, n) for n in NAMES]
UC_GPR = [globals()[f'UC_X86_REG_{n}'] for n in NAMES]
FLAGS = [('CF', 0), ('PF', 2), ('AF', 4), ('ZF', 6), ('SF', 7), ('OF', 11)]

UC = Uc(UC_ARCH_X86, UC_MODE_64)   # one instance, reset per trial
UC.mem_map(CODE, 0x1000)
UC.mem_map(DATA, DATA_SIZE)

asm = dragon.Assembler(dragon.Architecture.X86_64, dragon.Configuration())
dis = dragon.Disassembler(dragon.Architecture.X86_64, dragon.Configuration())
EDGES = [0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
         0xffffffff, 0x100000000, 0x7fffffffffffffff, 0x8000000000000000,
         0xffffffffffffffff, 0xfffffffffffffffe]


def rand64(rng):
    return rng.choice(EDGES) if rng.random() < 0.35 else rng.getrandbits(64)


def undefined_flags(text, regs):
    """Flags the SDM leaves undefined for this instruction + input state."""
    m = text.split()[0]
    cl = regs[1] & 0xff
    if m in ('and', 'or', 'xor', 'test'):
        return {'AF'}
    if m in ('shl', 'shr', 'sar', 'sal'):
        width = 64 if re.search(r'\br\w+,', text) or 'qword' in text else \
                32 if re.search(r'\be\w+,', text) else 16 if re.search(r'\b[a-d]x,', text) else 8
        if 'cl' in text.split(',')[-1]:
            cnt = cl & (0x3f if width == 64 else 0x1f)
        else:
            cnt = int(text.split(',')[-1], 0) & (0x3f if width == 64 else 0x1f)
        u = {'AF'}
        if cnt != 1:
            u.add('OF')
        if width < 32 and cnt >= width:   # 8/16-bit shift past the width: CF undefined
            u.add('CF')
        return u
    if m in ('rol', 'ror', 'rcl', 'rcr'):
        last = text.split(',')[-1].strip()
        cnt = (cl if last == 'cl' else int(last, 0)) & 0x3f
        return set() if cnt == 1 else {'OF'}
    if m in ('shld', 'shrd'):
        return {'AF', 'OF', 'CF', 'SF', 'ZF', 'PF'} if 'cl' in text else {'AF', 'OF'}
    if m in ('mul', 'imul'):
        return {'SF', 'ZF', 'AF', 'PF'}
    if m in ('div', 'idiv'):
        return {f for f, _ in FLAGS}
    if m in ('bsf', 'bsr'):
        return {'CF', 'OF', 'SF', 'AF', 'PF'}
    if m in ('bt', 'bts', 'btr', 'btc'):
        return {'OF', 'SF', 'AF', 'PF'}
    return set()


def run_case(text, rng, setup):
    code = bytes.fromhex(text.split('#')[1]) if '#' in text else bytes(asm.assemble(text))
    if not code:
        return 'asmfail', None
    g = dis.disassemble(code, CODE)
    insns = g.instructions()
    if len(insns) != 1:
        return 'asmfail', None
    blk = dis.lift_trace(insns)
    if blk.unlifted():
        return 'unlifted', None

    regs = [rand64(rng) for _ in range(16)]
    if re.match(r'sh[lr]d ax, bx, cl', text):
        regs[1] = rng.randrange(0, 17)   # count > 16: result undefined
    regs[3] = BASE + rng.randrange(-0x100, 0x100, 8)      # rbx: memory base
    regs[4] = BASE + 0x400 + rng.randrange(-0x80, 0x80, 8)  # rsp
    regs[5] = BASE + 0x200 + rng.randrange(-0x80, 0x80, 8)  # rbp
    regs[6] = rng.randrange(0, 0x40)                        # rsi: small index
    for k, v in setup.items():
        regs[NAMES.index(k)] = v(rng) if callable(v) else v
    eflags = 0x2 | sum(rng.getrandbits(1) << b for _, b in FLAGS)
    page = bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))

    uc = UC
    uc.mem_write(CODE, b'\xcc' * 0x1000)
    uc.mem_write(CODE, code)
    uc.mem_write(DATA, page)
    for r, v in zip(UC_GPR, regs):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, eflags)
    try:
        uc.emu_start(CODE, CODE + len(code), count=1)
    except UcError:
        return 'fault', None          # e.g. #DE on div: not a lifter question
    exp = [uc.reg_read(r) for r in UC_GPR]
    exp_fl = uc.reg_read(UC_X86_REG_EFLAGS)
    exp_rip = uc.reg_read(UC_X86_REG_RIP)
    exp_page = bytes(uc.mem_read(DATA, DATA_SIZE))

    ex = dragon.SymbolicExecutor()
    for reg, v in zip(GPR, regs):
        ex.set_register(reg, v)
    for f, b in FLAGS:
        ex.set_register(getattr(X, f), (eflags >> b) & 1)
    ex.set_register(X.DF, 0)
    ex.map_memory(DATA, page)
    res = ex.run(blk)

    diffs = []
    insn = insns[0]
    if insn.is_jump:
        taken = None
        for s in res.successors:
            if s.condition is None or ex.proves(s.condition):
                taken = s.address
        if taken != exp_rip:
            diffs.append(f'target dragon={hexn(taken)} unicorn={exp_rip:#x}')
    skip_rax = text.split()[0] in ('bsf', 'bsr') and regs[3 if 'rbx' in text else 2] == 0
    for i, reg in enumerate(GPR):
        if skip_rax and i == 0:
            continue
        got = ex.concrete_register(reg)
        if got != exp[i]:
            diffs.append(f'{NAMES[i]}: dragon={hexn(got)} unicorn={exp[i]:#x}')
    undef = undefined_flags(text, regs)
    for f, b in FLAGS:
        if f in undef:
            continue
        got = ex.concrete_register(getattr(X, f))
        want = (exp_fl >> b) & 1
        if got is None or (got & 1) != want:
            diffs.append(f'{f}: dragon={got} unicorn={want}')
    for off in range(0, DATA_SIZE, 8):
        got = ex.concrete_memory(DATA + off, 64)
        want = int.from_bytes(exp_page[off:off + 8], 'little')
        if got != want:
            diffs.append(f'mem[{DATA + off:#x}]: dragon={hexn(got)} unicorn={want:#x}')
            break
    return ('ok' if not diffs else 'diff'), (diffs, regs, eflags)


def hexn(v):
    return 'None' if v is None else hex(v)


small_cl = {'RCX': lambda r: r.randrange(0, 70)}
T = []
W = {64: ('rax', 'rbx', 'rdx', 'qword'), 32: ('eax', 'ebx', 'edx', 'dword'),
     16: ('ax', 'bx', 'dx', 'word'), 8: ('al', 'bl', 'dl', 'byte')}
for w, (a, b, d, p) in W.items():
    T += [(f'mov {a}, {b}', {}), (f'mov {a}, {p} ptr [rbx + 8]', {}),
          (f'mov {p} ptr [rbx + 8], {a}', {}), (f'mov {p} ptr [rbx + rsi*4 + 16], {a}', {}),
          (f'mov {a}, {p} ptr [rbx + rsi]', {})]
    imm = {64: '0x12345678', 32: '0x89abcdef', 16: '0x8001', 8: '0x81'}[w]
    T += [(f'mov {a}, {imm}', {}), (f'mov {p} ptr [rbx + 8], {imm}', {})]
    for op in ('add', 'sub', 'adc', 'sbb', 'cmp', 'and', 'or', 'xor', 'test'):
        T += [(f'{op} {a}, {b}', {}), (f'{op} {a}, {imm}', {}), (f'{op} {a}, 1', {}),
              (f'{op} {p} ptr [rbx + 8], {a}', {})]
        if op != 'test':
            T += [(f'{op} {a}, {p} ptr [rbx + 8]', {}), (f'{op} {p} ptr [rbx + 8], {imm}', {})]
    for op in ('inc', 'dec', 'neg', 'not'):
        T += [(f'{op} {a}', {}), (f'{op} {p} ptr [rbx + 8]', {})]
    for op in ('shl', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
        T += [(f'{op} {a}, 1', {}), (f'{op} {a}, 5', {}), (f'{op} {a}, {w - 1}', {}),
              (f'{op} {a}, cl', small_cl)]
    T += [(f'xchg {a}, {b}', {}), (f'xadd {a}, {b}', {})]
    if w > 8:
        T += [(f'imul {a}, {b}', {}), (f'imul {a}, {b}, 7', {}), (f'imul {a}, {b}, -3', {}),
              (f'shld {a}, {b}, 3', {}), (f'shrd {a}, {b}, 3', {}),
              (f'shld {a}, {b}, cl', small_cl), (f'shrd {a}, {b}, cl', small_cl),
              (f'bsf {a}, {b}', {}), (f'bsr {a}, {b}', {}), (f'bt {a}, {b}', {}),
              (f'bt {a}, 5', {}), (f'bts {a}, 5', {}), (f'btr {a}, {b}', {}),
              (f'btc {a}, 7', {}), (f'cmove {a}, {b}', {}), (f'cmovl {a}, {b}', {}),
              (f'cmova {a}, {b}', {}), (f'cmovs {a}, {p} ptr [rbx + 8]', {})]
    T += [(f'mul {b}', {}), (f'imul {b}', {}),
          (f'div {b}', {'RDX': 0, 'RBX': lambda r: r.getrandbits(32) | 1,
                        **({'RAX': lambda r: r.getrandbits(7)} if w == 8 else {})}),
          (f'idiv {b}', {'RDX': 0, 'RAX': lambda r: r.getrandbits(6 if w == 8 else 30),
                         'RBX': lambda r: r.getrandbits(16) | 1})]
T += [('mov ah, bl', {}), ('mov al, bh', {}), ('add ah, al', {}), ('mov rax, 0x123456789abcdef0', {}),
      ('movzx eax, bl', {}), ('movzx eax, bx', {}), ('movzx rax, byte ptr [rbx + 1]', {}),
      ('movsx rax, bl', {}), ('movsx eax, bx', {}), ('movsxd rax, ebx', {}),
      ('movsx eax, byte ptr [rbx + 3]', {}), ('movzx eax, ah', {}),
      ('lea rax, [rbx + rsi*4 + 0x10]', {}), ('lea eax, [rbx + rsi]', {}), ('lea rax, [rip + 0x40]', {}),
      ('cbw', {}), ('cwde', {}), ('cdqe', {}), ('cwd', {}), ('cdq', {}), ('cqo', {}),
      ('bswap eax', {}), ('bswap rax', {}), ('lahf', {}), ('sahf', {}),
      ('clc', {}), ('stc', {}), ('cmc', {}),
      ('push rax', {}), ('pop rax', {}), ('push 0x7f', {}), ('push -2', {}),
      ('push qword ptr [rbx + 8]', {}), ('pop qword ptr [rbx + 8]', {}),
      ('mov eax, [moffs64] #a11010200000000000', {}), ('mov rax, [moffs64] #48a11010200000000000', {}),
      ('mov al, [moffs64] #a01010200000000000', {}), ('mov ax, [moffs64] #66a11010200000000000', {}),
      ('mov [moffs64], eax #a31010200000000000', {}), ('mov [moffs64], rax #48a31010200000000000', {}),
      ('mov [moffs64], al #a21010200000000000', {}),
      ('pushfq', {}), ('popfq', {'RSP': BASE + 0x400}), ('leave', {}),
      ('sete al', {}), ('setb bl', {}), ('setg al', {}), ('setle byte ptr [rbx + 4]', {}),
      ('setp al', {}), ('seto al', {}), ('seta al', {}),
      ('jmp rax', {'RAX': CODE + 0x200}), ('jmp qword ptr [rbx + 8]', {}),
      ('loop #e23e', {'RCX': lambda r: r.choice([0, 1, 2, 3])}),
      ('jrcxz #e33e', {'RCX': lambda r: r.choice([0, 1, 2])}),
      ('jecxz #67e33e', {'RCX': lambda r: r.choice([0, 1 << 32, 1 << 33 | 1, 2])}),
      ('loope #e13e', {'RCX': lambda r: r.choice([0, 1, 2, 3])}),
      ('loopne #e03e', {'RCX': lambda r: r.choice([0, 1, 2, 3])})]
# short jcc, rel8 0x3e -> CODE + 2 + 0x3e = CODE + 0x40
T += [(f'j{c} #{0x70 + i:02x}3e', {}) for i, c in enumerate(
        ('o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g'))]

# register aliasing
T += [(f'{op} {r}, {r}', {}) for op in ('add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'cmp', 'test')
      for r in ('rax', 'eax', 'ax', 'al', 'ah')]
T += [('imul rax, rax', {}), ('imul ax, ax', {}), ('xadd rax, rax', {}), ('xchg rax, rax', {}), ('shld rax, rax, 3', {}), ('cmovs rax, rax', {}),
      ('neg rax', {}), ('bswap rax', {}), ('mul rax', {}), ('imul rax', {})]

rng = random.Random(4321)
summary = defaultdict(list)
import gc
for n_case, (text, setup) in enumerate(T):
    if ONLY and ONLY not in text:
        continue
    if n_case % 20 == 0:
        gc.collect()
    kinds = defaultdict(int)
    first = None
    for _ in range(TRIALS):
        try:
            kind, info = run_case(text, rng, setup)
        except Exception as e:  # noqa: BLE001
            kind, info = 'error', ([f'{type(e).__name__}: {e}'], [0] * 16, 0)
        kinds[kind] += 1
        if kind in ('unlifted', 'asmfail'):
            break
        if kind in ('diff', 'error') and first is None:
            first = info
    if kinds['diff'] or kinds['error']:
        diffs, regs, eflags = first
        summary['diff'].append(text)
        print(f'DIFF  {text:36s} {kinds["diff"] + kinds["error"]}/{sum(kinds.values())}  '
              f'e.g. {"; ".join(diffs[:3])}')
        print(f'      inputs: rax={regs[0]:#x} rbx={regs[3]:#x} rcx={regs[1]:#x} '
              f'rdx={regs[2]:#x} eflags={eflags:#x}')
    elif kinds['unlifted']:
        summary['unlifted'].append(text)
    elif kinds['asmfail']:
        summary['asmfail'].append(text)
    else:
        summary['ok'].append(text)
print(f"\n{len(summary['ok'])} ok, {len(summary['diff'])} diff, "
      f"{len(summary['unlifted'])} unlifted, {len(summary['asmfail'])} asm-fail")
print('unlifted:', ', '.join(summary['unlifted']))
print('asm-fail:', ', '.join(summary['asmfail']))
