# 6. Moving to LLVM IR

IGNIL (chapter 5) is architecture-independent, but it's still a bespoke IR
with a small, fixed opcode set — there's no optimizer for it. `LLVMLifter`
closes that gap: it translates a `Function`'s IGNIL blocks into real LLVM
IR, so you get everything LLVM's optimization pipeline already knows how to
do — constant folding, dead code elimination, SSA construction — applied to
lifted machine code.

## The `LLVMLifter` class

```cpp
#include <dragon/lifters/llvm/LLVMLifter.hpp>

dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::ARM64, "foo_module"};

bool ok = lifter.lift(function, graph);   // function's blocks must all have IGNIL translations
if (ok)
    std::cout << lifter.ir();             // textual LLVM IR
```

```python
lifter = dragon.LLVMLifter(dragon.Architecture.ARM64, "foo_module")
ok = lifter.lift(fn, graph)
print(lifter.ir())
```

`lift()` returns `false` if any block reachable from the function is
missing an IGNIL translation — the lifter refuses to produce a function
with a gap in it rather than silently emitting wrong code.

One lifter can hold multiple distinct functions. Re-lifting an existing symbol
returns `false`; it does not silently create a renamed copy. Choose an execution
mode before the first function is lifted.

### Stateful V2 execution

The default Legacy interface rejects cross-boundary or dynamic jumps that its
two-pointer protocol cannot report. To retain runtime control-flow exits, opt in:

```cpp
lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2);
```

```python
lifter = dragon.LLVMLifter(
    dragon.Architecture.X86_64,
    execution_mode=dragon.LLVMExecutionMode.STATEFUL_V2)
```

V2 emits `void @dragon_v2_<arch>_<hex-address>(state, memory, runtime, exit)`.
The shared C types are in `dragon/lifters/llvm/Execution.h`; C++, C shim and
Python use the same layout. After compiling/JIT-loading the IR, register each
supported Guest entry address with its four-pointer host implementation and run
`executeV2` (C: `dragon_llvm_execute_v2`). Missing targets and exhausted budgets
produce explicit exits with the synchronized Guest PC; they are not `unreachable`.
See [the execution contract](../llvm-execution-contract.md) for ownership,
registration, memory and state requirements.

