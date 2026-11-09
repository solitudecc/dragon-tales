# Python API

The Python API wraps the C shim via [cffi](https://cffi.readthedocs.io/).
Build the C shim first (`libdragon_c.so` must exist in `build/c_shim/`),
then import the module:

```python
import sys
sys.path.insert(0, 'python')
import dragon
```

All three main objects (`Configuration`, `Disassembler`, `Assembler`) support the
context-manager protocol for deterministic cleanup.

---

## Configuration

```python
cfg = dragon.Configuration(
    syntax   = dragon.Syntax.INTEL,           # default
    strategy = dragon.Strategy.LINEAR_SWEEP,  # default
    cpu      = '',                            # default — LLVM "generic" baseline
    features = '',                            # default — no extra features
    calling_convention = dragon.CallingConvention.DEFAULT,
)

print(cfg)          # <Configuration syntax=0 strategy=0>
print(cfg.syntax)   # 0  (INTEL)
print(cfg.strategy) # 0  (LINEAR_SWEEP)
print(cfg.cpu)      # ''
print(cfg.features) # ''
```

If you omit `config` when constructing a `Disassembler` or `Assembler`,
a default `Configuration` is created automatically.

### Targeting a specific CPU or ISA revision

`cpu` and `features` are forwarded to LLVM's `MCSubtargetInfo`. An empty
string uses the "generic" baseline for the target triple, which may omit
optional ISA extensions. Both are readable and writable properties.

```python
# ARM32 — enable BX/BLX (ARMv4T, absent from the generic ARM baseline)
cfg = dragon.Configuration(cpu="arm926ej-s")
dis = dragon.Disassembler(dragon.Architecture.ARM32, cfg)

# ARM32 — same effect using a feature flag instead of a CPU name
cfg = dragon.Configuration(features="+v4t")
dis = dragon.Disassembler(dragon.Architecture.ARM32, cfg)

# ARM32 — ARMv7-A with NEON
cfg = dragon.Configuration(features="+v7,+neon")

# x86-64 — Skylake micro-architecture
cfg = dragon.Configuration(cpu="skylake")

# RISC-V 64 — multiply, atomics and compressed extensions
cfg = dragon.Configuration(features="+m,+a,+c")

# Properties are writable after construction
cfg = dragon.Configuration()
cfg.cpu      = "cortex-a8"
cfg.features = "+neon"
```

**cpu examples by architecture**

| Architecture | `cpu` value | Enables |
|---|---|---|
| ARM32 | `"arm926ej-s"` | ARMv5TEJ — BX, BLX |
| ARM32 | `"cortex-a8"` | ARMv7-A, Thumb2, NEON, VFPv3 |
| ARM64 | `"cortex-a53"` | AArch64 baseline |
| ARM64 | `"apple-m1"` | AArch64 + Apple extensions |
| x86-64 | `"skylake"` | AVX2, BMI2, AES |
| x86-64 | `"znver3"` | AMD Zen 3 |
| RISC-V 64 | `"sifive-u74"` | RV64GC |

**features examples**

| String | Meaning |
|---|---|
| `"+v4t"` | ARM32: add BX (ARMv4T) |
| `"+v7,+neon"` | ARM32: ARMv7 + NEON |
| `"+sve"` | ARM64: Scalable Vector Extension |
| `"+avx2"` | x86: AVX2 |
| `"+m,+a,+c"` | RISC-V: multiply, atomics, compressed |
| `"+f,+d"` | RISC-V: single- and double-precision FP |

---

## Disassembler

### Basic usage

```python
code = bytes([0x90, 0xC3])   # nop; ret

cfg = dragon.Configuration(syntax=dragon.Syntax.INTEL)
dis = dragon.Disassembler(arch=dragon.Architecture.X86_64, config=cfg)

graph = dis.disassemble(code, base_address=0x1000)

for insn in graph.instructions():
    print(f"0x{insn.address:x}  {insn.disassembly}")
# 0x1000  nop
# 0x1001  ret
```

### Using the context manager

```python
cfg = dragon.Configuration()
with dragon.Disassembler(dragon.Architecture.X86_64, cfg) as dis:
    with dis.disassemble(code, 0x1000) as graph:
        for insn in graph.instructions():
            print(hex(insn.address), insn.mnemonic, insn.operands)
```

### Instruction properties

```python
for insn in graph.instructions():
    print(insn.address)          # int
    print(insn.size)             # int (bytes)
    print(insn.mnemonic)         # str
    print(insn.operands)         # str
    print(insn.disassembly)      # mnemonic + operands
    print(insn.bytes.hex())      # raw encoding, e.g. "90"
    print(insn.pattern)          # wildcard pattern string
    print(insn.chromosome_mask)  # bytes

    print(insn.is_call)
    print(insn.is_jump)
    print(insn.is_return)
    print(insn.is_conditional)
    print(insn.is_block_start)
    print(insn.is_function_start)
    print(insn.edges)

    print(insn.branches)   # list of target addresses
    print(insn.callees)    # list of callee addresses
```

### Recursive traversal

```python
cfg = dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL)
dis = dragon.Disassembler(dragon.Architecture.X86_64, cfg)
graph = dis.disassemble(code, 0x1000)
```

### Analysis: blocks, functions and IGNIL

`disassemble()` gives you the flat instruction listing. `analyze()` takes the
same arguments but goes further — it runs block and function discovery and
lifts each block to IGNIL, so the `Graph` it returns has everything the
backward slicer, the LLVM lifter and symbolic execution need.

```python
graph = dis.analyze(code, base_address=0x1000)
```

Use `disassemble()` when you only want the listing; use `analyze()` whenever a
later step needs blocks, functions or IGNIL.

### Entry points and external memory

`AnalysisHints` owns a copied list of entry points, which become function and
block starts and recursive traversal roots. Entries outside the supplied code
buffer are ignored. An optional `read_memory(address, size)` callable returns
exactly `size` bytes (a bytes-like object), or `None` for an unavailable range.
This ordinary reader exposes initial image bytes. It cannot certify runtime
jump-table contents, even when the section is read-only.

```python
def read_table(address, size):
    if table_address <= address and address + size <= table_address + len(table):
        offset = address - table_address
        return table[offset:offset + size]
    return None

with dragon.AnalysisHints(entry_points=[0x1000, 0x1100],
                          read_memory=read_table) as hints:
    graph = dis.analyze(code, base_address=0x1000, hints=hints)
# graph remains valid after hints are closed
```

`entry_points`, `read_memory` and the optional `read_known_memory` are writable
properties. The hints keep
their reader alive, and the callback is used only during analysis. Callback
exceptions are propagated after the native call returns; incorrect byte counts
raise `ValueError`. Access after explicitly closing hints raises
`ReferenceError`.

`read_known_memory(block, op_index, instruction_pc, address, size)` returns
exactly `size` bytes or None. It explicitly asserts contents immediately
before that IGNIL LOAD for all executions/visits admitted by the entry
contract, including paths exposed by later CFG expansion. It is not a dynamic
snapshot or an initial heap model. An unsupported entry remains unknown; no
partial target list is published. For example, an independent certificate can
serve reads keyed by the current LOAD identity:

```python
def certified_read(block, index, pc, address, size):
    data = certified_reads.get((block, index, pc, address))
    return data if data is not None and len(data) == size else None

with dragon.AnalysisHints(read_known_memory=certified_read) as hints:
    graph = dis.analyze(code, base_address=0x1000, hints=hints)
```

`certified_reads` must come from an independent proof, rather than a renamed
image byte map. A callback may be asked for individual entries repeatedly.
Trusted-reader exceptions stop native analysis and are re-raised with their
original identity; bad byte counts raise ValueError. Callbacks may replace or
close hints. Explicitly closing the active disassembler or loaded image from
its callback raises RuntimeError, and the object remains usable afterwards.
See [the full content contract](table-content-proof.md).
Decoded direct callees remain independent inputs even with explicit entry
points; tail-jump initialization does not constrain arbitrary CALL entries.

`Configuration.calling_convention` is readable and writable:
`CallingConvention.DEFAULT`, `SYSV`, or `WIN64`. Set it before constructing the
disassembler, which copies the configuration. It controls recovered argument
registers and the signature attached to lifted LLVM functions. Image loading
chooses Win64 automatically for x86-64 PE when the setting is DEFAULT.

### Trace lifting

`Disassembler.lift_trace()` lifts an arbitrary linear sequence of
already-decoded instructions straight to IGNIL — e.g. a captured execution
trace (Unicorn/QBDI/Frida/Intel PT) that isn't claimed to form a single
valid basic block or a coherent CFG. It bypasses `disassemble()`'s
CFG/function/block-validity analysis entirely: each instruction is lifted
independently and the results concatenated in order, with no synthesized
fall-through terminator.

```python
graph = dis.disassemble(code, 0x1000)
insns = graph.instructions()   # or hand-pick/reorder — need not be contiguous

with dis.lift_trace(insns) as block:
    print(block.to_string())
```

The returned `IGNILBlock` is **owned** — unlike `Graph.get_ignil_block()`'s
borrowed one — and gets destroyed on `__del__`/context-manager exit. It's
still the same `IGNILBlock` class either way; only the `owned` flag differs
internally.

---

## Graph: blocks and functions

```python
# Look up a single instruction by address
insn = graph.get_instruction(0x1000)
if insn:
    print(insn.disassembly)

# Block addresses that were identified
for addr in graph.block_addresses():
    print(f"block @ {hex(addr)}")

# Inspect a single block
bb = graph.get_block(0x1000)
if bb.is_valid:
    print(f"size:       {bb.size} instructions")
    print(f"byte size:  {bb.byte_size}")
    print(f"terminator: {hex(bb.terminator_address)}")
    for insn in bb.instructions():
        print(" ", insn.disassembly)
    for succ in bb.successors():
        print(f"  successor -> {hex(succ)}")

# Function addresses that were identified
for addr in graph.function_addresses():
    print(f"function @ {hex(addr)}")

# Inspect a single function
fn = graph.get_function(0x1000)
if fn.is_valid:
    print(f"blocks:       {fn.num_blocks}")
    print(f"instructions: {fn.num_instructions}")
    print(f"byte size:    {fn.byte_size}")
    print(f"cyclomatic:   {fn.cyclomatic_complexity}")
```

### Lifetimes: views keep their graph alive

`Instruction`, `BasicBlock`, `Function` and the `IGNILBlock` returned by
`Graph.get_ignil_block()` all point into memory the `Graph` owns. Each of
them holds a reference to its graph, so the graph lives as long as any view
of it does, and chaining is safe:

```python
for insn in dis.analyze(code, 0x1000).instructions():   # graph kept alive
    print(insn.disassembly)
```

Closing the graph explicitly (`with graph:` exit, or `graph.__exit__()`)
still frees it immediately. Any view of it used afterwards raises
`ReferenceError` instead of reading freed memory:

```python
with dis.analyze(code, 0x1000) as graph:
    insn = graph.get_instruction(0x1000)
insn.disassembly   # ReferenceError: Instruction used after its Graph was closed
```

---

## Assembler

```python
cfg = dragon.Configuration(syntax=dragon.Syntax.INTEL)
asm = dragon.Assembler(arch=dragon.Architecture.X86_64, config=cfg)

if not asm.is_valid:
    raise RuntimeError("assembler init failed")

raw = asm.assemble("nop\nret\n")
print(raw.hex())   # 90c3
```

### Round-trip example

```python
source = "push rbp\nmov rbp, rsp\npop rbp\nret\n"

cfg = dragon.Configuration()
asm = dragon.Assembler(dragon.Architecture.X86_64, cfg)
raw = asm.assemble(source)

dis = dragon.Disassembler(dragon.Architecture.X86_64, cfg)
graph = dis.disassemble(raw, base_address=0x0)

for insn in graph.instructions():
    print(f"0x{insn.address:x}  {insn.disassembly}")
```

### AT&T syntax

```python
cfg = dragon.Configuration(syntax=dragon.Syntax.ATT)
asm = dragon.Assembler(dragon.Architecture.X86_64, cfg)
raw = asm.assemble("nop\nret\n")
```

---

## Loading a binary

`LoadedImage` parses an ELF or PE/COFF file — or in-memory bytes — into its
sections and segments at their virtual addresses, the entry point, the image
base, the symbol table, and the architecture, all detected from the header.
You never name the architecture: it comes from the file.

```python
img = dragon.LoadedImage.load_file('a.out')
print(hex(img.image_base), 'arch', img.architecture)
print('entry', hex(img.entry) if img.entry is not None else None)

for s in img.sections():
    print(s)        # <Section '.text' @0x1139 size=0x1a2 r-x>

graph = img.disassemble()       # every executable section -> a populated Graph
print(len(graph), 'instructions')
```

Load from bytes you already hold with `LoadedImage.load_bytes(data)`. Both
classmethods raise `ValueError` with the loader's message on failure, and the
image is a context manager:

```python
with dragon.LoadedImage.load_file('a.out') as img:
    graph = img.disassemble()
```

### Sections, segments and symbols

`sections()` and `segments()` both return `Section` objects (`name`,
`address`, `virtual_size`, `readable`, `writable`, `executable`, `data`).
Sections are the linker's view; segments are the ELF `PT_LOAD` regions the
system loader actually maps, named `LOAD0`, `LOAD1`, … — the only layout left
to work with once a protector strips the section headers. `segments()` is
empty for PE.

```python
for seg in img.segments():
    print(seg)      # <Section 'LOAD0' @0x400000 size=0x1000 r-x>

for sym in img.symbols():
    if sym.exported:
        print(hex(sym.address), sym.name, sym.kind)
```

`read_virtual(address, size)` reads straight out of the mapped address space,
or returns `None` if the range is unmapped — handy for following a pointer the
disassembler turned up without caring which section it lands in.

```python
data = img.read_virtual(img.image_base, 4)
print(data.hex() if data else 'unmapped')
```

### disassemble()

`disassemble(config=None, *, graph_pipeline=None)` disassembles every executable section into a
`Graph` built for the detected architecture, so a file reaches analysis in one
call. Pass a `Configuration` to control syntax or strategy; omit it for the
defaults.

| Member | Type | Notes |
|---|---|---|
| `LoadedImage.load_file(path)` | `LoadedImage` | Classmethod; raises `ValueError` on failure |
| `LoadedImage.load_bytes(data)` | `LoadedImage` | Classmethod; raises `ValueError` on failure |
| `format` | `Format` | `ELF`, `PE`, or `UNKNOWN` |
| `architecture` | `Architecture` | Detected from the header |
| `is_64bit` | `bool` | |
| `image_base` | `int` | |
| `entry` | `int` or `None` | `None` when the image has no entry point |
| `sections()` | `list[Section]` | |
| `segments()` | `list[Section]` | ELF `PT_LOAD` regions; empty for PE |
| `symbols()` | `list[Symbol]` | |
| `read_virtual(address, size)` | `bytes` or `None` | `None` if the range is unmapped |
| `disassemble(config=None, *, graph_pipeline=None)` | `Graph` | Every executable section, then an optional Graph pipeline |

`Section` and `Symbol` are plain data objects; `Format` and `SymbolKind` are
the enums listed under [Enum reference](#enum-reference).

---

## IGNIL blocks

`Disassembler.analyze()` (not `disassemble()`) populates IGNIL blocks in the
graph. Retrieve one with `graph.get_ignil_block(address)`.

```python
dis   = dragon.Disassembler(dragon.Architecture.X86_64)
# nop; cmp rax, rbx; ret
code  = bytes([0x90, 0x48, 0x39, 0xD8, 0xC3])
graph = dis.analyze(code, base_address=0x1000)

ib = graph.get_ignil_block(0x1000)   # None if no IGNIL block at that address
if ib:
    print(ib.to_string())
    # IGNILBlock @ 0x1000
    #   t0:i64 = SUB(RAX:i64, RBX:i64)  ; @0x1001
    #   ...

    # Individual ops (each yields address, opcode int, index)
    for addr, opc, idx in ib.ops():
        print(f"[{idx}] @{hex(addr)}  {dragon.IGNILOpcode.name(opc)}")
        print(ib.op_to_string(idx))
```

`IGNILBlock` is a borrowed view — it keeps the owning graph alive and is
never destroyed itself; see [Lifetimes](#lifetimes-views-keep-their-graph-alive).

### Peephole simplification

`simplify()` runs the peephole simplifier to a fixed point: constant
folding/propagation, algebraic identity/annihilator elimination, no-op
width-conversion elimination, and dead-Temp elimination. It is block-local
only — it never crosses into predecessor/successor blocks, and never
removes a `Reg`-defining op even if apparently unused within the block (a
`Reg` may be live-out).

```python
simplified, stats = ib.simplify()   # max_iterations=0 selects the default (8)
print(stats)
# <SimplifyStats constants_folded=2 identities_applied=1 ops_removed=3 iterations=2>
print(simplified.to_string())
```

`simplified` is a new, independently owned `IGNILBlock` — `ib` is left
untouched and both must be cleaned up independently (context manager or GC).

### Dead-store elimination

`dead_store_eliminate(arch, live_out=None)` removes dead register and flag
stores by byte-precise backward liveness: a value written to a register that
is overwritten before it is ever read, and is not live on exit, is dropped.
Unlike `simplify()` this *does* remove `Reg`-defining ops — which is what
collapses VM-protector junk — so an instruction whose only live effect was the
removed write disappears entirely. Side-effecting ops
(`STORE`/`CALL`/`JMP`/`CJMP`/`RET`) are never removed.

```python
cleaned, removed = ib.dead_store_eliminate(dragon.Architecture.X86_64)
print(f"{removed} ops removed")
print(cleaned.to_string())
```

`arch` selects the register file used to resolve which bytes a 32-bit write
zero-extends over, so pass the architecture the block was lifted for.
`live_out` is an iterable of `(offset, width)` pairs — the first two fields of
the `x86` / `arm` descriptors — naming the registers observable after the
block. `None` (the default) assumes everything is live on exit, which removes a
store only when it is provably overwritten within the block and so needs no ABI
knowledge; passing a precise list (e.g. just the ABI return and callee-saved
registers for a `RET`-terminated block) allows more aggressive elimination,
with correctness then resting on the list being sound. The returned block is
new and independently owned.

### Instructions with no semantics

`unlifted()` returns the machine addresses of the instructions the lifter had
no semantics for, in order. Their results are `UNDEF`, so nothing downstream is
silently wrong — but an `UNDEF` that later dies is invisible, and an
instruction whose only effect is a memory write leaves no op at all. Check it
when the block has to be a complete account of the code, e.g. before trusting a
symbolic run of it. `simplify()` and `dead_store_eliminate()` carry the list
over.

System/exception transitions emit `IGNILOpcode.UNSUPPORTED` (72) as well as
the marker. Execution stops before that instruction, including in symbolic
trace mode; no OS return value or post-state is supplied. IGNIL optimizers
preserve the block unchanged. Legacy refuses it; Static/V2 report the exact
`Unsupported/Instruction` PC. See [the boundary contract](system-transition-boundaries.md).

```python
missing = ib.unlifted()
if missing:
    print('no semantics for:', [hex(a) for a in missing])
```

---

## x86 register constants

`dragon.x86` exposes every x86/x86-64 register as a 3-tuple
`(offset, width, name)` that can be splatted directly into
`BackwardSlicer.slice_reg()`:

```python
dragon.x86.RAX   # (0,   64, 'RAX')
dragon.x86.ZF    # (139,  8, 'ZF')
dragon.x86.AH    # (1,    8, 'AH')

# Available groups
# 64-bit: RAX RCX RDX RBX RSP RBP RSI RDI R8..R15 RIP
# 32-bit: EAX ECX .. R15D
# 16-bit: AX CX .. R15W
# 8-bit low: AL CL .. R15B
# 8-bit high: AH CH DH BH
# Flags (8-bit): CF PF AF ZF SF OF DF
```

---

The x87 descriptors are `FPU0..FPU7` (raw 80 bits), `FPU0L..FPU7L`
(low 64) and `FPU0H..FPU7H` (high 16), plus `FPTOP`, `FPTAG`, `FPCW`, `FPSW`.
Use the two views to initialize full extended values without losing bits:

```python
ex.set_register(dragon.x86.FPU0L, 0x8000000000000001)
ex.set_register(dragon.x86.FPU0H, 0x3fff)
ex.set_register(dragon.x86.FPTOP, 0)
ex.set_register(dragon.x86.FPTAG, 0xfffc)  # R0 valid, other slots empty
ex.set_register(dragon.x86.FPCW, 0x037f)  # masked exceptions, nearest ties-even
ex.set_register(dragon.x86.FPSW, 0)
```

ST(i) means physical `(TOP+i)&7`. Stores and integer conversions honor CW.RC;
extended arithmetic remains explicitly unlifted. FPTOP is authoritative when
composing SW. The executor does not deliver hardware traps or model FIP/FDP.

---

## AArch64 register constants

`dragon.arm` mirrors `dragon.x86` for AArch64 — every register as a 3-tuple
`(offset, width, name)`:

```python
dragon.arm.X0   # (0,   64, 'X0')
dragon.arm.ZF   # (785,  8, 'ZF')
dragon.arm.W0   # (0,   32, 'W0')   -- 32-bit view of X0
dragon.arm.FP   # (232, 64, 'FP')  -- alias for X29
dragon.arm.LR   # (240, 64, 'LR')  -- alias for X30

# Available groups
# 64-bit GPRs: X0..X28 X29(FP) X30(LR) XZR SP PC
# 32-bit views: W0..W28 W29 W30 WZR WSP
# NEON/FP (same 128-bit slot, narrower views): V0..V31/Q0..Q31 (128-bit),
#   D0..D31 (64-bit), S0..S31 (32-bit), H0..H31 (16-bit), B0..B31 (8-bit)
# NZCV flags (8-bit): NF ZF CF VF
```

---

## Backward slicer

`BackwardSlicer` propagates a register value backward through an IGNIL CFG.
Both the slicer and every `SliceExpr` it returns are context managers (owned
handles that must be destroyed).

```python
dis   = dragon.Disassembler(dragon.Architecture.X86_64)
code  = bytes([0x48, 0x39, 0xD8, 0xC3])   # cmp rax, rbx; ret
graph = dis.analyze(code, 0x5000)

with dragon.BackwardSlicer(max_blocks=5) as slicer:
    with slicer.slice_reg(graph, 0x5000, *dragon.x86.ZF) as expr:
        print(expr.to_string())
        # EQ(SUB(RAX:i64, RBX:i64), 0x0:i64)
```

The same call works unchanged for AArch64 — swap the architecture and the
register namespace:

```python
dis   = dragon.Disassembler(dragon.Architecture.ARM64)
code  = bytes([0x1f, 0x14, 0x00, 0x71, 0xc0, 0x03, 0x5f, 0xd6])  # cmp w0, #5 ; ret
graph = dis.analyze(code, 0x5000)

with dragon.BackwardSlicer(max_blocks=5) as slicer:
    with slicer.slice_reg(graph, 0x5000, *dragon.arm.ZF) as expr:
        print(expr.to_string())
        # EQ(SUB(W0:i32, 0x5:i32), 0x0:i32)
```

### Walking the tree

```python
def print_expr(expr, indent=0):
    pad = '  ' * indent
    t = expr.type
    if t == dragon.SliceExprType.CONST:
        print(f"{pad}Const 0x{expr.const_value:x}:i{expr.const_width}")
    elif t == dragon.SliceExprType.SYM_REG:
        print(f"{pad}Reg {expr.symreg_name}:i{expr.symreg_width}")
    elif t == dragon.SliceExprType.BINOP:
        print(f"{pad}{expr.opcode_name}")
        with expr.left()  as l: print_expr(l,  indent + 1)
        with expr.right() as r: print_expr(r, indent + 1)
    elif t == dragon.SliceExprType.UNARYOP:
        print(f"{pad}{expr.opcode_name}")
        with expr.operand() as o: print_expr(o, indent + 1)
    elif t == dragon.SliceExprType.LOAD:
        print(f"{pad}Load:i{expr.load_width}")
        with expr.load_addr() as a: print_expr(a, indent + 1)
    else:
        print(f"{pad}undef")

with dragon.BackwardSlicer() as slicer:
    with slicer.slice_reg(graph, 0x5000, *dragon.x86.ZF) as expr:
        print_expr(expr)
# EQ
#   SUB
#     Reg RAX:i64
#     Reg RBX:i64
#   Const 0x0:i64
```

### SliceExpr properties

| Property / method | Type | Available on |
|---|---|---|
| `type` | `SliceExprType` | all |
| `to_string()` | `str` | all |
| `const_value` | `int` | CONST |
| `const_width` | `int` | CONST |
| `symreg_offset` | `int` | SYM_REG |
| `symreg_width` | `int` | SYM_REG |
| `symreg_name` | `str` | SYM_REG |
| `opcode` | `int` | BINOP / UNARYOP |
| `opcode_name` | `str` | BINOP / UNARYOP |
| `left()` | owned `SliceExpr` | BINOP |
| `right()` | owned `SliceExpr` | BINOP |
| `operand()` | owned `SliceExpr` | UNARYOP |
| `load_addr()` | owned `SliceExpr` | LOAD |
| `load_width` | `int` | LOAD |

### Z3 string representation

Available when dragon was built with `-DDRAGON_USE_Z3=ON`.
Raises `RuntimeError` if Z3 support is not present.

```python
z3_str = expr.to_z3_string()
print(z3_str)   # (bvadd rax #x0000000000000004)
```

The returned string is the Z3 bitvector expression in SMT-LIB2 format —
identical to what you would get from lowering with `SliceExprZ3` in C++.

### Value enumeration and unique-value solving

Also requires `-DDRAGON_USE_Z3=ON`; both raise `RuntimeError` otherwise.
Each call constructs its own Z3 context internally — there is no way to pass
extra Z3 assertions across the C boundary, unlike the C++ API's optional
`constraints` parameter; use the C++ API directly
(`dragon::slice::enumeratedValues` / `solveUnique`) if you need that.

```python
values = expr.enumerate_values()          # max_values=0 -> default cap (256), sorted=True
print(values)                             # e.g. [1, 2, 3, 5, 8]

unique = expr.solve_unique()
print(unique)                             # a single int, or None if not uniquely determined
```

| Method | Returns | Notes |
|---|---|---|
| `enumerate_values(max_values=0, sorted=True)` | `list[int]` | `max_values=0` selects the library default (256) |
| `solve_unique()` | `int` or `None` | `None` when zero or more than one model exists |

---

## SwitchDetector

Recognizes a jump-table indirect-branch pattern from a backward-slice
result (typically the target of an indirect jump, sliced with
`BackwardSlicer` above) and reads the concrete targets out of the table's
raw bytes.
`read_table_targets()` only decodes the supplied bytes. The shape and bound
alone do not prove the values seen by the runtime LOAD; automatic CFG recovery
uses Guest STORE facts or the explicit trusted callback.

```python
with dragon.BackwardSlicer() as slicer:
    with slicer.slice_reg(graph, block_addr, *dragon.x86.RAX) as jump_target:
        sw = dragon.detect_possible_switch(jump_target, block_addr, graph)
        if sw is not None:
            print(sw)   # <SwitchCandidate table_base=0x... entry_size=8 num_cases=4>

            # `rodata`/`rodata_base` describe a window of the binary's image
            # (e.g. the whole .rodata section) that contains the table.
            targets = dragon.read_table_targets(sw, rodata, rodata_base)
            for i, t in enumerate(targets):
                print(f"case {i} -> 0x{t:x}")
```

`SwitchCandidate` is a plain data object (`table_base`, `entry_size`,
`num_cases`, `index_reg_offset`, `index_reg_width` — the last two use the
same register-address-space convention as `BackwardSlicer.slice_reg()`),
returned by `detect_possible_switch()` and also constructible directly if
you already know the table's shape from other analysis.

| Function | Returns | Notes |
|---|---|---|
| `detect_possible_switch(jump_target, block_addr, graph)` | `SwitchCandidate` or `None` | A returned candidate with `num_cases == 0` means the pattern matched but the bounds guard wasn't found |

For a table of offsets (x86-64 PIC, AArch64 gcc) the candidate has
`relative=True`; case *i* is at
`target_base + (ext(entry[i]) << entry_shift)`, where `ext` sign-extends
(`entry_signed`) or zero-extends the low `entry_bits` bits of the entry.
`read_table_targets()` applies that.
| `read_table_targets(candidate, data, base_addr)` | `list[int]` | Empty if `num_cases == 0` or the table falls outside `data` |

Not every indirect jump is a jump table — GCC and Clang often lower a
`switch` to a plain compare chain instead (no jump table at all) when the
case count is small or the cases aren't dense, in which case
`detect_possible_switch` correctly returns `None` and there's nothing
further to recover here.

---

## LLVMLifter

`LLVMLifter` lifts a `Function` to LLVM IR using the IGNIL graph.
`graph` must come from `Disassembler.analyze()` — IGNIL blocks must be present.

```python
dis   = dragon.Disassembler(dragon.Architecture.X86_64)
code  = bytes([
    0x55,                    # push rbp
    0x48, 0x89, 0xE5,        # mov  rbp, rsp
    0x48, 0x01, 0xD8,        # add  rax, rbx
    0x5D,                    # pop  rbp
    0xC3,                    # ret
])
graph = dis.analyze(code, base_address=0x1000)

lifter = dragon.LLVMLifter(arch=dragon.Architecture.X86_64, module_name='my_module')

fn = graph.get_function(0x1000)
if lifter.lift(fn, graph):
    print(lifter.ir())    # raw IR

    lifter.optimize()     # mem2reg → instcombine → GVN → MBA → predicates → DCE

    print(lifter.ir())    # optimized IR
```

### StatefulV2 execution

The default is `LLVMExecutionMode.LEGACY`. To emit stateful Guest functions:

```python
lifter = dragon.LLVMLifter(
    arch=dragon.Architecture.X86_64,
    execution_mode=dragon.LLVMExecutionMode.STATEFUL_V2)
assert lifter.lift(fn, graph)
```

V2 emits uniform four-pointer implementations named `dragon_v2_<arch>_<addr>`.
Compile/JIT the IR and register all addresses listed in `dragon.entrypoints`;
the shared C types and `dragon.lib.dragon_llvm_execute_v2` use the same
`Execution.h` layout. The caller owns all buffers and implementations. The
Guest PC in the register file drives dispatch, and the exit record distinguishes
normal root Return from missing targets, unsupported effects and budgets.
A CALL/JMP to the root exit address does not count as a Guest Return.

With `LoadedImage.disassemble()` the IR can contain conservative
`!dragon.imports` provenance; raw-byte Graphs have no loader names to infer.
Names do not call host libraries. V2 models near x86 CALL/RET and ordinary A64
BL/BLR/RET; Legacy safely refuses transfers its interface cannot represent.
See [the execution contract](llvm-execution-contract.md) for exact supported
conditions, state/memory layouts, ownership and registration requirements.

### Static analysis

Select `LLVMExecutionMode.STATIC_ANALYSIS` before lifting. Functions use
`dragon_static_<arch>_<addr>(state, memory, runtime, exit)`. Unknown calls remain
four-pointer declarations of `dragon_static_external`; no external implementation
is needed to verify or optimize the IR. Calls commit Guest stack/LR effects,
flush state, and reload it before a verified normal continuation. All modelled
registers and Guest memory may change. Other exits propagate explicitly.
Static IR consumes no V2 budget and is not registered with `execute_v2`.
Known internal entries call `dragon_static_*` directly, with forward declarations
that can receive definitions later in either lift order. See the
[static contract](llvm-static-analysis-contract.md) for scope and later stages.

To analyze execution beginning only at each function's start, select the
explicit entry policy:

```python
lifter = dragon.LLVMLifter(
    image.architecture, execution_mode=dragon.LLVMExecutionMode.STATIC_ANALYSIS,
    static_entry_policy=dragon.LLVMStaticEntryPolicy.FUNCTION_START)
```

This emits `dragon_static_entry_<arch>_<addr>` functions. Other input PCs
return Unsupported/Entry while preserving Guest registers and memory.
Internal loops and call continuations still execute normally. The default
`None`/`LLVMStaticEntryPolicy.RESUMABLE` retains resumable static functions.
This option requires static mode; it supplies no initialization/heap facts.
See [entry views](llvm-static-entry.md).

### Optimization passes

Individual passes can be called separately or use `optimize()` for the
recommended pipeline.

`PassPipeline.llvm_default()` constructs the compatible named sweep. You can
select/reorder steps, pass integer options, register callable-class plugins,
and use `pipeline.run(lifter, max_iterations=4, max_pass_runs=32)` or
`lifter.optimize(pipeline)` to obtain a report. Plain `lifter.optimize()` still
returns None and runs one historical sweep through the public `run_*` hooks.

Graph pipelines run via `pipeline.run(graph)` or the optional
`graph_pipeline=` keyword of `Disassembler.analyze()` / `LoadedImage.disassemble()`.
Existing borrowed Graph views expire after a Graph pipeline call; reacquire
them before use. Failed automatic Graph processing closes its new Graph.
See [pass pipelines](pass-pipeline.md) for callbacks, cache scope, errors and limits.

`pipeline.register_finite_dispatch(code, base_address, entry_points=entries)` copies
code and entries; follow with `.add("finite-dispatch")` and `.run(graph)` to opt in
to native x86 recovery. Optional `hints=hints` unions both entry lists and
snapshots the two reader callables into independent bridges/error lists.
Replacing or closing the original hints does not alter the registration;
successful override/close releases the snapshot, while `clear()` only clears
steps. The callable's captured mutable data is not deep-copied.
Trusted-reader errors stop subsequent passes and raise PipelineError with the
original exception as its cause. A failed target must be discarded. Finally
clears the pipeline's error references; caller-held tracebacks can still retain
their reader. `hints=None` keeps the original C entry point.

`LoadedImage.disassemble(config, hints=hints)` forwards the same contract and
adds caller entries to image metadata seeds. It snapshots hints synchronously,
destroys a failed intermediate graph before re-raising callback errors, and
does not retain readers in the returned Graph. Its default `hints=None` path
also uses the original C symbol. These new hint-routing features require the
updated shim; the Guest ABI is unchanged.
Reanalysis withdraws inferred edges across the Graph but rebuilds only the
supplied code region. Other regions' inferred targets do not recover
automatically. Incomplete can consequently report `changed=True` and removed
edges; reacquire views and do not interpret it as an unchanged or complete
deobfuscation result.

`graph.finite_values(entry_points=entries,
query_register=(block, offset, width))` queries a register at block end; omit
`query_register` to request only jump sets. `ValueSetKind` distinguishes finite,
unknown and unreachable values. Both APIs work without Z3. See
[finite dispatch](finite-dispatch.md) for complete entry contracts, limits and memory/call barriers.

```python
lifter.run_mem2reg()      # promotes allocas to SSA values
lifter.run_inst_combine() # folds sub-register mask/OR patterns
lifter.run_gvn()          # eliminates redundant loads
lifter.run_mba_simplification() # word-level equivalence proofs before rewrites
lifter.run_predicate_folding()  # constant branch/select proofs and CFG cleanup
lifter.run_dce()          # removes dead flag computations

# equivalent shorthand:
lifter.optimize()
```

### Method reference

| Method | Returns | Notes |
|---|---|---|
| `lift(function, graph)` | `bool` | `False` for invalid/missing blocks, duplicate symbols or unsupported Legacy transfers |
| `ir()` | `str` | Textual LLVM IR for all lifted functions |
| `run_mem2reg()` | — | |
| `run_inst_combine()` | — | |
| `run_gvn()` | — | |
| `run_mba_simplification(timeout_ms=100, max_queries=16, max_expression_nodes=128, max_variables=3, resource_limit=1000000)` | `MBASimplifyStats` | Every rewrite requires UNSAT; without Z3, unavailable/no-op |
| `run_predicate_folding(timeout_ms=25, max_queries=64, max_expression_nodes=256, resource_limit=200000)` | `PredicateFoldStats` | Bounded scalar integer branch/select proofs |
| `run_dce()` | — | |
| `optimize()` | — | Runs the pipeline above |

See [MBA scope, budgets and statistics](llvm-mba-simplification.md) and
[predicate folding](llvm-predicate-folding.md). Neither step assumes initialized
heap data or changes the Guest execution protocol.

---

## FLIRT signatures

`SignatureDB` loads FLIRT-style `.pat` signature databases and matches them
against disassembled bytes. Text `.pat` format only (the proprietary binary
`.sig` format is not supported).

`dragon.sigs_crc16(data)` exposes the raw CRC16 primitive used to verify
matches: FLAIR's CRC16 (reflected poly `0x8408`, init `0xFFFF`, complemented,
bytes swapped; `0` for empty input), i.e. the value in a `.pat` line's CRC field.

```python
db = dragon.SignatureDB()
db.load_directory('./signatures')   # every .pat file in the dir, non-recursive
print(db.signature_count)

# Single candidate: try every signature against code starting at 0x1000.
match = db.match_at(code, 0x1000)
if match:
    address, name, length = match
    print(f'{name} at 0x{address:x}, length {length}')

# Many candidates at once: every entry point against every signature.
# A module can bundle more than one function/offset under one pattern, so
# one entry point may produce more than one match in the result list.
for address, name, length in db.match_entries(image, image_base, [0x1000, 0x1040]):
    print(f'0x{address:x}: {name} (len {length})')
```

### Method reference

| Method | Returns | Notes |
|---|---|---|
| `SignatureDB()` | | |
| `load_directory(path)` | — | Every `.pat` file directly inside `path`, not recursive; unparseable files are silently skipped |
| `load_file(path)` | — | No-op if the file can't be parsed |
| `signature_count` | `int` | |
| `match_at(data, address)` | `(address, name, length)` or `None` | `data` must begin at the candidate function's address |
| `match_entries(image, image_base, entry_points)` | `list[(address, name, length)]` | |
| `dragon.sigs_crc16(data)` | `int` | Module-level function, not a `SignatureDB` method |

---

## Symbolic execution

Available when the library is built with `-DDRAGON_USE_Z3=ON`.

`SymbolicExecutor` runs lifted IGNIL over Z3 bitvectors: seed the machine, run
a block, then ask the solver what it computed. Registers are the
`(offset, width, name)` tuples from the `x86` / `arm` classes.

```python
import dragon
from dragon import x86

cfg = dragon.Configuration(strategy=dragon.Strategy.LINEAR_SWEEP)
dis = dragon.Disassembler(dragon.Architecture.X86_64, cfg)

#   xor rax, rdi ; add rax, rsi ; ret
graph = dis.analyze(bytes([0x48,0x31,0xF8, 0x48,0x01,0xF0, 0xC3]), 0x1000)
block = graph.get_ignil_block(0x1000)

with dragon.SymbolicExecutor() as ex:
    ex.set_register(x86.RAX, 0)
    a = ex.symbolize_register(x86.RDI, 'a')
    b = ex.symbolize_register(x86.RSI, 'b')

    result = ex.run(block)

    rax = ex.read_register(x86.RAX)
    print(ex.proves(rax == a + b))   # True
    print(ex.proves(rax == a * b))   # False
```

State persists across `run()` calls, so walking a path is a sequence of runs.

### Expressions

`SymExpr` operators build expressions rather than computing anything, so `==`
returns a `SymExpr` and not a `bool` — the same convention z3py uses. Plain
ints are widened to the other operand's width automatically.

```python
rax = ex.read_register(x86.RAX)

goal   = rax == 0x1337          # SymExpr, not a bool
masked = (rax & 0xFF) + 1
```

Comparison operators (`<`, `<=`, `>`, `>=`) are **signed**, matching Python's
own integer semantics. Use `ult()`, `ule()`, `ugt()`, `uge()` for the unsigned
versions, `logical_and()` / `logical_or()` / `implies()` to combine booleans,
and `~` for boolean negation.

> Expressions may safely outlive the executor that produced them. A
> `z3::expr` does not own its context, so each handle keeps the executor
> alive itself — which is what makes `SymExpr` objects safe to hold in
> globals, where Python finalizes in an order you do not control.

### Solving — recovering an input

A symbolic region of N bytes creates N variables named
`<region>[0]`..`<region>[N-1]`, which `Solution.bytes()` reads back in address
order.

```python
#   mov rax, [rdi] ; xor rax, rsi ; ret
with dragon.SymbolicExecutor() as ex:
    ex.set_register(x86.RDI, 0x4000)
    ex.set_register(x86.RSI, 0x5A5A5A5A5A5A5A5A)
    ex.symbolize_memory(0x4000, 8, 'flag')
    ex.run(block)

    solution = ex.solve(ex.read_register(x86.RAX) == ex.const(target, 64))
    if solution:
        print(solution.bytes('flag'))    # b'DRAGON!!'
        print(solution.values())         # every bound symbol
```

`solve()` returns `None` when no assignment exists — an unreachable goal is
reported, never guessed at. Narrow the search with `assume()` before running:

```python
for i in range(4):
    byte = ex.read_memory(0x5000 + i, 8)
    ex.assume(byte.uge(ex.const(0x21, 8)))    # printable ASCII only
    ex.assume(byte.ule(ex.const(0x7E, 8)))
```

### Control flow — resolving computed jumps

`RunResult.successors` names the concrete addresses control flow can reach,
each with the guard under which it is reached. A branch whose condition is
provably constant collapses to a single successor; a computed jump has its
target enumerated, which is what recovers a jump table or a VM dispatch.

```python
#   mov esi, [rdi] ; jmp qword ptr [rsi*8 + 0x6000]
with dragon.SymbolicExecutor() as ex:
    ex.map_memory(0x6000, handler_table)
    ex.set_register(x86.RDI, 0x7000)

    opcode = ex.symbolize_memory(0x7000, 4, 'vm_opcode')
    ex.assume(opcode.ult(ex.const(3, 32)))     # the VM's own range check

    result = ex.run(block)
    print(result.complete)                     # True
    print([hex(s.address) for s in result.successors])
    # ['0xaa00', '0xbb00', '0xcc00']
```

`result.complete` is the honesty check: True only when the run succeeded *and*
every address and branch target resolved exactly, so the successor list really
is the complete set. Otherwise `result.warnings` says what had to be left open:

```python
for w in result.warnings:
    print(f"@0x{w.address:x}: {w.message}")
# @0x1002: branch target has more than 256 possible values ...
```

### Modelling notes

- Registers and memory are byte-granular, so sub-register aliasing comes out
  right: `EAX` writes bytes 0–3, `AH` writes byte 1, and a `RAX` read
  concatenates bytes 0–7.
- A 32-bit write clears the upper half of a 64-bit register (x86-64 and
  AArch64). Pass `zero_extend_on_32bit_write=False` for a target where it
  merges instead.
- A symbolic address is concretized by enumerating its satisfying values, up
  to `max_address_solutions`; past that bound the access is reported
  unresolved rather than guessed at.
- Blocks stop at the first terminator. Pass `stop_at_terminator=False` to run
  a trace from `lift_trace()`, whose interior terminators must be stepped
  over.

### Concrete readback and OPAQUE ops

`concrete_register(reg)` and `concrete_memory(address, width)` return a plain
`int` when the location holds a literal, or `None` when it is still symbolic —
the cheap form of `evaluate(read_register(reg))` for a value expected to be
concrete, with no expression handle and no solver. `write_register(reg, expr)`
and `write_memory(address, expr)` store an expression rather than a host
integer (`write_memory` is little-endian, as wide as the expression).

An OPAQUE op — `CPUID`, `RDTSC`, `RDTSCP` — has a determinate value IGNIL
cannot express, so by default `run()` turns its results into fresh variables.
Pin them to keep a concolic trace concrete through an anti-tamper check:

```python
# CPUID(leaf=1, sub=0) -> fixed EAX/EBX/ECX/EDX
ex.set_concrete_cpuid(1, 0, 0x000906EA, 0, 0x7FFAFBFF, 0xBFEBFBFF)
```

`set_concrete_opaque(kind, selector, results)` is the general form for the
other kinds (`OPAQUE_CPUID`, `OPAQUE_RDTSC`, `OPAQUE_RDTSCP`), where `selector`
identifies the invocation from its operands.

### Snapshots and diffing

Copying state is far cheaper than rebuilding it, so an analysis that runs many
blocks from one seeded machine should seed once and rewind. `snapshot()`
returns a `SymState`, and `restore()` puts it back:

```python
with dragon.SymbolicExecutor() as ex:
    ex.symbolize_memory(0x4000, 16, 'input')
    ex.set_register(x86.RDI, 0x4000)
    seeded = ex.snapshot()

    for block in blocks:
        ex.restore(seeded)
        ex.run(block)
        ...
```

`diff(before)` reports every register and memory location whose value differs
from a snapshot, as a list of `StateDelta` (`kind` is `'reg'` or `'mem'`, `key`
the register offset or address, `width` in bits, `value` the `SymExpr` now
held). A run of bytes that came from one written value is reported as that
single value, so a 64-bit store is one delta and not eight.

```python
seeded = ex.snapshot()
ex.run(block)
for d in ex.diff(seeded):
    print(d.kind, hex(d.key), d.width, d.value)
```

`SymState.merge(incoming, tag)` joins another snapshot into this one, turning
every location the two disagree on into a fresh variable named after `tag`, and
returns the symbolised locations as a `StateDelta` list — an empty list is the
fixed-point signal for a worklist exploration. `set_fresh_tag(tag)` sets the
suffix for the variables `run()` invents for loads it cannot resolve, so the
same block run in two contexts can keep its fresh names apart.

### Unresolved accesses

When a symbolic address has more satisfying values than
`max_address_solutions`, the access cannot be expanded: the load produces a
fresh variable and the store is dropped, each with a warning.
`RunResult.unresolved` lists them as `UnresolvedAccess` objects (`is_store`,
`op_address`, `width`, `address`, `value`).

### Exporting an expression

Besides `to_string()`, a `SymExpr` can leave the library:

```python
print(expr.as_int())      # int if it folds to a <=64-bit literal, else None
print(expr.to_smt2())     # a complete SMT-LIB 2 script, declarations included
z3_term = expr.to_z3()    # a z3py term (needs the z3-solver package)
```

`to_smt2()` emits a self-contained script another Z3 binding can parse back
without being told the free variables; `to_z3()` does exactly that and hands
back the term.

### API summary

| Symbol | Description |
|---|---|
| `SymbolicExecutor(**options)` | Create an executor (see options below) |
| `set_register(reg, value)` | Seed a register concretely |
| `symbolize_register(reg, name)` → `SymExpr` | Bind a register to a fresh variable |
| `map_memory(addr, data)` | Map a concrete blob |
| `symbolize_memory(addr, size, name)` → `SymExpr` | Fill a region with per-byte variables |
| `assume(cond)` | Constrain the rest of the run |
| `run(block)` → `RunResult` | Execute a block; state persists |
| `read_register(reg)` / `read_memory(addr, width)` → `SymExpr` | What a location now holds |
| `const(value, width)` → `SymExpr` | A concrete bitvector |
| `proves(cond)` → `bool \| None` | Does it hold on every model? `None` = solver gave up |
| `is_satisfiable(cond)` → `bool \| None` | Can it hold at all? |
| `evaluate(expr)` → `int \| None` | Value, only if it is forced to exactly one |
| `enumerate(expr, max)` → `(list, bool)` | Distinct values, and whether that list is complete |
| `solve(goal)` → `Solution \| None` | A satisfying assignment |
| `write_register(reg, expr)` / `write_memory(addr, expr)` | Store an expression, not a host int |
| `concrete_register(reg)` / `concrete_memory(addr, width)` → `int \| None` | Literal value, or `None` if symbolic |
| `set_concrete_cpuid(leaf, sub, eax, ebx, ecx, edx)` | Pin a CPUID OPAQUE result |
| `set_concrete_opaque(kind, selector, results)` | Pin any OPAQUE result |
| `set_fresh_tag(tag)` | Suffix for auto-invented load variables |
| `snapshot()` → `SymState` / `restore(state)` | Copy / rewind the whole machine state |
| `diff(before)` → `list[StateDelta]` | Locations changed since a snapshot |
| `reset()` | Clear register, memory and constraint state |
| `RunResult.successors` | `Successor(address, taken, condition)` list |
| `RunResult.complete` | True only if that list is exhaustive |
| `RunResult.warnings` | `SymWarning(address, message)` list |
| `RunResult.unresolved` | `UnresolvedAccess(is_store, op_address, width, address, value)` list |
| `RunResult.ops_executed` / `.solver_queries` / `.addresses_concretized` | Run statistics |
| `Solution.value(name)` / `.values()` / `.bytes(region)` | Read the model back |
| `SymState.merge(incoming, tag)` → `list[StateDelta]` | Join another snapshot; `[]` is the fixed point |
| `SymExpr.as_int()` / `.to_smt2()` / `.to_z3()` | Fold to a literal / SMT-LIB 2 / z3py term |

Constructor options, all deferring to the library default when omitted:
`solver_timeout_ms` (5000), `max_address_solutions` (64),
`max_branch_targets` (256), `zero_extend_on_32bit_write` (True),
`symbolize_unmapped_reads` (True), `stop_at_terminator` (True).

`examples/python/symbolic_execution.py` runs all of the above against real
lifted x86-64.

---

## Enum reference

| Class | Values |
|-------|--------|
| `dragon.Architecture` | `UNKNOWN`, `X86_32`, `X86_64`, `ARM32`, `ARM64`, `RISCV32`, `RISCV64` |
| `dragon.Syntax` | `INTEL`, `ATT` |
| `dragon.LLVMExecutionMode` | `LEGACY`, `STATEFUL_V2`, `STATIC_ANALYSIS` |
| `dragon.LLVMStaticEntryPolicy` | `RESUMABLE`, `FUNCTION_START` (static mode only) |
| `dragon.PassStage` | `GRAPH`, `LLVM` |
| `dragon.PassStatus` | `SUCCESS`, `UNAVAILABLE`, `INCOMPLETE`, `ERROR` |
| `dragon.Strategy` | `LINEAR_SWEEP`, `RECURSIVE_TRAVERSAL` |
| `dragon.SliceExprType` | `CONST`, `SYM_REG`, `BINOP`, `UNARYOP`, `LOAD`, `UNDEF` |
| `dragon.IGNILOpcode` | `COPY`, `LOAD`, `STORE`, `ADD`, … `UNDEF`, `OPAQUE`, floating-point and local control ops, `UNSUPPORTED` (73 values) |
| `dragon.Format` | `UNKNOWN`, `ELF`, `PE` |
| `dragon.SymbolKind` | `UNKNOWN`, `FUNCTION`, `OBJECT`, `SECTION`, `FILE` |
| `dragon.SymExecStatus` | `OK`, `MALFORMED_OP`, `UNSUPPORTED_OP`, `SOLVER_FAILURE` |
| `dragon.SymExit` | `FALLTHROUGH`, `JUMP`, `CONDITIONAL_JUMP`, `CALL`, `RETURN`, `ERROR` |
| `dragon.SwitchCandidate` | `table_base`, `entry_size`, `num_cases`, `index_reg_offset`, `index_reg_width` (plain data, not an enum) |
| `dragon.SimplifyStats` | `constants_folded`, `identities_applied`, `ops_removed`, `iterations`, `changed` (plain data, not an enum) |
| `dragon.Section` | `name`, `address`, `virtual_size`, `readable`, `writable`, `executable`, `data` (plain data, not an enum) |
| `dragon.Symbol` | `name`, `address`, `size`, `kind`, `imported`, `exported` (plain data, not an enum) |
| `dragon.StateDelta` | `kind`, `key`, `width`, `value`, `is_register` (plain data, not an enum) |
