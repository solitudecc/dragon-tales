"""Check that IGNIL simplify() and dead_store_eliminate() preserve semantics.

Random straight-line x86-64 blocks (instructions the Unicorn difftest already
validated) are lifted with lift_trace, optimised, and every version is run
through the SymbolicExecutor from the same random concrete state. A version is
wrong when a location the original leaves concrete ends up with a different
value, or symbolic. With live_out=None ("everything live") the whole state
must survive; with live_out=[RAX] only RAX and memory.
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, sys
from collections import Counter


import dragon

X = dragon.x86
ARCH = dragon.Architecture.X86_64
NAMES = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI',
         'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15']
GPR = [getattr(X, n) for n in NAMES]
FLAGS = [getattr(X, f) for f in ('CF', 'PF', 'AF', 'ZF', 'SF', 'OF')]
DATA, DATA_SIZE = 0x200000, 0x1000
BLOCKS = int(os.environ.get('BLOCKS', 300))

asm = dragon.Assembler(ARCH, dragon.Configuration())
dis = dragon.Disassembler(ARCH, dragon.Configuration())

R64 = ['rax', 'rcx', 'rdx', 'rdi', 'r8', 'r9']
R32 = ['eax', 'ecx', 'edx', 'edi', 'r8d', 'r9d']
R16 = ['ax', 'cx', 'dx', 'di']
R8 = ['al', 'cl', 'dl', 'ah', 'ch', 'dil']


def rand_insn(rng):
    kind = rng.randrange(14)
    regs = rng.choice([R64, R32, R16, R8]) if kind < 6 else rng.choice([R64, R32])
    a, b = rng.choice(regs), rng.choice(regs)
    imm = rng.choice([0, 1, 2, 3, 0x7f, 0xff, -1, -2, 0x55])
    if kind == 0:
        return f'{rng.choice(["add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"])} {a}, {b}'
    if kind == 1:
        return f'{rng.choice(["add", "sub", "and", "or", "xor", "cmp"])} {a}, {imm}'
    if kind == 2:
        return f'mov {a}, {b}'
    if kind == 3:
        return f'{rng.choice(["inc", "dec", "neg", "not"])} {a}'
    if kind == 4:
        return f'{rng.choice(["shl", "shr", "sar", "rol", "ror"])} {a}, {rng.randrange(1, 8)}'
    if kind == 5:
        return f'xchg {a}, {b}'
    if kind == 6:
        return f'mov {a}, {rng.choice([0, 1, 0x1234, 0x7fffffff])}'
    if kind == 7:
        return f'imul {a}, {b}'
    if kind == 8:
        return f'{rng.choice(["mov", "add", "xor"])} {a}, {"qword" if a in R64 else "dword"} ptr [rbx + {rng.randrange(0, 64, 8)}]'
    if kind == 9:
        return f'mov {"qword" if a in R64 else "dword"} ptr [rbx + {rng.randrange(0, 64, 8)}], {a}'
    if kind == 10:
        return f'{rng.choice(["cmove", "cmovb", "cmovl", "cmovs"])} {a}, {b}'
    if kind == 11:
        return f'lea {a}, [{rng.choice(R64)} + {rng.choice(R64)}*{rng.choice([1, 2, 4, 8])} + {imm}]' if a in R64 else f'mov {a}, {b}'
    if kind == 12:
        return f'set{rng.choice(["e", "b", "l", "a", "s"])} {rng.choice(R8)}'
    return f'{rng.choice(["clc", "stc", "cmc"])}'


def execute(blk, state):
    regs, flags, page = state
    ex = dragon.SymbolicExecutor()
    for r, v in zip(GPR, regs):
        ex.set_register(r, v)
    for f, v in zip(FLAGS, flags):
        ex.set_register(f, v)
    ex.set_register(X.DF, 0)
    ex.map_memory(DATA, page)
    ex.run(blk)
    out = {n: ex.concrete_register(r) for n, r in zip(NAMES, GPR)}
    out.update({f[2]: ex.concrete_register(f) for f in FLAGS})
    out.update({f'mem+{o:#x}': ex.concrete_memory(DATA + o, 64) for o in range(0, 128, 8)})
    return out


def compare(ref, got, keys):
    bad = []
    for k in keys:
        if ref[k] is not None and got[k] != ref[k]:
            bad.append(f'{k}: orig={ref[k]:#x} opt={got[k] if got[k] is None else hex(got[k])}')
    return bad


rng = random.Random(99)
stats = Counter()
shown = Counter()
for n in range(BLOCKS):
    text = [rand_insn(rng) for _ in range(rng.randrange(4, 11))]
    try:
        code = b''.join(bytes(asm.assemble(t)) for t in text)
    except RuntimeError:          # e.g. `mov ch, dil`: not encodable
        stats['not encodable'] += 1
        continue
    g = dis.disassemble(code, 0x100000)
    insns = g.instructions()
    if len(insns) != len(text):
        stats['asm-mismatch'] += 1
        continue
    orig = dis.lift_trace(insns)
    if orig.unlifted():
        stats['unlifted'] += 1
        continue
    simp, _ = orig.simplify()
    dse, removed = orig.dead_store_eliminate(ARCH)
    both, _ = simp.dead_store_eliminate(ARCH)
    dse_rax, _ = orig.dead_store_eliminate(ARCH, live_out=[X.RAX[:2]])
    stats['ops: original'] += orig.size
    stats['ops: after simplify+dse'] += both.size
    stats['ops: after dse(live=[RAX])'] += dse_rax.size
    if os.environ.get('CONTROL'):
        # Deliberately unsound: nothing live out, yet the full state compared.
        bogus, _ = orig.dead_store_eliminate(ARCH, live_out=[])
    versions = {'simplify': (simp, None), 'dse(all live)': (dse, None),
                'simplify+dse': (both, None), 'dse(live=[RAX])': (dse_rax, 'rax')}
    if os.environ.get('CONTROL'):
        versions['CONTROL dse(live=[]) vs full state'] = (bogus, None)
    for trial in range(4):
        regs = [rng.getrandbits(64) if rng.random() < 0.6 else rng.choice([0, 1, 0xff, 0xffffffff, 1 << 63])
                for _ in range(16)]
        regs[3] = DATA + 0x100          # rbx: memory base
        state = (regs, [rng.getrandbits(1) for _ in FLAGS], bytes(rng.getrandbits(8) for _ in range(DATA_SIZE)))
        ref = execute(orig, state)
        for name, (blk, live) in versions.items():
            keys = [k for k in ref if (live is None or k in ('RAX',) or k.startswith('mem'))]
            bad = compare(ref, execute(blk, state), keys)
            stats[f'{name} checked'] += 1
            if bad:
                stats[f'{name} WRONG'] += 1
                if shown[name] < 2:
                    shown[name] += 1
                    print(f'--- {name} differs from the original block:')
                    print('    ' + '; '.join(text))
                    print('    ' + '; '.join(bad[:4]))
for k in sorted(stats):
    print(f'{k:28s} {stats[k]}')
