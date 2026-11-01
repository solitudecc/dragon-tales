"""Guard/index differential checks against Unicorn, including wider views.

Every accepted CFG target set must contain every concrete guarded target.
Positive prefix guards must resolve; non-prefix/vacuous guards must stay
unresolved. Each table entry has a different target so a missing case cannot
hide behind duplicated entries. Only known machine-code patterns are used.
"""
import os
import struct
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))
import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RAX, UC_X86_REG_RDI, UC_X86_REG_RSP

BASE, TABLE, CASES, DEFAULT, STACK, STOP = 0x1000, 0x3000, 0x1800, 0x1400, 0x4800, 0x9000
arch = dragon.Architecture.X86_64
cfg = dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL)
assembler = dragon.Assembler(arch, cfg)
disassembler = dragon.Disassembler(arch, cfg)


def fixture(bits, mode):
    reg = {8: "dil", 16: "di", 32: "edi"}[bits]
    lines = []
    if mode == "vacuous_mask":
        lines += ["and edi, 1"]
    if mode == "unsigned_prefix":
        lines += [f"cmp {reg}, 2", "setbe al"]
    elif mode == "unsigned_large_prefix":
        lines += [f"cmp {reg}, 33", "setb al"]
    elif mode == "signed_prefix":
        lines += [f"cmp {reg}, 2", "setle al"]
    elif mode in ("outside_hole", "inside_hole", "xor_hole", "redundant_prefix"):
        bound = 2 if mode in ("outside_hole", "redundant_prefix") else 32
        lines += [f"cmp {reg}, {bound}", "setbe al"]
        k = 2 if mode == "redundant_prefix" else 17
        lines += [f"cmp {reg}, {k}",
                  "setne cl" if mode == "inside_hole" else "sete cl"]
        op = {"outside_hole": "or", "inside_hole": "and",
              "xor_hole": "xor", "redundant_prefix": "or"}[mode]
        lines += [f"{op} al, cl"]
    elif mode == "not_equal":
        lines += [f"cmp {reg}, 17", "setne al"]
    elif mode == "vacuous_mask":
        lines += [f"cmp {reg}, 7", "setbe al"]
    else:
        raise ValueError(mode)
    lines += ["test al, al"]
    prefix = b"".join(bytes(assembler.assemble(line)) for line in lines)
    dispatch = BASE + len(prefix) + 6
    code = prefix + b"\x0f\x84" + struct.pack("<i", DEFAULT-dispatch)
    code += b"\x40\x0f\xb6\xc7\xff\x24\xc5" + struct.pack("<I", TABLE)
    image = bytearray(b"\xcc" * (TABLE-BASE+256*8))
    image[:len(code)] = code
    for i in range(256):
        target = CASES+i*8
        image[target-BASE:target-BASE+6] = b"\xb8"+struct.pack("<I", i)+b"\xc3"
        struct.pack_into("<Q", image, TABLE-BASE+i*8, target)
    image[DEFAULT-BASE:DEFAULT-BASE+6] = b"\xb8\xff\xff\xff\xff\xc3"
    return bytes(image), dispatch


def main():
    executions = graphs = 0
    modes = ("unsigned_prefix", "unsigned_large_prefix", "signed_prefix",
             "outside_hole", "inside_hole", "xor_hole", "redundant_prefix",
             "not_equal", "vacuous_mask")
    for bits in (8, 16, 32):
        upper = (0,) if bits == 8 else (0, 0x100, 1 << (bits-1), ((1 << bits)-1) ^ 255)
        for mode in modes:
            image, dispatch = fixture(bits, mode)
            graph = disassembler.analyze(image, BASE)
            targets = set(graph.get_block(dispatch).successors())
            positive = mode in ("unsigned_prefix", "unsigned_large_prefix", "redundant_prefix")
            assert bool(targets) == positive, (bits, mode, "resolution", len(targets))
            emulator = Uc(UC_ARCH_X86, UC_MODE_64)
            emulator.mem_map(BASE, 0x4000)
            emulator.mem_write(BASE, image)
            emulator.mem_write(STACK, struct.pack("<Q", STOP))
            for high in upper:
                for low in range(256):
                    value = high | low
                    emulator.reg_write(UC_X86_REG_RDI, value)
                    emulator.reg_write(UC_X86_REG_RSP, STACK)
                    emulator.emu_start(BASE, STOP, count=100)
                    result = emulator.reg_read(UC_X86_REG_RAX)
                    if result != 0xffffffff and targets:
                        assert CASES+result*8 in targets, (bits, mode, value, result)
                    executions += 1
            graphs += 1
    print(f"guard truth: {graphs} CFGs, {executions} Unicorn executions, 0 wrong target sets")


if __name__ == "__main__":
    main()
