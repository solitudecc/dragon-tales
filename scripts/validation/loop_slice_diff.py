"""Check nested-join loop slices against concrete Unicorn executions.

Four loop bodies preserve RDX, overwrite it on one arm, increment it on one
arm, or exchange values through RAX. Every uniquely solved slice must agree
with all executions; the invariant body must also resolve successfully.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))

import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RDX, UC_X86_REG_RDI, UC_X86_REG_RIP


BASE = 0x1000
arch = dragon.Architecture.X86_64
assembler = dragon.Assembler(arch, dragon.Configuration())
disassembler = dragon.Disassembler(
    arch, dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL))


def program(variant, iterations):
    code = bytearray(b"\xcc" * 0x200)

    def emit(at, *lines):
        for line in lines:
            data = bytes(assembler.assemble(line))
            code[at - BASE:at - BASE + len(data)] = data
            at += len(data)
        return at

    def branch(at, target, conditional=False):
        prefix = b"\x0f\x85" if conditional else b"\xe9"
        size = len(prefix) + 4
        data = prefix + (target - at - size).to_bytes(4, "little", signed=True)
        code[at - BASE:at - BASE + size] = data
        return at + size

    at = emit(BASE, "mov rdx, 0x9000", "mov rax, 0x8000",
              f"mov ecx, {iterations}")
    branch(at, 0x1040)
    at = emit(0x1040, "xor edi, ecx", "test edi, 1")
    at = branch(at, 0x1080, conditional=True)
    branch(at, 0x10c0)
    at = emit(0x1080, *(["mov rdx, rax"] if variant == 3 else []))
    branch(at, 0x1100)
    body = {0: [], 1: ["mov rdx, 0x9001"], 2: ["add rdx, 1"], 3: []}[variant]
    at = emit(0x10c0, *body)
    branch(at, 0x1100)
    at = emit(0x1100, *(["mov rax, rdx"] if variant == 3 else []), "dec ecx")
    exit_address = branch(at, 0x1040, conditional=True)
    emit(exit_address, "ret")
    return bytes(code), exit_address


def main():
    checks = resolved = 0
    for variant in range(4):
        for iterations in (1, 2, 3, 5):
            code, exit_address = program(variant, iterations)
            graph = disassembler.analyze(code, BASE)
            sliced = dragon.BackwardSlicer(32).slice_reg(
                graph, exit_address, *dragon.x86.RDX).solve_unique()
            truth = set()
            for seed in range(8):
                emulator = Uc(UC_ARCH_X86, UC_MODE_64)
                emulator.mem_map(BASE, 0x1000)
                emulator.mem_write(BASE, code)
                emulator.reg_write(UC_X86_REG_RDI, seed)
                emulator.emu_start(BASE, exit_address, count=1000)
                assert emulator.reg_read(UC_X86_REG_RIP) == exit_address
                value = emulator.reg_read(UC_X86_REG_RDX)
                truth.add(value)
                assert sliced is None or sliced == value, (
                    variant, iterations, seed, sliced, value)
                checks += 1
            if variant == 0:
                assert sliced is not None, "lost loop invariant"
            if len(truth) > 1:
                assert sliced is None, "merged differing loop values"
            resolved += sliced is not None
    print(f"nested loops: {checks} Unicorn executions; "
          f"{resolved} unique slices; 0 wrong")


if __name__ == "__main__":
    main()
