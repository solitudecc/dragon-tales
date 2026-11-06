#!/usr/bin/env python3
"""Independent x86-64 segment-address oracle; no Dragon dependency.

The C++ regressions validate honest unsupported lowering. This script derives
the actual hardware-reference outcomes for nonzero FS/GS bases and positive
ignored-prefix cases. It is not a whole-program or full-lifter equivalence test.
All cases execute; errors fail the process. No exclusions or normalizations.
"""
import argparse
import json
from pathlib import Path

import unicorn
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RBP, UC_X86_REG_RCX,
    UC_X86_REG_RDX, UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RSP,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS, UC_X86_REG_FS_BASE, UC_X86_REG_GS_BASE,
)

MASK = (1 << 64) - 1
CODE, FLAT, SEGBASE, STACK = 0x1000, 0x3000, 0x4000, 0x9800
EDGE = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
        0x7fffffffffffffff, 0x8000000000000000, MASK]
REGS = {"rax": UC_X86_REG_RAX, "rbx": UC_X86_REG_RBX,
        "rbp": UC_X86_REG_RBP, "rcx": UC_X86_REG_RCX,
        "rdx": UC_X86_REG_RDX, "rsi": UC_X86_REG_RSI,
        "rdi": UC_X86_REG_RDI, "rsp": UC_X86_REG_RSP,
        "rip": UC_X86_REG_RIP, "eflags": UC_X86_REG_EFLAGS}


def machine(code, value, address=FLAT):
    u = Uc(UC_ARCH_X86, UC_MODE_64)
    for start in (CODE, FLAT, FLAT + SEGBASE, 0x9000):
        u.mem_map(start, 0x1000)
    u.mem_write(CODE, code)
    initial = dict(rax=value, rbx=address, rbp=0x1040, rcx=3,
                   rdx=0xabcdef0123456789, rsi=address, rdi=address,
                   rsp=STACK, rip=CODE, eflags=2)
    for name, val in initial.items():
        u.reg_write(REGS[name], val)
    u.reg_write(UC_X86_REG_FS_BASE, SEGBASE)
    u.reg_write(UC_X86_REG_GS_BASE, SEGBASE)
    return u, initial


def run_one(u, code, *, transfer=False):
    u.emu_start(CODE, CODE + len(code), count=1)
    if not transfer:
        assert u.reg_read(UC_X86_REG_RIP) == CODE + len(code), "instruction did not finish"


def read64(u, addr):
    return int.from_bytes(u.mem_read(addr, 8), "little")


def write64(u, addr, value):
    u.mem_write(addr, (value & MASK).to_bytes(8, "little"))


