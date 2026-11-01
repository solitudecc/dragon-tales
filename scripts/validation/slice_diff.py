import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, sys


import dragon
from collections import Counter
X = dragon.x86; A = dragon.Architecture.X86_64
asm = dragon.Assembler(A, dragon.Configuration())
dis = dragon.Disassembler(A, dragon.Configuration())
R64 = ['rax', 'rcx', 'rdx', 'rbx']; R32 = ['eax', 'ecx', 'edx', 'ebx']; R16 = ['ax', 'cx', 'dx', 'bx']; R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
READ = [X.RAX, X.RCX, X.RDX, X.RBX, X.EAX, X.AX, X.AH, X.CL, X.CH, X.DX]
def rand_insn(rng):
    regs = rng.choice([R64, R32, R16, R8]); a, b = rng.choice(regs), rng.choice(regs)
    k = rng.randrange(6)
    if k == 0: return f'mov {a}, {b}'
    if k == 1: return f'mov {a}, {rng.choice([0, 1, 0x7f, 0x55])}'
    if k == 2: return f'{rng.choice(["add", "sub", "and", "or", "xor"])} {a}, {b}'
    if k == 3: return f'{rng.choice(["add", "and", "or", "xor"])} {a}, {rng.choice([1, 3, 0x7f])}'
    if k == 4: return f'{rng.choice(["shl", "shr"])} {a}, {rng.randrange(1, 8)}'
    return f'{rng.choice(["not", "neg"])} {a}'
if __name__ == "__main__":
    rng = random.Random(7); st = Counter()
    for n in range(300):
        init = [rng.getrandbits(64) for _ in range(4)]
        text = [f'mov {r}, {v:#x}' for r, v in zip(R64, init)] + [rand_insn(rng) for _ in range(rng.randrange(3, 9))]
        try: code = b''.join(bytes(asm.assemble(t)) for t in text)
        except RuntimeError: st['not encodable'] += 1; continue
        ret = bytes(asm.assemble('ret'))
        blk = dis.lift_trace(dis.disassemble(code, 0x1000).instructions())
        ex = dragon.SymbolicExecutor(); ex.run(blk)
        g = dis.analyze(code + ret, 0x1000)
        for reg in READ:
            want = ex.concrete_register(reg)
            e = dragon.BackwardSlicer(5).slice_reg(g, 0x1000, *reg)
            got = e.solve_unique()
            if got is None: st['slice not unique'] += 1
            elif got == want: st['match'] += 1
            else:
                st['WRONG'] += 1
                if st['WRONG'] <= 3: print('WRONG', reg[2], hex(got), 'want', hex(want), '|', '; '.join(text[4:]))
    print(dict(st))
