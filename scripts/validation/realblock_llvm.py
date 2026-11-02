"""Differential test of the LLVM lifter on real basic blocks against Unicorn.

Like realblock_diff.py, but the block (plus a `ret`) is lifted to LLVM IR as
a function, compiled for the host with llc-21 and run natively: a small C
driver maps the pages Unicorn touched at their own addresses (the function's
`mem` argument is NULL, so IR addresses are absolute), fills the register
file at the IGNIL offsets, calls the function and prints registers, flags
and the qwords Unicorn wrote. Any disagreement is reported.

Runs on an AArch64 Linux host (the VM) with llc-21 and gcc. Shared objects
are based at 0, so `sysctl vm.mmap_min_addr=4096` lets their low pages map.
Skipped like in realblock_diff.py: Unicorn faults, values >= 2^47 (Unicorn
truncates such addresses), registers the block leaves UNDEF, and OF after an
x86 shift by more than one (deliberate, see emitGatedShiftFlags).

Usage: realblock_llvm.py BINARY [BLOCKS]
       REALBLOCK_ONLY=0xADDR realblock_llvm.py BINARY   (one block, keeps its files)
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
import random
import re
import subprocess
import sys
import tempfile
from collections import Counter

import dragon
from unicorn import Uc, UcError, UC_ARCH_X86, UC_ARCH_ARM64, UC_MODE_32, UC_MODE_64, \
    UC_MODE_ARM, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, \
    UC_HOOK_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn import x86_const as X86C, arm64_const as A64C

path = sys.argv[1]
NBLOCKS = int(sys.argv[2]) if len(sys.argv) > 2 else 150
# REALBLOCK_ONLY=0xADDR runs just that block and keeps its work directory.
ONLY = int(_os.environ['REALBLOCK_ONLY'], 16) if _os.environ.get('REALBLOCK_ONLY') else None
PAGE = 0x1000
img = dragon.LoadedImage.load_file(path)
arch = img.architecture
A = dragon.Architecture
if arch == A.X86_64:
    X = dragon.x86
    names = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI'] + [f'R{i}' for i in range(8, 16)]
    regs = [(n, getattr(X, n), getattr(X86C, f'UC_X86_REG_{n}')) for n in names]
    flags = [(f, getattr(X, f), b) for f, b in (('CF', 0), ('PF', 2), ('AF', 4), ('ZF', 6), ('SF', 7), ('DF', 10), ('OF', 11))]
    uc_arch, uc_mode, sp_name, flag_reg, width, ret = UC_ARCH_X86, UC_MODE_64, 'RSP', X86C.UC_X86_REG_EFLAGS, 64, b'\xc3'
    pc_reg = X86C.UC_X86_REG_RIP
elif arch == A.X86_32:
    X = dragon.x86
    names = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']
    regs = [(n, getattr(X, n), getattr(X86C, f'UC_X86_REG_{n}')) for n in names]
    flags = [(f, getattr(X, f), b) for f, b in (('CF', 0), ('PF', 2), ('AF', 4), ('ZF', 6), ('SF', 7), ('DF', 10), ('OF', 11))]
    uc_arch, uc_mode, sp_name, flag_reg, width, ret = UC_ARCH_X86, UC_MODE_32, 'ESP', X86C.UC_X86_REG_EFLAGS, 32, b'\xc3'
    pc_reg = X86C.UC_X86_REG_EIP
elif arch == A.ARM64:
    R = dragon.arm
    regs = [(f'X{i}', getattr(R, f'X{i}'), getattr(A64C, f'UC_ARM64_REG_X{i}')) for i in range(31)]
    regs.append(('SP', R.SP, A64C.UC_ARM64_REG_SP))
    flags = [(f, getattr(R, f), b) for f, b in (('NF', 31), ('ZF', 30), ('CF', 29), ('VF', 28))]
    uc_arch, uc_mode, sp_name, flag_reg, width, ret = UC_ARCH_ARM64, UC_MODE_ARM, 'SP', A64C.UC_ARM64_REG_NZCV, 64, \
        b'\xc0\x03\x5f\xd6'
    pc_reg = A64C.UC_ARM64_REG_PC
else:
    print('unsupported'); sys.exit(0)

# XMM registers (x86): seeded with random 128-bit values on both sides and
# compared as their two 64-bit halves (XMMn = bits 63:0, XMMnH = 127:64).
xmms = []
if arch in (A.X86_64, A.X86_32):
    xmms = [(i, getattr(dragon.x86, f'XMM{i}'), getattr(dragon.x86, f'XMM{i}H'),
             getattr(X86C, f'UC_X86_REG_XMM{i}')) for i in range(16 if arch == A.X86_64 else 8)]
elif arch == A.ARM64:
    xmms = [(i,(getattr(dragon.arm,f'V{i}')[0],64,f'V{i}'),
             (getattr(dragon.arm,f'V{i}')[0]+8,64,f'V{i}H'),
             getattr(A64C,f'UC_ARM64_REG_Q{i}')) for i in range(32)]
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


def reg_value(r):
    top = MASK if width == 32 else (1 << 47) - 1
    k = r.random()
    if k < 0.3:
        return r.choice([0, 1, 2, 3, 7, 8, 0x10, 0xff, 0x7f, 0x80, 0x7fffffff, 0x80000000, 0xffffffff, top, top >> 1])
    if k < 0.7:
        return HEAP + r.randrange(0, 0x10000, 4)
    return r.getrandbits(32 if width == 32 else 47)


DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
void FN(unsigned char *, unsigned char *);
static unsigned char regs[8192];
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb");
  unsigned long n, base, off; unsigned char page[4096];
  fread(&n, 8, 1, f);
  for (unsigned long i = 0; i < n; i++) {
    fread(&base, 8, 1, f); fread(page, 1, 4096, f);
    void *p = mmap((void *)base, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)base) { printf("MAPFAIL\n"); return 0; }
    memcpy(p, page, 4096);
  }
  fread(&n, 8, 1, f);
  for (unsigned long i = 0; i < n; i++) { unsigned long v; unsigned w; fread(&off, 8, 1, f); fread(&w, 4, 1, f); fread(&v, 8, 1, f); memcpy(regs + off, &v, w); }
  FN(0, regs);
  fread(&n, 8, 1, f);
  for (unsigned long i = 0; i < n; i++) { unsigned w; fread(&off, 8, 1, f); fread(&w, 4, 1, f); unsigned long v = 0; memcpy(&v, regs + off, w); printf("R %lu %lx\n", off, v); }
  fread(&n, 8, 1, f);
  for (unsigned long i = 0; i < n; i++) { fread(&off, 8, 1, f); unsigned long v; memcpy(&v, (void *)off, 8); printf("M %lx %lx\n", off, v); }
  return 0;
}
'''

g = img.disassemble(dragon.Configuration(strategy=0))
blocks = sorted(g.block_addresses())
random.Random(3).shuffle(blocks)
stats = Counter()
shown = 0
work = tempfile.mkdtemp()
for b in blocks:
    if stats['blocks run'] >= NBLOCKS:
        break
    if ONLY is not None and b != ONLY:
        continue
    bb = g.get_block(b)
    if not bb or not bb.is_valid:
        continue
    insns = list(bb.instructions())
    while insns and (insns[-1].is_jump or insns[-1].is_return or insns[-1].is_trap):
        insns.pop()
    # REALBLOCK_FILTER=text keeps only blocks containing the mnemonic or an operand
    # (e.g. xmm, to concentrate on SSE code).
    if _os.environ.get('REALBLOCK_FILTER') and not any(
            _os.environ['REALBLOCK_FILTER'] in f'{i.mnemonic} {i.operands}' for i in insns):
        continue
    if not insns or any(i.is_call or i.is_jump or i.is_return or i.is_trap or
                        i.mnemonic in ('syscall', 'svc', 'int', 'cpuid', 'rdtsc', 'xgetbv', 'rdrand', 'hlt')
                        or i.mnemonic.startswith(('lock', 'cmpxchg'))
                        or (arch != A.ARM64 and i.mnemonic.startswith('f')) for i in insns):
        continue
    code = b''.join(bytes(i.bytes) for i in insns)
    start, end = insns[0].address, insns[-1].address + insns[-1].size
    blk = dragon.Disassembler(arch, dragon.Configuration()).lift_trace(
        dragon.Disassembler(arch, dragon.Configuration()).disassemble(code, start).instructions())
    if blk.unlifted():
        stats['skip: unlifted'] += 1
        continue
    # Unicorn reference
    r = random.Random(b)
    init = {n: reg_value(r) for n, _, _ in regs}
    if uc_arch == UC_ARCH_X86 and any(i.mnemonic.startswith('rep') for i in insns):
        init['RCX' if width==64 else 'ECX']=r.choice([0,1,2,5])
    init[sp_name] = STACK + r.randrange(0x100, 0xf00, 16)
    fl = (r.getrandbits(32) & 0xf0000000) if uc_arch == UC_ARCH_ARM64 else (r.getrandbits(32) & 0xcd5) | 2
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
    # A 64-bit value loaded from memory (an unaligned load can assemble one
    # from the page bytes) that is >= 2^47 is a pointer Unicorn would
    # truncate if the block dereferences it, so such a trial is skipped too.
    loaded_high = []
    if width == 64:
        uc.hook_add(UC_HOOK_MEM_READ, lambda u, a, addr, size, v, d: size == 8 and int.from_bytes(
            u.mem_read(addr, 8), 'little') >> 47 and loaded_high.append(addr))
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED,
                lambda u, a, addr, size, v, d: map_page(addr) and map_page(addr + size - 1))
    uc.hook_add(UC_HOOK_MEM_WRITE, lambda u, a, addr, size, v, d: written.update(
        range(addr & ~7, addr + size, 8)))
    for a in range(start & ~(PAGE - 1), end, PAGE):
        map_page(a)
    for n, _, ucr in regs:
        uc.reg_write(ucr, init[n])
    uc.reg_write(flag_reg, fl)
    xv = xmm_values(b * 131 + 7)
    for (i, lo, hi, ucx), (vlo, vhi) in zip(xmms, xv):
        uc.reg_write(ucx, (vhi << 64) | vlo)
    try:
        uc.emu_start(start, end, count=100000 if any(i.mnemonic.startswith('rep') for i in insns) else len(insns), timeout=200000)
    except UcError:
        stats['skip: unicorn fault'] += 1
        continue
    if uc.reg_read(pc_reg) != end:
        stats['skip: left early'] += 1
        continue
    high = width == 64 and (any(v >> 47 for v in list(init.values()) + [uc.reg_read(u) for _, _, u in regs])
                            or any(base >> 47 for base in mapped) or bool(loaded_high))
    if high:
        stats['skip: high address'] += 1
        continue
    # LLVM side: the block as a function, compiled and run natively
    dis = dragon.Disassembler(arch, dragon.Configuration())
    fg = dis.analyze(code + ret, start)
    fn = fg.get_function(start)
    if not fn or not fn.is_valid:
        stats['no function'] += 1
        continue
    L = dragon.LLVMLifter(arch)
    if not L.lift(fn, fg):
        stats['lift failed'] += 1
        continue
    ir = L.ir()
    name = re.search(r'define void @"?([\w.]+)"?\(', ir).group(1)
    open(f'{work}/m.ll', 'w').write(ir)
    open(f'{work}/d.c', 'w').write(DRIVER.replace('FN', name))
    c = subprocess.run(f'llc-21 -O0 -filetype=obj -mtriple=aarch64-linux-gnu {work}/m.ll -o {work}/m.o && '
                       f'gcc -O0 {work}/d.c {work}/m.o -o {work}/a', shell=True, capture_output=True, text=True)
    if c.returncode:
        stats['compile failed'] += 1
        print('COMPILE FAIL', hex(b), c.stderr.strip().splitlines()[-1:])
        continue
    flag_regs = [(reg[0], 1) for _, reg, _ in flags]
    gpr_regs = [(reg[0], width // 8) for _, reg, _ in regs]
    import struct
    with open(f'{work}/in.bin', 'wb') as f:
        f.write(struct.pack('<Q', len(mapped)))
        for base, data in mapped.items():
            f.write(struct.pack('<Q', base) + data)
        f.write(struct.pack('<Q', len(regs) + len(flags) + 2 * len(xmms)))
        for (n, reg, _), (off, w) in zip(regs, gpr_regs):
            f.write(struct.pack('<QIQ', off, w, init[n]))
        for (fname, reg, bit), (off, w) in zip(flags, flag_regs):
            f.write(struct.pack('<QIQ', off, 1, (fl >> bit) & 1))
        for (i, lo, hi, ucx), (vlo, vhi) in zip(xmms, xv):
            f.write(struct.pack('<QIQ', lo[0], 8, vlo))
            f.write(struct.pack('<QIQ', hi[0], 8, vhi))
        outs = gpr_regs + flag_regs + [(r[0], 8) for _, lo, hi, _ in xmms for r in (lo, hi)]
        f.write(struct.pack('<Q', len(outs)))
        for off, w in outs:
            f.write(struct.pack('<QI', off, w))
        known = [a for a in sorted(written) if STACK <= a < STACK + PAGE or HEAP <= a < HEAP + 0x11000]
        f.write(struct.pack('<Q', len(known)))
        for a in known:
            f.write(struct.pack('<Q', a))
    run = subprocess.run([f'{work}/a', f'{work}/in.bin'], capture_output=True, text=True, timeout=20)
    if 'MAPFAIL' in run.stdout:
        stats['page could not be mapped at its address'] += 1
        continue
    if run.returncode:
        stats['NATIVE CRASH (rc %d)' % run.returncode] += 1
        if ONLY is not None:
            print('work dir', work)
        print(f'CRASH rc={run.returncode} block {b:#x}: ' +
              '; '.join(f'{i.mnemonic} {i.operands}' for i in insns)[:300])
        continue
    stats['blocks run'] += 1
    got = {}
    for line in run.stdout.split('\n'):
        p = line.split()
        if len(p) == 3:
            got[(p[0], int(p[1], 16) if p[0] == 'M' else int(p[1]))] = int(p[2], 16)
    # Registers the block leaves UNDEF (architecturally undefined results)
    # are LLVM `undef`: any value is right, so they are not compared.
    undef_offsets = set()
    regmod = dragon.x86 if uc_arch == UC_ARCH_X86 else dragon.arm
    for line in blk.to_string().splitlines():
        m = re.match(r'\s*(\w+):i\d+ = UNDEF\(\)', line)
        if m and hasattr(regmod, m.group(1)):
            undef_offsets.add(getattr(regmod, m.group(1))[0])
    # A result *computed from* an UNDEF (`bsr ebx, eax; xor ebx, 31` with
    # eax = 0) is just as free: run the block concretely through the
    # SymbolicExecutor from the same state and skip what it leaves symbolic.
    ex = dragon.SymbolicExecutor()
    for n, reg, _ in regs:
        ex.set_register(reg, init[n])
    for fname, reg, bit in flags:
        ex.set_register(reg, (fl >> bit) & 1)
    for (i, lo, hi, ucx), (vlo, vhi) in zip(xmms, xv):
        ex.set_register(lo, vlo)
        ex.set_register(hi, vhi)
    for base, data in mapped.items():
        ex.map_memory(base, data)
    if ex.run(blk).ok:
        for reg in [r for _, r, _ in list(regs) + list(flags)] + [r for _, lo, hi, _ in xmms for r in (lo, hi)]:
            if ex.concrete_register(reg) is None:
                undef_offsets.add(reg[0])
    diffs = []
    for (n, reg, ucr) in regs:
        if reg[0] in undef_offsets:
            continue
        want = uc.reg_read(ucr) & MASK
        v = got.get(('R', reg[0]))
        if v is not None and (v & MASK) != want:
            diffs.append(f'{n}: llvm={v & MASK:#x} unicorn={want:#x}')
    for i, lo, hi, ucx in xmms:
        full = uc.reg_read(ucx)
        prefix = 'V' if arch == A.ARM64 else 'XMM'
        for name, reg, want in ((f'{prefix}{i}', lo, full & (2**64 - 1)), (f'{prefix}{i}H', hi, full >> 64)):
            v = got.get(('R', reg[0]))
            if reg[0] not in undef_offsets and v is not None and v != want:
                diffs.append(f'{name}: llvm={v:#x} unicorn={want:#x}')
    fafter = uc.reg_read(flag_reg)
    # Deliberate: after an x86 shift by more than one, OF is architecturally
    # undefined and dragon models it like a 1-bit shift (emitGatedShiftFlags);
    # Unicorn clears it. Not compared when such a shift is in the block.
    multi_shift = uc_arch == UC_ARCH_X86 and any(
        i.mnemonic in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd') and not i.operands.replace(' ', '').endswith(',1')
        for i in insns)
    for fname, reg, bit in flags:
        v = got.get(('R', reg[0]))
        if reg[0] in undef_offsets or (fname == 'OF' and multi_shift):
            continue
        if v is not None and (v & 1) != (fafter >> bit) & 1:
            diffs.append(f'{fname}: llvm={v & 1} unicorn={(fafter >> bit) & 1}')
    for a in known:
        v = got.get(('M', a))
        want = int.from_bytes(uc.mem_read(a, 8), 'little')
        if v is not None and v != want:
            diffs.append(f'mem[{a:#x}]: llvm={v:#x} unicorn={want:#x}')
            break
    if diffs:
        stats['DIFFERENT'] += 1
        if shown < 12:
            shown += 1
            print(f'DIFF block {b:#x}: ' + '; '.join(f'{i.mnemonic} {i.operands}' for i in insns)[:300])
            print('     ' + '; '.join(diffs[:4]))
print(_os.path.basename(path), dict(stats))