def add_flags(a, b):
    result = (a + b) & MASK
    cf = int(a + b > MASK)
    pf = int((result & 0xff).bit_count() % 2 == 0)
    af = int(bool((a ^ b ^ result) & 0x10))
    zf, sf = int(result == 0), result >> 63
    of = int(bool((~(a ^ b) & (a ^ result)) >> 63 & 1))
    return 2 | cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (of << 11)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records, errors, controls, dropped_load_controls = [], [], [], []

    def expect(name, code, value, actual, expected, flattened=None, **details):
        okay = actual == expected
        entry = dict(name=name, code=code.hex(), input=hex(value),
                     actual=hex(actual), expected=hex(expected), match=okay,
                     **details)
        if flattened is not None:
            entry["flattened_control"] = hex(flattened)
            entry["flattened_control_detected"] = actual != flattened
            controls.append(actual != flattened)
        records.append(entry)
        if not okay:
            errors.append(entry)

    # Eight value boundaries, and both an ordinary address and last valid
    # qword of a mapped page. The flat and segmented pages carry distinct data.
    for value in EDGE:
        other = value ^ 0x9e3779b97f4a7c15
        for address in (FLAT, FLAT + 0xff8):
            for prefix in (0x64, 0x65):
                for op in ("load", "store", "add"):
                    body = {"load": b"\x48\x8b\x03", "store": b"\x48\x89\x03",
                            "add": b"\x48\x01\x03"}[op]
                    code = bytes([prefix]) + body
                    u, initial = machine(code, value, address)
                    if op == "load":
                        # Neither a flat load nor a dropped load may accidentally
                        # retain the expected value from the input accumulator.
                        u.reg_write(UC_X86_REG_RAX, value ^ 0x13579bdf2468ace0)
                    write64(u, address, other)
                    write64(u, address + SEGBASE, value if op == "load" else other)
                    before_flat = read64(u, address)
                    run_one(u, code)
                    if op == "load":
                        dropped_load_controls.append(
                            u.reg_read(UC_X86_REG_RAX) != value ^ 0x13579bdf2468ace0)
                        expect(f"{prefix:02x}-load", code, value,
                               u.reg_read(UC_X86_REG_RAX), value, other,
                               address=hex(address))
                    else:
                        expected = value if op == "store" else (other + value) & MASK
                        expect(f"{prefix:02x}-{op}-segmented-memory", code, value,
                               read64(u, address + SEGBASE), expected,
                               other, address=hex(address))
                    expect(f"{prefix:02x}-{op}-flat-memory-unchanged", code, value,
                           read64(u, address), before_flat, address=hex(address))
                    if op == "add":
                        expect(f"{prefix:02x}-add-flags", code, value,
                               u.reg_read(UC_X86_REG_EFLAGS), add_flags(other, value),
                               address=hex(address))

        # LEA neither accesses guest memory nor adds a segment base. Address
        # size 32 wraps before the resulting zero-extension to RAX.
        for prefix in (0x64, 0x65):
            for addr32 in (False, True):
                code = bytes([prefix]) + (b"\x67" if addr32 else b"") + b"\x48\x8d\x43\x65"
                u, initial = machine(code, value)
                u.reg_write(UC_X86_REG_RBX, value)
                run_one(u, code)
                expected = (value + 0x65) & (0xffffffff if addr32 else MASK)
                expect(f"{prefix:02x}-lea-addr{32 if addr32 else 64}", code, value,
                       u.reg_read(UC_X86_REG_RAX), expected)
        for prefix in (0x26, 0x2e, 0x36, 0x3e):
            code = bytes([prefix]) + b"\x48\x8b\x03"
            u, initial = machine(code, value)
            u.reg_write(UC_X86_REG_RAX, value ^ 0x13579bdf2468ace0)
            write64(u, FLAT, value)
            write64(u, FLAT + SEGBASE, other)
            run_one(u, code)
            dropped_load_controls.append(
                u.reg_read(UC_X86_REG_RAX) != value ^ 0x13579bdf2468ace0)
            expect(f"{prefix:02x}-ignored-long-mode-base", code, value,
                   u.reg_read(UC_X86_REG_RAX), value)

        # FS/GS do not redirect fixed-ES STOS or implicit-SS PUSH/POP. The
        # positive cases prevent a broad scan of raw prefix bytes from making
        # otherwise ordinary semantics unavailable.
        for prefix in (0x64, 0x65):
            for body, name in ((b"\x48\xab", "stos"), (b"\x50", "push"),
                               (b"\x58", "pop")):
                code = bytes([prefix]) + body
                u, initial = machine(code, value)
                write64(u, FLAT + SEGBASE, other)
                write64(u, STACK, other)
                run_one(u, code)
                if name == "stos":
                    expect(f"{prefix:02x}-fixed-es-stos", code, value, read64(u, FLAT), value)
                    expect(f"{prefix:02x}-stos-segment-memory-unchanged", code, value,
                           read64(u, FLAT + SEGBASE), other)
                elif name == "push":
                    expect(f"{prefix:02x}-implicit-ss-push", code, value,
                           read64(u, STACK - 8), value)
                    expect(f"{prefix:02x}-push-sp", code, value,
                           u.reg_read(UC_X86_REG_RSP), STACK - 8)
                else:
                    expect(f"{prefix:02x}-implicit-ss-pop", code, value,
                           u.reg_read(UC_X86_REG_RAX), other)
                    expect(f"{prefix:02x}-pop-sp", code, value,
                           u.reg_read(UC_X86_REG_RSP), STACK + 8)

    # Control-flow target loads use the segment base, while their machine CALL
    # return slot still uses SS. Different mapped code targets avoid exceptions.
    for prefix in (0x64, 0x65):
        for call in (False, True):
            code = bytes([prefix]) + (b"\xff\x13" if call else b"\xff\x23")
            u, initial = machine(code, 0)
            write64(u, FLAT, 0x1800)
            write64(u, FLAT + SEGBASE, 0x1900)
            run_one(u, code, transfer=True)
            expect(f"{prefix:02x}-memory-{'call' if call else 'jmp'}", code, 0,
                   u.reg_read(UC_X86_REG_RIP), 0x1900, 0x1800)
            if call:
                expect(f"{prefix:02x}-memory-call-return-slot", code, 0,
                       read64(u, STACK - 8), CODE + len(code))

    keys = {(r['name'], r['code'], r['input'], r.get('address')) for r in records}
    assert len(records) == len(keys) == 390, "incomplete or duplicate oracle checks"
    assert len(controls) == 100 and len(dropped_load_controls) == 64, "incomplete control plan"
    result = dict(unicorn_version=unicorn.__version__,
                  purpose="independent segment-address and ignored-prefix oracle; not Dragon execution",
                  records=records, comparisons=len(records), failures=len(errors),
                  skipped=0, normalized=0, exclusions=[],
                  flattened_controls=len(controls),
                  flattened_controls_detected=sum(controls),
                  flattened_controls_equal=len(controls)-sum(controls),
                  dropped_load_controls=len(dropped_load_controls),
                  dropped_load_controls_detected=sum(dropped_load_controls), errors=errors)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: val for key, val in result.items() if key not in ("records", "errors")}))
    if errors or not all(dropped_load_controls):
        raise SystemExit(1)
    if not all(controls):
        # A zero ADD intentionally leaves the segmented value unchanged, so
        # its data-only flattened control may agree. Other checks still test
        # memory address selection. Count these rather than hiding them.
        print("Some value-only controls agree; see explicit flattened_controls_equal count.")


if __name__ == "__main__":
    main()
