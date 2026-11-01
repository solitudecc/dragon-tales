import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, random, sys, re, subprocess, tempfile

import dragon
from collections import Counter
from slice_diff import rand_insn, R64
X = dragon.x86; A = dragon.Architecture.X86_64
asm = dragon.Assembler(A, dragon.Configuration()); dis = dragon.Disassembler(A, dragon.Configuration())
GPR = [X.RAX, X.RCX, X.RDX, X.RBX]
rng = random.Random(11); st = Counter(); d = tempfile.mkdtemp()
for n in range(int(os.environ.get('N', 150))):
    init = [rng.getrandbits(64) for _ in range(4)]
    text = [rand_insn(rng) for _ in range(rng.randrange(3, 9))]
    try: code = b''.join(bytes(asm.assemble(t)) for t in text)
    except RuntimeError: continue
    blk = dis.lift_trace(dis.disassemble(code, 0x1000).instructions())
    if blk.unlifted(): continue
    ex = dragon.SymbolicExecutor()
    for r, v in zip(GPR, init): ex.set_register(r, v)
    ex.run(blk); want = [ex.concrete_register(r) for r in GPR]
    g = dis.analyze(code + b'\xc3', 0x1000); L = dragon.LLVMLifter(A)
    if not L.lift(g.get_function(0x1000), g): st['lift fail'] += 1; continue
    ir = L.ir(); name = re.search(r'define void @"?([\w.]+)"?\(', ir).group(1)
    open(f'{d}/m.ll', 'w').write(ir)
    initc = ''.join(f'*(unsigned long long*)(regs+{o})=0x{v:x}ULL;' for o, v in zip((0, 8, 16, 24), init))
    open(f'{d}/drv.c', 'w').write(f'#include <stdio.h>\nvoid {name}(unsigned char*,unsigned char*);static unsigned char regs[4096],mem[1];int main(){{{initc}{name}(mem,regs);for(int i=0;i<4;i++)printf("%llx ",*(unsigned long long*)(regs+8*i));}}')
    subprocess.run(f'llc-21 -O0 -filetype=obj -mtriple=aarch64-linux-gnu {d}/m.ll -o {d}/m.o && gcc {d}/drv.c {d}/m.o -o {d}/a', shell=True, check=True)
    got = [int(x, 16) for x in subprocess.run(f'{d}/a', capture_output=True, text=True).stdout.split()]
    if got == want: st['match'] += 1
    else:
        st['WRONG'] += 1
        if st['WRONG'] <= 3: print('WRONG', '; '.join(text), [hex(x) for x in got], [hex(x) for x in want])
print(dict(st))
