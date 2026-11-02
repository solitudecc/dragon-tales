"""Store-to-load slices vs Unicorn, including aliases and stack changes.

Only uniquely solved slices make claims. Every such claim must match the
machine's final RAX; exact and disjoint-slot cases must resolve as well.
"""
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RAX, UC_X86_REG_RDI, UC_X86_REG_RSP

BASE, STACK, EXIT = 0x1000, 0x8000, 0x2000
rng = random.Random(67)
assembler = dragon.Assembler()
disassembler = dragon.Disassembler()
resolved = checks = 0
for variant in range(7):
    for trial in range(64):
        value = rng.getrandbits(64)
        offset = rng.randrange(0, 16) * 8
        lines = [f'mov rax, {value:#x}', f'mov qword ptr [rsp+{offset}], rax',
                 'mov rax, 0']
        if variant == 1:
            lines += [f'mov qword ptr [rsp+{offset+8}], 7']
        elif variant == 2:
            lines += [f'mov byte ptr [rsp+{offset+3}], 7']
        elif variant == 3:
            lines += ['mov qword ptr [rdi], 7']
        elif variant == 4:
            lines += ['add rsp, 8']
        elif variant == 5:
            lines += [f'mov qword ptr [rsp+{offset}], 7']
        elif variant == 6:
            lines += [f'mov dword ptr [rsp+{offset}], 7']
        lines += [f'mov rax, qword ptr [rsp+{offset}]']
        code = b''.join(bytes(assembler.assemble(line)) for line in lines)
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        uc.mem_map(BASE, 0x2000)
        uc.mem_map(STACK, 0x2000)
        uc.mem_write(BASE, code)
        uc.mem_write(STACK, bytes(rng.getrandbits(8) for _ in range(0x2000)))
        uc.reg_write(UC_X86_REG_RSP, STACK+0x800)
        uc.reg_write(UC_X86_REG_RDI, STACK+0x800+offset+(trial % 3))
        uc.emu_start(BASE, BASE+len(code))
        expected = uc.reg_read(UC_X86_REG_RAX)
        with disassembler.analyze(code+b'\xc3', BASE) as graph:
            with dragon.BackwardSlicer(8) as slicer:
                with slicer.slice_reg(graph, BASE, *dragon.x86.RAX) as expr:
                    actual = expr.solve_unique()
        if variant in (0,1,5):
            assert actual is not None, (variant, lines)
        if actual is not None:
            assert actual == expected, (variant, lines, hex(actual), hex(expected))
            resolved += 1
        checks += 1
print(f'memory slices: {checks} Unicorn executions; {resolved} unique slices; 0 wrong')