V2 implements near x64/i386 CALL/RET, including i386 word forms, and ordinary
A64 BL/BLR/RET Xn. Register all hexadecimal addresses in `dragon.entrypoints`
so Guest returns resume the actual continuation with fresh state. Known import
origins from a loaded image appear in `!dragon.imports` metadata. Names do not
provide external implementations. Legacy rejects CALL, including reachable
trace calls. Complete execution and boundary details are described
[below](#executing-guest-functions-with-statefulv2).

## The calling convention

Legacy lifted functions have the signature `void @sub_<address>(ptr %mem, ptr %regs)`
(the name is the function's start address in **hexadecimal** — `foo`
at `0x1000` becomes `@sub_1000`). Two opaque pointers carry all lifted
state:

- **`%regs`** — the same flat register address space from chapter 5's
  `Reg{offset, width}` values. Reading `X0`/`W0` at the start of the
  function is a `getelementptr` on `%regs` at offset 0, exactly matching
  `dragon::arm::X0`'s offset.
- **`%mem`** — the address space `LOAD`/`STORE` IGNIL ops read and write
  through, via `getelementptr` + `load`/`store` at the computed address.

Only registers the function *actually touches* get an `alloca` — the
lifter's first pass (`buildRegFile`) scans every IGNIL op in every block
and allocates one `alloca` per `(offset, width)` pair actually referenced,
not one for every architectural register.

## Optimization passes

`LLVMLifter` bundles four passes, meant to run in this order (or all at
once via `optimize()`/`run_mem2reg()` + friends in Python):

| Pass | Effect |
|---|---|
| `runMem2Reg()` | Promotes the register-file `alloca`s to SSA values, inserting `phi` nodes at block merges. This is what turns "read/write a stack slot" into real dataflow. |
| `runInstCombine()` | Folds arithmetic/comparison patterns — notably the mask/OR sequences IGNIL's sub-register handling produces. |
| `runGVN()` | Eliminates redundant loads (global value numbering) — e.g. re-reading `%mem[SP]` twice in a row collapses to one load. |
| `runDCE()` | Removes now-dead computations — very relevant here, since IGNIL always computes *all four* condition flags per comparison (chapter 5), and DCE deletes the ones the function never actually branches on. |

## Watching it optimize: the running example

Lifting the same ARM64 `foo(x)` function from chapters 3-5 and printing
`ir()` before and after `optimize()`:

```python
fn = graph.get_function(0x1000)
lifter = dragon.LLVMLifter(dragon.Architecture.ARM64, "foo_module")
lifter.lift(fn, graph)

print(lifter.ir())      # non-optimized
lifter.optimize()
print(lifter.ir())      # optimized
```

**Non-optimized** — a near-literal transcription of chapter 5's IGNIL, one
`alloca` per touched register, explicit loads/stores for every access:

```llvm
define void @sub_1000(ptr %mem, ptr %regs) {
bb_4096:
  %W0 = alloca i32, align 4
  %FP = alloca i64, align 8
  %LR = alloca i64, align 8
  %SP = alloca i64, align 8
  %NF = alloca i8, align 1
  %ZF = alloca i8, align 1
  %CF = alloca i8, align 1
  %VF = alloca i8, align 1
  %0 = getelementptr i8, ptr %regs, i32 0
  %1 = load i32, ptr %0, align 4
  store i32 %1, ptr %W0, align 4
  ; ... FP/LR/SP/NF/ZF/CF/VF loaded from %regs the same way ...
  %16 = load i64, ptr %SP, align 4
  %17 = add i64 %16, -16
  %18 = getelementptr i8, ptr %mem, i64 %17
  %19 = load i64, ptr %FP, align 4
  store i64 %19, ptr %18, align 4          ; stp x29,x30,[sp,#-16]! (first store)
  %20 = add i64 %17, 8
  %21 = getelementptr i8, ptr %mem, i64 %20
  %22 = load i64, ptr %LR, align 4
  store i64 %22, ptr %21, align 4          ; (second store)
  store i64 %17, ptr %SP, align 4
  %23 = load i64, ptr %SP, align 4
  %24 = add i64 %23, 0
  store i64 %24, ptr %FP, align 4          ; mov x29, sp
  %25 = load i32, ptr %W0, align 4
  %26 = sub i32 %25, 0                     ; cmp w0, #0
  %27 = icmp slt i32 %26, 0
  %28 = zext i1 %27 to i8
  store i8 %28, ptr %NF, align 1
  %29 = icmp eq i32 %26, 0
  %30 = zext i1 %29 to i8
  store i8 %30, ptr %ZF, align 1
  ; ... CF, VF computed and stored the same way ...
  %41 = load i8, ptr %ZF, align 1
  %42 = icmp ne i8 %41, 0
  %43 = load i8, ptr %NF, align 1
  %44 = load i8, ptr %VF, align 1
  %45 = icmp ne i8 %43, %44
  %46 = or i1 %42, %45                     ; Z==1 || N!=V  (the LE condition)
  br i1 %46, label %bb_4120, label %bb_4112

bb_4112:                                   ; preds = %bb_4096   (fallthrough: mov w0,#1 / b end_branch)
  store i32 1, ptr %W0, align 4
  br label %bb_4124

bb_4120:                                   ; preds = %bb_4096   (else_branch: mov w0,#-1)
  store i32 -1, ptr %W0, align 4
  br label %bb_4124

bb_4124:                                   ; preds = %bb_4120, %bb_4112   (end_branch)
  ; ldp x29,x30,[sp],#16 : two loads mirroring the stp above, then SP advances
  ; ... then every register written back to %regs before `ret void`
  ret void
}
```

**Optimized** (`mem2reg` → `instcombine` → `GVN` → `DCE`) — the `alloca`s
are gone, replaced by SSA values and a `phi`, and DCE has pruned every
flag computation the function doesn't actually use:

```llvm
define void @sub_1000(ptr %mem, ptr %regs) {
bb_4096:
  %0 = load i32, ptr %regs, align 4
  %1 = getelementptr i8, ptr %regs, i64 232
  %2 = load i64, ptr %1, align 4
  %3 = getelementptr i8, ptr %regs, i64 240
  %4 = load i64, ptr %3, align 4
  %5 = getelementptr i8, ptr %regs, i64 256
  %6 = load i64, ptr %5, align 4
  %7 = add i64 %6, -16
  %8 = getelementptr i8, ptr %mem, i64 %7
  store i64 %2, ptr %8, align 4
  %9 = getelementptr i8, ptr %mem, i64 %6
  %10 = getelementptr i8, ptr %9, i64 -8
  store i64 %4, ptr %10, align 4
  %11 = icmp slt i32 %0, 1
  br i1 %11, label %bb_4120, label %bb_4112

bb_4112:
  br label %bb_4124

bb_4120:
  br label %bb_4124

bb_4124:
  %W0.0 = phi i32 [ -1, %bb_4120 ], [ 1, %bb_4112 ]
  %12 = icmp eq i32 %0, 0
  %.lobit = lshr i32 %0, 31
  %13 = trunc nuw nsw i32 %.lobit to i8
  %14 = zext i1 %12 to i8
  store i32 %W0.0, ptr %regs, align 4
  store i64 %2, ptr %1, align 4
  store i64 %4, ptr %3, align 4
  store i64 %6, ptr %5, align 4
  %15 = getelementptr i8, ptr %regs, i64 784
  store i8 %13, ptr %15, align 1
  %16 = getelementptr i8, ptr %regs, i64 785
  store i8 %14, ptr %16, align 1
  %17 = getelementptr i8, ptr %regs, i64 786
  store i8 1, ptr %17, align 1
  %18 = getelementptr i8, ptr %regs, i64 787
  store i8 0, ptr %18, align 1
  ret void
}
```

This is the payoff of the whole pipeline: LLVM's optimizer, given no
information except the IGNIL translation of nine ARM64 instructions,
independently rediscovered that `foo(x)` is `x > 0 ? 1 : -1` and collapsed
the entire two-branch comparison down to one `icmp slt i32 %0, 1` feeding a
`phi`. It even worked out that, because the comparison is always against
`0`, `CF` and `VF` are *constant* on every path (`store i8 1`/`store i8 0`)
rather than needing to be computed — something visible nowhere in the
original assembly, only in the optimized IR.

## Working with the `llvm::Module` directly (C++ only)

`module()` exposes the underlying `llvm::Module&` for anything `ir()`
doesn't cover — running your own custom pass, JIT-compiling it with LLVM's
ORC APIs, or emitting object code:

```cpp
llvm::Module& mod = lifter.module();
// ... pass mod to your own llvm::PassBuilder pipeline, an ORC JIT, etc.
```

This is C++-only; the Python/C API only exposes the rendered IR text via
`ir()`.

## Executing Guest functions with StatefulV2

The default Legacy interface keeps `void sub_<addr>(mem, regs)` and the trace
API. It refuses CALL and transfers it cannot report. For complete Guest
control flow, opt in before the first lift:

```python
lifter = dragon.LLVMLifter(
    image.architecture,
    execution_mode=dragon.LLVMExecutionMode.STATEFUL_V2)
assert lifter.lift(function, graph)
```

V2 emits `void dragon_v2_<arch>_<addr>(state, memory, runtime, exit)`. All target
implementations use this same four-pointer protocol, including external models.
The shared C types are in `dragon/lifters/llvm/Execution.h`; C++ aliases and the
runtime are in `Execution.hpp`, and C exposes `dragon_llvm_execute_v2`.
Guest memory is a bounded flat mapping, and the caller owns all buffers and
target implementations. This mode currently requires a 64-bit little-endian host.

Compile or JIT the emitted module, then register each address in its
`dragon.entrypoints` attribute to that implementation. These include exact CALL
continuations; registering only the function start loses the resume entries.
`executeV2` dispatches using the PC in the Guest register file and reports
Return, NeedTarget, Unsupported or Budget explicitly. A Guest RET only ends the
root execution when its actual target equals the supplied root exit address.
A missing target can be registered and resumed from the committed PC; a CALL
must not be executed a second time after its stack/LR effects have committed.

Near x86 CALL/RET models the Guest stack and RET imm16, including i386 word
forms; A64 BL/BLR updates X30 and RET uses its specified register. There is no
assumed callee ABI: models can modify every Guest register and memory byte.
Far/system/authenticated transfers and x64 66-prefixed CALL/RET remain
Unsupported. An import name never invokes a host function automatically.
See [the execution contract](../llvm-execution-contract.md) for exact layouts,
preconditions, boundaries and unsupported conditions.

## Scope

Same as chapter 5 — `LLVMLifter` consumes whatever `getIGNILBlock()` can
produce, so it inherits IGNIL's scope: x86 (32/64) and ARM64 only. A
`Function` with a block missing an IGNIL translation (ARM32, RISC-V, or a
gap from an unsupported addressing mode) makes `lift()` return `false`
rather than emit a function with a hole in it.

---
Previous: [5. Lifting to IGNIL](05-lifting-to-ignil.md) · Next: [7. A Real-World Example](07-a-real-world-example.md)
