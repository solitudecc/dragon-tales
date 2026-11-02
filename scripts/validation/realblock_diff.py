"""Differential test of real basic blocks against Unicorn.

Samples basic blocks of a binary (x86-64, i386 or AArch64), drops the
terminating control transfer, skips blocks with calls (IGNIL CALL does not
push), lifts the rest with lift_trace and runs it from random register states
in Unicorn and in SymbolicExecutor. Memory is mapped on demand: image bytes
where the address is in the image, deterministic pseudo-random bytes
elsewhere; the executor gets exactly the pages Unicorn touched.

A difference is reported only when dragon gives a concrete value that differs
from Unicorn's; an unknown (UNDEF, unlifted, NaN) is not a difference.

Known, deliberate difference: after an x86 shift by more than one, OF is
architecturally undefined; dragon models it like a 1-bit shift (see
emitGatedShiftFlags - VMProtect folds RFLAGS into its state), Unicorn
clears it.

REALBLOCK_OPT=1 runs simplify() + dead_store_eliminate() on each block first.

Usage: realblock_diff.py BINARY [BLOCKS] [TRIALS]
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
import random
import sys
from collections import Counter

import dragon
from unicorn import Uc, UcError, UC_ARCH_X86, UC_ARCH_ARM64, UC_MODE_32, UC_MODE_64, \
    UC_MODE_ARM, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, \
    UC_HOOK_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_WRITE
from unicorn import x86_const as X86C, arm64_const as A64C

path = sys.argv[1]
NBLOCKS = int(sys.argv[2]) if len(sys.argv) > 2 else 400
TRIALS = int(sys.argv[3]) if len(sys.argv) > 3 else 6
PAGE = 0x1000

img = dragon.LoadedImage.load_file(path)
arch = img.architecture
A = dragon.Architecture
if arch == A.X86_64:
    X = dragon.x86
    names = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI'] + [f'R{i}' for i in range(8, 16)]
    regs = [(n, getattr(X, n), getattr(X86C, f'UC_X86_REG_{n}')) for n in names]
    flags = [(f, getattr(X, f), b) for f, b in (('CF', 0), ('PF', 2), ('AF', 4), ('ZF', 6), ('SF', 7), ('OF', 11))]
    uc_mode, uc_arch, sp_name, flag_reg, width = UC_MODE_64, UC_ARCH_X86, 'RSP', X86C.UC_X86_REG_EFLAGS, 64
elif arch == A.X86_32:
    X = dragon.x86
    names = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']
    regs = [(n, getattr(X, n), getattr(X86C, f'UC_X86_REG_{n}')) for n in names]
    flags = [(f, getattr(X, f), b) for f, b in (('CF', 0), ('PF', 2), ('AF', 4), ('ZF', 6), ('SF', 7), ('OF', 11))]
    uc_mode, uc_arch, sp_name, flag_reg, width = UC_MODE_32, UC_ARCH_X86, 'ESP', X86C.UC_X86_REG_EFLAGS, 32
elif arch == A.ARM64:
    R = dragon.arm
    regs = [(f'X{i}', getattr(R, f'X{i}'), getattr(A64C, f'UC_ARM64_REG_X{i}')) for i in range(31)]
    regs.append(('SP', R.SP, A64C.UC_ARM64_REG_SP))
    flags = [(f, getattr(R, f), b) for f, b in (('NF', 31), ('ZF', 30), ('CF', 29), ('VF', 28))]
    uc_mode, uc_arch, sp_name, flag_reg, width = UC_MODE_ARM, UC_ARCH_ARM64, 'SP', A64C.UC_ARM64_REG_NZCV, 64
else:
    print('unsupported architecture'); sys.exit(0)

# XMM registers (x86): seeded with random 128-bit values on both sides and
# compared as their two 64-bit halves (XMMn = bits 63:0, XMMnH = 127:64).
xmms = []
if arch in (A.X86_64, A.X86_32):
    xmms = [(i, getattr(dragon.x86, f'XMM{i}'), getattr(dragon.x86, f'XMM{i}H'),
             getattr(X86C, f'UC_X86_REG_XMM{i}')) for i in range(16 if arch == A.X86_64 else 8)]
def xmm_values(seed):
    xr = random.Random(seed)
    half = lambda: xr.getrandbits(46) if xr.random() < 0.5 else xr.getrandbits(64)
    return [(half(), half()) for _ in xmms]
MASK = (1 << width) - 1
HEAP = 0x10000000 if width == 32 else 0x7f0000000000
STACK = 0x20000000 if width == 32 else 0x7ff000000000

# Only sections in the address space (an ELF section without SHF_ALLOC sits at
# address 0 and is not readable).
sections = [(s.address, s.virtual_size, s.data) for s in img.sections() if s.readable or s.executable]
def page_content(base):
    # Every 4-byte word below 2^15: any 4-aligned 8-byte load assembled from
    # this memory stays below 2^47, the range Unicorn addresses faithfully
    # (above it, it truncates where hardware would fault).
    pr = random.Random(base)
    data = bytearray(b''.join(pr.getrandbits(15 if width == 64 else 32).to_bytes(4, 'little')
                              for _ in range(PAGE // 4)))
    for addr, size, raw in sections:
        lo, hi = max(base, addr), min(base + PAGE, addr + size)
        for a in range(lo, hi):
            off = a - addr
            data[a - base] = raw[off] if off < len(raw) else 0
    return bytes(data)

g = img.disassemble(dragon.Configuration(strategy=0))
dis = dragon.Disassembler(arch, dragon.Configuration())
rng = random.Random(1)
blocks = sorted(g.block_addresses())
rng.shuffle(blocks)

def reg_value(r):
    # On x86-64 Unicorn truncates any address outside the low canonical half
    # (to 48 or 52 bits) where hardware would fault or wrap, and AArch64 TBI
    # ignores the top byte: values that may become addresses therefore stay
    # below 2^47. Edge values for arithmetic are covered by the
    # single-instruction difftests.
    top = MASK if width == 32 else (1 << 47) - 1
    k = r.random()
    if k < 0.3: return r.choice([0, 1, 2, 3, 7, 8, 0x10, 0xff, 0x7f, 0x80, 0x7fffffff,
                                 0x80000000, 0xffffffff, top, top >> 1])
    if k < 0.7: return HEAP + r.randrange(0, 0x10000, 4)
    return r.getrandbits(32 if width == 32 else 47)

stats = Counter()
shown = 0
for b in blocks:
    if stats['blocks run'] >= NBLOCKS:
        break
    bb = g.get_block(b)
    if not bb or not bb.is_valid:
        continue
    insns = list(bb.instructions())
    while insns and (insns[-1].is_jump or insns[-1].is_return or insns[-1].is_trap):
        insns.pop()
    # REALBLOCK_FILTER=text keeps only blocks with an operand containing it
    # (e.g. xmm, to concentrate on SSE code).
    if _os.environ.get('REALBLOCK_FILTER') and not any(
            _os.environ['REALBLOCK_FILTER'] in i.operands for i in insns):
        continue
    if not insns or any(i.is_call or i.is_jump or i.is_return or i.is_trap or
                        i.mnemonic in ('syscall', 'svc', 'int', 'cpuid', 'rdtsc',
                                       'xgetbv', 'rdrand', 'hlt', 'in', 'out')
                        or i.mnemonic.startswith(('rep', 'lock', 'f', 'cmpxchg')) for i in insns):
        stats['skipped (call/system/x87/rep)'] += 1
        continue
    code = b''.join(bytes(i.bytes) for i in insns)
    start, end = insns[0].address, insns[-1].address + insns[-1].size
    blk = dis.lift_trace(dis.disassemble(code, start).instructions())
    if blk.unlifted():
        stats['skipped (unlifted)'] += 1
        continue
    if _os.environ.get('REALBLOCK_OPT'):
        # Run the IGNIL optimisers first: the optimised block must give the
        # same concrete values as the original (everything is live).
        blk, _ = blk.simplify()
        blk, _ = blk.dead_store_eliminate(arch)
    stats['blocks run'] += 1
    for trial in range(TRIALS):
        r = random.Random(b * 131 + trial)
        init = {n: reg_value(r) for n, _, _ in regs}
        init[sp_name] = STACK + r.randrange(0x100, 0xf00, 16)
        fl = r.getrandbits(32) if uc_arch == UC_ARCH_ARM64 else (r.getrandbits(32) & 0x8d5) | 2
        if uc_arch == UC_ARCH_ARM64:
            fl &= 0xf0000000
        uc = Uc(uc_arch, uc_mode)
        mapped = {}
        def map_page(addr):
            base = addr & ~(PAGE - 1)
            if base in mapped:
                return True
            if base < 0x1000 or base > MASK:
                return False
            try:
                uc.mem_map(base, PAGE)
            except UcError:
                return False
            mapped[base] = page_content(base)
            uc.mem_write(base, mapped[base])
            return True
        written = set()
        def on_unmapped(uc_, access, addr, size, value, user):
            ok = map_page(addr)
            if ok and (addr + size - 1) & ~(PAGE - 1) != addr & ~(PAGE - 1):
                ok = map_page(addr + size - 1)
            return ok
        def on_write(uc_, access, addr, size, value, user):
            for a in range(addr & ~7, addr + size, 8):
                written.add(a)
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, on_unmapped)
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
        for a in range(start & ~(PAGE - 1), end, PAGE):
            map_page(a)
        uc.mem_write(start, code)
        for n, _, ucr in regs:
            uc.reg_write(ucr, init[n])
        uc.reg_write(flag_reg, fl)
        xv = xmm_values(b * 131 + trial + 7)
        for (i, lo, hi, ucx), (vlo, vhi) in zip(xmms, xv):
            uc.reg_write(ucx, (vhi << 64) | vlo)
        try:
            uc.emu_start(start, end, count=len(insns))
        except UcError:
            stats['trials faulted'] += 1
            continue
        if uc.reg_read(X86C.UC_X86_REG_RIP if width == 64 and uc_arch == UC_ARCH_X86 else
                       X86C.UC_X86_REG_EIP if uc_arch == UC_ARCH_X86 else A64C.UC_ARM64_REG_PC) != end:
            stats['trials left early'] += 1
            continue
        ex = dragon.SymbolicExecutor()
        for n, reg, _ in regs:
            ex.set_register(reg, init[n])
        for f, reg, bit in flags:
            ex.set_register(reg, (fl >> bit) & 1)
        if uc_arch == UC_ARCH_X86:
            ex.set_register(X.DF, (fl >> 10) & 1)
        for (i, lo, hi, ucx), (vlo, vhi) in zip(xmms, xv):
            ex.set_register(lo, vlo)
            ex.set_register(hi, vhi)
        for base, data in mapped.items():
            # the executor sees the memory as it was before the block ran
            ex.map_memory(base, data)
        res = ex.run(blk)
        if not res.ok:
            stats['executor stopped early'] += 1
            continue
        diffs = []
        for n, reg, ucr in regs:
            got, want = ex.concrete_register(reg), uc.reg_read(ucr) & MASK
            if got is not None and got != want:
                diffs.append(f'{n}: dragon={got:#x} unicorn={want:#x}')
        for i, lo, hi, ucx in xmms:
            full = uc.reg_read(ucx)
            for name, reg, want in ((f'XMM{i}', lo, full & (2**64 - 1)), (f'XMM{i}H', hi, full >> 64)):
                got = ex.concrete_register(reg)
                if got is not None and got != want:
                    diffs.append(f'{name}: dragon={got:#x} unicorn={want:#x}')
        fafter = uc.reg_read(flag_reg)
        for f, reg, bit in flags:
            got = ex.concrete_register(reg)
            if got is not None and (got & 1) != ((fafter >> bit) & 1):
                diffs.append(f'{f}: dragon={got} unicorn={(fafter >> bit) & 1}')
        # A value at or above 2^47 in a register (e.g. a pointer assembled
        # by an unaligned load) may have been used as an address, which
        # Unicorn truncates: its writes are then not where the code wrote.
        high = width == 64 and any(v >> 47 for v in list(init.values()) +
                                   [uc.reg_read(ucr) for _, _, ucr in regs])
        if high:
            stats['memory not compared (high address)'] += 1
        # Only writes inside the regions the harness laid out - the stack,
        # the heap the register values point into, the image - are compared.
        # A write through a pointer loaded from random memory may have gone
        # through an intermediate address >= 2^47 that Unicorn truncated.
        def known(a):
            return (STACK <= a < STACK + PAGE or HEAP <= a < HEAP + 0x11000 or
                    any(addr <= a < addr + size for addr, size, _ in sections))
        skipped = [a for a in written if not known(a)]
        if skipped and not high:
            stats['writes outside known regions (not compared)'] += 1
        for a in ([] if high else sorted(a for a in written if known(a))):
            got = ex.concrete_memory(a, 64)
            want = int.from_bytes(uc.mem_read(a, 8), 'little') if (a & ~(PAGE - 1)) in mapped and \
                ((a + 7) & ~(PAGE - 1)) in mapped else None
            if got is not None and want is not None and got != want:
                diffs.append(f'mem[{a:#x}]: dragon={got:#x} unicorn={want:#x}')
                break
        stats['trials compared'] += 1
        if diffs:
            stats['DIFFERENT'] += 1
            if shown < 12:
                shown += 1
                print(f'DIFF block {b:#x}: ' + '; '.join(f'{i.mnemonic} {i.operands}' for i in insns)[:300])
                print('     ' + '; '.join(diffs[:4]))
            break
print(_os.path.basename(path), dict(stats))
