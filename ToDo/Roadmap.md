# dragon-tales — Roadmap / session continuity notes

Purpose: pick up exactly where this left off in a future session, without
re-deriving context from scratch. Written 2026-08-08.

## Where this came from

A comparison against NeverD (a much larger, more mature sibling
binary-analysis/decompilation engine) and a discussion of rev.ng's approach
produced three agreed priorities, in this order:

1. Improve IGNIL instruction coverage (fix known lifter gaps).
2. Correctness infrastructure: emulation-based semantic round-trip testing,
   an opcode-coverage audit tool, and a CI pipeline.
3. Function/signature identification (FLIRT-style) + better function-start
   detection + a CFG-agnostic trace lifter.

Parameter recognition / calling-convention-based function reconstruction
(recovering typed LLVM function signatures instead of the current generic
`void f(i8* mem, i8* regs)`) was raised as a future priority #4 but **not
started yet** — see "Not started" below.

## Done, in commit order (branch `dev`, formerly `improvements`)

1. **x86 memory-operand lifter gaps fixed** (`ToDo/LifterGaps.md` #1):
   `mov [mem],imm`, RMW `add/sub [mem],reg|imm`, `cmp reg,[mem]`, `cmp
   [mem],reg|imm`, and the reg,mem direction `add/sub reg,[mem]` (found by
   the coverage tool below, not in the original bug report). Plus a generic
   safety net: any still-unhandled opcode with a register-def operand now
   emits `UNDEF` instead of silently producing zero ops.
2. **`tool/opcode_audit` + `scripts/audit_x86_lifter_coverage.py`**: cross-
   references the lifter's dispatch table against LLVM's full X86 opcode
   list for the ALU-family mnemonics (MOV/ADD/SUB/AND/OR/XOR/CMP/TEST/
   ADC/SBB). Currently 32/64-bit rr/ri/ri8/rm/mr/mi/mi8 forms are covered
   for MOV/ADD/SUB/CMP; **AND/OR/XOR/TEST/ADC/SBB memory forms and all
   8/16-bit forms are still gaps** — run the script to see the live list.
3. **`.github/workflows/ci.yml`**: installs LLVM 21 via apt.llvm.org, Z3,
   Unicorn (best-effort), builds with Ninja, runs `ctest`, runs the
   coverage script as an informational (non-blocking) step. Confirmed
   working on GitHub after fixing the branch-name mismatch (workflow only
   triggered on `main`/`dev`; the working branch was `improvements` and got
   renamed to `dev`).
4. **`tests/semantic/` — Unicorn-based semantic round-trip harness**:
   executes raw bytes under real Unicorn emulation and under the actual
   lift→LLVM-IR→ORC-JIT pipeline, diffs full register+memory state.
   `DRAGON_USE_UNICORN` CMake option, degrades gracefully if
   `libunicorn-dev` isn't installed (unlike `DRAGON_USE_Z3`, which is hard
   `REQUIRED`).
5. **Function-start detection**: `X86Analysis::analyze()` and
   `ARMAnalysis::analyze()` now seed the caller's `baseAddr` as an
   unconditional function start (previously only prologue matches and call
   targets counted). Fixes `LifterGaps.md` #3 (ARM64 frame-pointer-omitted
   leaf functions) and the x86 equivalent. Deliberately did **not** broaden
   the byte-level prologue pattern scan itself (false-positive risk from
   scanning every instruction position) — see "Not started" below if that
   turns out to still be needed.
6. **`dragon::sigs` — FLIRT-compatible `.pat` signature matching**:
   `Signature`/`PatternParser`/`SignatureDB`/`SignatureMatcher`. Text `.pat`
   format only. **Caveat carried forward: the CRC16 variant (reflected,
   poly 0xA001, init 0xFFFF) was never verified against a real
   IDA/FLAIR-generated `.pat` file** — no such file or network access was
   available. Self-consistent and tested end-to-end with its own computed
   CRCs, but if a real signature database is loaded and nothing ever
   matches, check this first.
7. **`Disassembler::liftTrace()`**: lifts an arbitrary linear instruction
   sequence straight to IGNIL, bypassing `X86Analysis`/`ARMAnalysis`
   entirely (no CFG/function/block validity checks, no synthesized
   fall-through terminator). Backed by `X86IGNILLifter::liftTrace` /
   `ARMIGNILLifter::liftTrace`, which just loop `liftInstruction` over the
   span. Intended for captured execution traces (Unicorn/QBDI/Frida/Intel
   PT) feeding `BackwardSlicer`/Z3 directly.
8. **Wired `dragon::sigs` and `liftTrace` into the C shim + Python API**
   (`c_shim/dragon_c.h`/`.cpp`, `python/dragon.py`). Mechanical exposure of
   already-tested C++ functionality, not new logic:
   - `sigs`: `dragon_sigdb_*` (opaque `SignatureDB` handle: create/destroy,
     `load_directory`/`load_file`, `signature_count`), `dragon_sigs_crc16`
     (exposes `SignatureMatcher::computeCRC16` directly — useful for
     verifying the CRC16 caveat from item 6 against a real `.pat` file
     without needing a full match), `dragon_sigdb_match_at` (single result,
     via out-params + an owned name string), `dragon_sigdb_match_entries`
     (multiple results, via callback, mirroring the `each_*` iteration
     pattern used elsewhere in the shim). **Scope cut:** `PatternParser`
     (`parseLine`/`parseFile`) and `SignatureDB::addSignatures` were not
     exposed — `SignatureDB::loadFile`/`loadDirectory` already cover the
     practical FLIRT workflow (load a `.pat` database, match against
     disassembled code), and programmatic signature construction wasn't
     asked for.
   - `liftTrace`: `dragon_disasm_lift_trace` takes an array of borrowed
     `dragon_instruction_t*` (e.g. from `Graph.instructions()` /
     `dragon_graph_each_instruction` — reuses already-decoded instructions
     rather than re-decoding raw bytes) and returns a **heap-owned**
     `dragon_ignil_block_t*`, unlike every other use of that type in the
     shim (which are borrowed views into a `Graph` and must never be
     destroyed). New `dragon_ignil_block_destroy` is only valid on
     trace-lifted blocks — calling it on a `Graph`-borrowed block would be a
     double-free/use-after-free. Documented prominently in the header at
     both declaration sites. Python's `IGNILBlock` gained an `owned` flag
     (default `False`, preserving old borrowed behavior) so
     `Disassembler.lift_trace()` can return the same class with correct
     `__del__` semantics instead of needing a parallel owned-block class.
   - Verified via `ctest` (111/111 still pass, no regressions) and a
     from-scratch Python smoke script exercising both features end-to-end
     (disassemble → `lift_trace` → render IGNIL; build a `.pat` line →
     `SignatureDB.load_file` → `match_at`/`match_entries`) — see git log for
     the commit, script itself was scratch and not kept in the repo.
   - Also fixed `README.md`'s feature bullet, which had drifted out of sync
     (claimed sigs was C++-only right above a bullet claiming the shim
     exposed "the full feature set").

9. **`LLVMLifter::liftBlock()`** (`include/dragon/lifters/llvm/LLVMLifter.hpp`
   / `lib/LLVMLifter.cpp`): lifts a single, already-flattened `ignil::Block`
   directly to LLVM IR with no `Graph`/`Function` needed — the Graph-free
   counterpart to `Disassembler::liftTrace()`, which had no way to reach
   `LLVMLifter` before this (its `lift()` fundamentally requires a `Graph`,
   which can't represent a temporal trace that revisits the same address
   many times — `Graph`'s instruction listing is keyed by unique address).
   JMP/CJMP ops are dropped (a linear resolved trace has nothing left to
   branch on — the op order already *is* the path taken); RET, if present,
   must be the block's true terminator, otherwise a synthetic `ret void`
   with the usual register write-back is appended. Refactored
   `buildRegFile`/`liftOp`'s RET case into shared `collectRegsFromOps`/
   `allocateRegFile`/`emitReturn` helpers so `liftBlock` reuses them instead
   of duplicating logic.
10. **`tests/toy-vm/`** — a hand-written, deliberately MBA-obfuscated toy
    bytecode VM (assembled via `dragon::Assembler`, not hand-encoded bytes),
    built as the first, smallest possible end-to-end proof-of-concept for
    the planned VM-devirtualization pipeline: Unicorn captures a real
    execution trace through the interpreter → `Disassembler::liftTrace()` →
    `LLVMLifter::liftBlock()` → real LLVM optimization passes
    (mem2reg/instcombine/GVN/DCE). **It works**: raw lift 2300 IR
    instructions → optimized 74 (~31×), and the optimized IR shows the
    literal recovered MBA identity (`xor`/`and`/`shl`/`add` = the textbook
    `(a^b)+2*(a&b)`) applied directly to the original pushed operands, with
    the entire dispatch loop gone — both the raw and optimized lifts
    JIT-execute to the correct answer. 4 `ctest` cases
    (`ctest -R toy-vm` or Catch tag `[toy-vm]`), each isolating one pipeline
    stage so a future regression points at the right one.
    - **Found and fixed three real, previously-undiscovered `LLVMLifter`
      correctness bugs** while building this (all applied to the core
      library, not toy-vm-local workarounds):
      1. `liftOp`'s SHL/SHR/SAR cases built a shift instruction with
         mismatched LLVM operand types when the shift-count's IGNIL width
         differs from the shifted value's (the common case on real ISAs —
         x86 shift-by-immediate/CL is always an 8-bit count regardless of
         operand width) — produced IR that couldn't even be parsed back in.
         Fixed by zext/trunc-ing the shift count to match.
      2. `mergeIntoParent` preserved the high 32 bits of a 64-bit register
         on a 32-bit sub-register write, when x86-64 actually
         zero-extends on that specific transition (`xor ecx,ecx` must
         clear all of RCX, not just ECX — unlike 8/16-bit sub-register
         writes, which correctly don't touch the rest of the register).
         This one silently corrupted results rather than failing loud —
         found by tracing a wrong-answer bug back through raw, unoptimized
         IR by hand.
      3. `resolve()`'s Const-to-`llvm::APInt` conversion didn't mask the
         value to its declared width — `cmp eax, 0xFFFFFFFF` decodes to
         `Const{width=32, value=0xFFFFFFFFFFFFFFFF}` (sign-extended, never
         masked back down), and `llvm::APInt`'s constructor *asserts* the
         value fits the given bit width. Invisible in a Release/NDEBUG
         build (silently truncates) — reliably `SIGABRT`s an
         assertions-enabled build. **This is exactly how it was actually
         found**: it passed in this project's own Release-configured
         `build-verify/` (assertions stripped) but crashed immediately in
         the user's plain/no-`CMAKE_BUILD_TYPE` `build/` (assertions on) —
         a good reminder to occasionally test in an assertions-enabled
         build, not just whichever one happens to be configured. Fixed
         defensively at this one choke point (every `ignil::Const` passes
         through `resolve()`); the upstream sign-extension bug in whichever
         X86IGNILLifter immediate-decoding path produces the unmasked
         value is still real and un-root-caused.
      None of the three had ever been caught by `tests/semantic/`, since
      none of its 6 existing cases exercise a shift, a 32-bit write
      followed by a 64-bit read of the same register, or a comparison
      against a large/negative-looking hex immediate.
    - **Found, but deliberately did NOT fix (redesigned the toy VM around
      them instead), 5 real X86IGNILLifter coverage gaps** — each one
      silently emits `UNDEF` (the LifterGaps.md #1 failure shape) for
      *any* form of: **INC/DEC**, **MOVZX**, **NEG**, **SHL/SHR/SAR by an
      immediate** (as opposed to by `CL` — untested), and **8-bit MOV**
      (load or store). Also observed: plain MOV/CMP through **base+index
      (SIB) addressing with an implicit ×1 scale** (e.g. `[rsi+r8]`)
      produced `UNDEF` even though the equivalent ×4-scale form
      (`[rdi+rcx*4]`) lifted correctly — the covered/gap boundary for SIB
      forms is narrower than `ToDo/Roadmap.md`'s existing MOV/ADD/SUB/CMP
      coverage notes describe; not root-caused further. None of these were
      previously listed anywhere (`scripts/audit_x86_lifter_coverage.py`
      only tracks ALU-family memory-operand/width forms, not these
      mnemonics/addressing modes at all). Full detail and the exact
      instruction sequences that trip each gap are in
      `tests/toy-vm/ToyVM.hpp`'s file header comment.
11. **`tests/toy-vm/TestToyVMHandlerRecovery.cpp` + `ToyVMHarness.hpp`** —
    a materially different (and more honest) technique than item 10's
    whole-trace flattening, prompted by a direct question about how this
    compares to the back.engineering posts' guided symbolic execution.
    Item 10 flattens one whole-*program* trace into one function, which is
    only provably correct for the exact path traced — not the blog posts'
    technique at all (those lift the *static* interpreter with symbolic
    state and discover the CFG by resolving branch targets; a single
    concrete trace has no unresolved branch to discover). This item is a
    step closer, still short of full guided symbolic execution: since the
    VM's structure is known (dispatch `je` targets = handler entry points,
    read via `Instruction::branches()`, not hardcoded), one whole-program
    trace is *segmented* into one episode per handler (dispatch
    fetch+compare chain discarded), and each handler is lifted to its own
    function independently. Shared pipeline plumbing (assemble/run/trace/
    lift/JIT, now used by 3 files) was consolidated out of `ToyVM.hpp` into
    a new `ToyVMHarness.hpp` in the same pass — duplicating it a third time
    stopped being reasonable.
    - **Verified against fresh inputs that never appeared in the original
      trace capture** (not the original a=10/b=4/c=3), which is the actual
      point: a whole-trace-flattened function's correctness says nothing
      about inputs it didn't see, but each of these 7 recovered functions
      computes the right answer for new numbers — real evidence of
      recovering generic per-opcode semantics, not memorizing one
      execution. 96 assertions, all passing.
    - **Result is stronger than item 10's**: `op_and`/`op_or`/`op_xor`'s
      MBA identities collapse *completely* to a single native
      `and`/`or`/`xor` instruction (not just a shorter equivalent chain).
      `op_add`/`op_sub` reduce to a short `xor`/`and`/`shl`/`add` chain
      rather than one native `add`/`sub` — LLVM InstCombine recognizes the
      AND/OR/XOR identity shape but not the ADD/SUB one this VM uses,
      apparently a canonicalization gap in LLVM itself rather than
      anything dragon-tales did; each handler still shrank from ~15-40 raw
      instructions to ~10-20 optimized, and both add/examples show the
      exact `(a^b)+2*(a&b)` shape now applied to genuinely symbolic
      per-call inputs rather than one recorded run's values.
    - New demo: `examples/cpp/toy_vm_handler_recovery.cpp` (companion to
      item 10's `toy_vm_devirtualization.cpp`) prints the segmentation, all
      7 recovered functions, and the fresh-input verification.
    - **Still not guided symbolic execution**: still needs one *concrete*
      run per handler (no CFG discovery over unresolved/symbolic
      branches), and a handler with its own internal data-dependent branch
      would still only have one arm recovered from a single trace. See
      `ToyVMHarness.hpp`'s header comment for the full comparison.
12. **Stack-pointer concretization experiment**, added directly to
    `examples/cpp/toy_vm_handler_recovery.cpp` only (no core library
    changes) — prompted by the Tencent/Themida posts' "concretize RSP to
    an arbitrary constant before lifting, reverse it before lowering"
    preparation step. Two small local passes:
    - `concretizeStackPointer()` (IGNIL level): prepends a synthetic
      `RDI = Const(value)` op to a handler's block before lifting — no new
      dragon-tales API needed, `ignil::Block::append()` already suffices.
    - `rewriteStackPointerBack()` (LLVM IR level): after optimization,
      finds every load/store address and the RDI register write-back that
      resolved to the concretized literal (via
      `Value::stripAndAccumulateConstantOffsets`, which walks an entire GEP
      chain to its base) and rewrites them back in terms of a
      freshly-reconstructed real `RDI` parameter. Deliberately targets only
      the two patterns this VM's handlers produce, not a general pass.
    - **Empirical result, worth remembering**: round-tripping through
      concretize → optimize → rewrite-back reproduces IR structurally
      identical to the plain (never-concretized) per-handler recovery — for
      a *single isolated handler*, LLVM's default `BasicAA` already proves
      "same symbolic base + different constant offset ⇒ no alias" on its
      own, so concretization adds nothing yet. It would matter once
      *composing multiple handlers symbolically* (the actual missing piece
      below), where offsets stop sharing one obvious common base and
      `BasicAA`'s reasoning alone isn't enough.
    - Verified against the same fresh inputs as item 11, all 7 handlers
      round-trip correctly (one real bug caught and fixed while building
      this: the rewrite computed offsets against the detection window's
      start rather than the actual concretized value — wrong addresses,
      caught immediately by the fresh-input checks).
    - **The precise remaining gap to real guided symbolic execution**,
      sharpened by this experiment: every technique built so far (items
      10-12) still needs *something concrete* to anchor it — a real trace,
      a hand-identified handler boundary, or an arbitrary-but-fixed RSP.
      The posts' actual engine removes that anchor entirely: a worklist
      that lifts the *static* interpreter with mostly-unknown state and
      *discovers* where to go next by constant-folding an indirect branch
      target until it resolves, no execution required. Nothing here
      attempts that yet — it's a CFG-discovery algorithm, not an
      optimization pipeline, and is the next thing that would make this
      look like the blog posts rather than adjacent to them.

13. **`ignil::simplifyBlock()` - IGNIL-level peephole simplifier**
    (`include/dragon/lifters/ignil/IGNILSimplify.hpp` /
    `lib/IGNILSimplify.cpp`). A "code cleaner" pass over the IGNIL `Op`
    stream itself, not the derived `slice::Expr` tree - deliberate choice:
    `BackwardSlicer::buildFromOp` builds `slice::Expr` straight from
    whatever `Op`s are in a block, so simplifying at the IGNIL level makes
    `slice::Expr`/`SwitchDetector`/`SliceExprMatch` output cleaner for free,
    on top of shrinking the LLVM IR `LLVMLifter::liftBlock` produces before
    LLVM's own optimizer even runs. Functional (`const Block& -> Block`),
    not in-place - no changes to `Block`'s existing API. Runs to a fixed
    point (bounded iteration count) over two passes per iteration:
    - Forward constant fold + **general copy propagation** (any
      `Temp = COPY(v)`, not just `Temp = COPY(Const)` - needed for e.g.
      `NEG(NEG(x))` to fully collapse to one `COPY(x)` instead of leaving a
      two-hop copy chain) + algebraic identity/annihilator elimination
      (`x+0`, `x*1`, `x&-1`, `x|0`/`x|-1`, `x^0`, shift-by-0, `x*0`/`x&0`) +
      double-negation collapse (`NEG(NEG(t))`/`NOT(NOT(t))`, Temp operand
      only) + no-op `ZEXT`/`SEXT`/`TRUNC` (same width) elimination + the
      half-adder identity `ADD(XOR(p,q), 2*AND(p,q)) -> ADD(p,q)` (matches
      either `SHL(and,1)` or self-`ADD(and,and)` as the "×2" step - real
      x86 lifts tend to produce the latter, e.g. `add eax,eax` rather than
      `shl eax,1`).
    - Backward dead-Temp elimination. `Reg`-defining ops are **never**
      removed even when apparently unused in-block, since a `Reg` may be
      live-out across the block boundary - this pass is intentionally
      block-local only, same boundary `BackwardSlicer` already stops at.
    Not auto-invoked inside `disassemble`/`liftTrace`/`liftBlock` - opt-in
    only, to avoid silently changing output existing tests already assert
    on. 18 test cases (`tests/analysis/TestIGNILSimplify.cpp`, 77
    assertions), 135/135 total tests passing in both build dirs after
    adding it, no regressions.
    - **Correction to this item's own first draft**: originally claimed
      MBA-identity recognition was "already handled post-lift by LLVM's own
      InstCombine per the toy-vm results" - **this was wrong**, caught in
      the same session by testing it directly on real generated code.
      Verified with a minimal standalone repro run through `opt-21 -O3`
      (full pipeline, not just `-passes=instcombine`): LLVM folds
      `(a|b)-(a^b) -> a&b` and `a+(-b) -> a-b` just fine, but does **not**
      fold `(a^b)+2*(a&b) -> a+b` even in already-clean SSA form with zero
      memory/register-file noise (confirmed by inspecting `op_add`'s
      already-`mem2reg`/`instcombine`/`gvn`/`dce`-optimized IR from this
      demo - the `xor`/`and`/`shl`/`add` chain sits there untouched, fully
      exposed, register-file round-tripping already eliminated by GVN+DCE's
      BasicAA reasoning). This is why the half-adder rule above was added
      to the IGNIL simplifier instead of being left to LLVM - `op_and`'s
      handler uses the *other* identity (subtraction-based), which LLVM
      *does* already fold, which is what made the asymmetry easy to miss
      the first time.
    - **Known real limitation, found while adding the half-adder rule**:
      it only fires when the `XOR`/`AND`/doubling chain's operands are
      `Temp`s reachable through `defOf`/`copyOf`, which are Temp-keyed only
      (Temps are single-assignment in a block; `Reg` is not, so it was
      deliberately excluded from that tracking). In practice, real lifted
      code round-trips every ALU result through its destination `Reg`
      between instructions (`t = XOR(...); EDX = COPY(t)`, then the *next*
      instruction reads `EDX` again, not `t`) - so on `tests/toy-vm/`'s
      actual `op_add`/`op_sub` handlers the rule does not fire yet (op
      counts unchanged: still 127->116 raw->simplified for `op_add`, same
      as before this rule existed). Fixing this needs `defOf`/`copyOf` to
      also track `Reg` via forward last-write-wins (reasoned to be sound
      for a single straight-line `Block` - no loops/joins inside one block,
      so "most recent write so far" is always correct - unlike the
      cross-block case `BackwardSlicer` has to handle by scanning
      backward). **Deliberately not done yet** - see "Not started" below;
      the user's call for now was to fix it at the LLVM-IR level instead
      (a small `PatternMatch.h`-based rule alongside `LLVMLifter`'s
      existing `runInstCombine`), since that substrate is already clean
      SSA by the time it matters, which is a smaller and more targeted fix
      than generalizing the IGNIL-level tracking - also not done yet,
      left for whoever picks this up next.

14. **`tests/obfush/`** - a real (non-toy) constant-recovery exercise
    against a target modeled on [obfus.h](https://github.com/DosX-dev/obfus.h)
    (cloned locally to `/opt/work/obfus.h` - MIT licensed), the user's
    candidate real-world VM-obfuscation target. Two findings from
    investigating obfus.h shaped this:
    - Its "Virtualization" feature is **not** a dispatch-loop VM like
      `tests/toy-vm/` or VMProtect/Themida-style protectors - no VIP, no
      bytecode stream, no handler table. Every `VM_ADD(a,b)`-style call is
      a direct call to **one shared function** with a compile-time-constant
      encrypted opcode and arithmetically-masked operands, which does a
      plain `switch` to the real op. Confirmed by compiling the real
      macros and inspecting with `objdump`: even with all 12 real opcodes,
      GCC lowers the switch to a **compare chain**, not a jump table (case
      values are `RND(...)*5` products scattered across ~0-60000, too
      sparse for GCC's jump-table heuristic) - so `SwitchDetector` has
      nothing to match here, confirmed empirically rather than assumed.
    - The real obfus.h uses `long double` (x87 extended precision)
      throughout, forcing `fld`/`fstp`/x87 codegen.
      `X86IGNILLifter.cpp` has **zero** x87 or SSE support (grepped for
      it - nothing), so the real compiled `VM_ADD` call sites are not
      liftable today.
    Given both findings, `tests/obfush/obfush_repro.c` is a small,
    deliberately-adapted **integer-only** reproduction (`int`, not
    `long double`) preserving the exact same structural pattern (opcode
    encryption via `(op-1)*~SALT_CMD`, per-operand masking via
    `-(x)+SALT`, shared-function dispatch via `switch`) - explicitly *not*
    the literal obfus.h binary, documented as such in the fixture's own
    header comment. Compiled with real `gcc -O2`; GCC tail-call-optimized
    the call site into a `jmp` and produced a small, all-register-operand
    function (`compute`, 28 bytes) - no lifter gaps, nothing new needed in
    `X86IGNILLifter` or `BackwardSlicer`. `tests/obfush/TestObfush.cpp` +
    `TestObfushZ3.cpp` (Z3-gated, same pattern as `TestSliceExprZ3.cpp`)
    disassemble the real compiled bytes, `BackwardSlicer` them, and:
    - recover the encrypted opcode as a plain `slice::Const`, checked
      against the same encryption formula recomputed in the test (not a
      hardcoded magic literal, so a transcription bug couldn't make the
      test pass by accident);
    - recover each masked operand's formula structurally via
      `dragon::slice::match` (`m_Sub(m_Const(...), m_SymReg())`) as
      `SALT - <the caller's own unresolved parameter>` - a clean exercise
      of `BackwardSlicer`'s last-write-wins `Reg` scan, since `EDI`/`ESI`
      each mean something different at different points in the block
      (`EDI` = parameter `a` until it's overwritten with the encrypted
      opcode at the very end);
    - **formally prove** (Z3, not just structural match) that the
      recovered masking formula really is the exact inverse of the real
      unmask step, for *every* possible input value, not just checked
      shape.
    All predictions from hand-tracing the backward scan during planning
    held exactly - every test passed first try. 139/139 total tests
    passing in both build dirs, no regressions. A parallel
    `examples/obfush/` folder holds the same fixture compiled to a real
    ELF executable, handed to the user directly for independent manual
    analysis (Binary Ninja), alongside `obfush_constant_recovery.cpp` (a
    real, built (`if(DRAGON_USE_Z3)`) `add_subdirectory(examples/obfush)`
    target, not just tests) that runs the same
    disassemble->lift->slice->Z3-prove pipeline and prints every stage
    narrated - added after the first pass only had assertions, no visible
    output of the analysis actually running.
    - **Scope cuts, explicit**: `VmDispatch`'s own switch/dispatch body is
      not lifted or analyzed at all (the exercise is scoped to what's
      recoverable from the call site, `compute`, where the interesting
      masking arithmetic lives); no new x87/SSE lifter support was added
      (a much bigger, separate undertaking - `/opt/work/obfus.h` stays
      cloned locally for whoever picks that up and wants to revisit the
      real, unmodified target).

15. **Full trace->IGNIL->LLVM IR recovery for `obfush_repro`'s `compute`
    calling into `VmDispatch`** - direct follow-up to item 14, answering
    the question item 14 didn't: does LLVM's ordinary optimizer actually
    recover the real `a + b` from the masked call, once everything needed
    to lift it correctly exists? Two dead ends hit first, both worth
    remembering:
    - **`LLVMLifter::lift()` (the Graph-based, multi-block path) has no
      tail-call support.** `compute`'s `jmp VmDispatch` targets an address
      outside `compute`'s own discovered block set (the Graph correctly
      recognizes `VmDispatch` as its own function, thanks to its
      `endbr64` prologue) - `lift()`'s JMP handling only knows "branch
      within this function" or `unreachable`
      (`lib/LLVMLifter.cpp:358`, literally commented `// indirect or
      out-of-function target`). Manually patching the IR (replacing the
      `unreachable` with a real `call` into the separately-lifted
      `VmDispatch` function, same `(mem, regs)` calling convention both
      share) got further, but produced `br i1 poison` and a visibly wrong
      result (`store i64 0` instead of the real sum).
    - **That `poison` result was chased down a wrong path first** -
      hypothesized (never confirmed) as a flag-register/PHI interaction
      specific to multi-block functions. **This was wrong.** Switching to
      a trace-based approach (capture ONE concrete Unicorn execution via
      `compute(15,7)`, `Disassembler::liftTrace()`, `LLVMLifter::liftBlock()`
      - the same single-block techniques `tests/toy-vm/` already uses,
      sidestepping `lift()`/tail-calls/PHIs entirely) hit the **exact same
      corruption** in a purely linear block with zero branches or PHI
      nodes possible - proving the poison theory false. The real cause,
      found by walking the raw IGNIL dump: `RAX:i64 = UNDEF()` /
      `EDI:i32 = UNDEF()` right at `VmDispatch`'s `command / ~SALT_CMD`
      division-by-constant lowering - three (later five) plain unhandled
      instructions, same UNDEF safety net as `LifterGaps.md` #1, nothing
      exotic.
    - **Fixed the real gap**: five `X86IGNILLifter` forms, all hit by
      GCC's completely ordinary signed-division-by-constant lowering (the
      standard multiply-high trick for `x / C`) - **not
      obfuscation-specific**, this is what any program dividing by a
      non-power-of-two constant compiles to. `MOVSX64rr32` (SEXT, no
      flags), `SAR32ri`/`SAR64ri` (shift-by-0 leaves every flag and the
      value untouched per real x86 semantics - not even recomputed; CF =
      last bit shifted out; OF only architecturally defined for a 1-bit
      shift, where an arithmetic right shift can never overflow - UNDEF
      otherwise rather than guessed), `IMUL64rri32`/`rri8`/`32rri`/`32rri8`
      (all flags UNDEF - SF/ZF/AF/PF are architecturally undefined for
      IMUL, CF/OF would need a double-width product IGNIL doesn't model),
      `LEA64_32r` (address computed at 64-bit width per the addressing
      registers, explicitly `TRUNC`ed to the 32-bit destination -
      `LLVMLifter`'s `Reg`-destination `COPY` only *widens*
      (`CreateZExt`), narrowing needs an explicit op or it builds invalid
      IR). Caught a real bug in the first draft of the SAR CF/flags
      logic while writing it (read the tied dst/src register's value
      *after* the RMW `COPY` had already overwritten it) - fixed to save
      the original value in a Temp first, exactly the pattern
      `liftAdd`'s own `lhs` save already uses for OF, before it ever ran.
      All 7 new byte sequences generated with real `gcc -c`/`objdump`, not
      hand-derived. 7 new IGNIL-shape unit tests (21 assertions),
      145/145 total tests passing in both build dirs.
    - **Confirmed, with the real fix in place**: the optimized IR contains
      `%10 = add i32 %5, %3` written straight to the return register - the
      real recovered addition, no custom pass, just
      `mem2reg`+`instcombine`+`GVN`+`DCE`. JIT-verified
      (`examples/obfush/obfush_full_recovery.cpp`) against three inputs
      including two never present in the original trace (`(100,23)->123`,
      `(-4,9)->5`), both correct - genuine recovery, not replay.

16. **`INC`/`DEC`/`NEG`/`MOVZX`/`SHL`-by-imm/`SHR`-by-imm** - the remaining
    X86IGNILLifter gaps from `tests/toy-vm/ToyVM.hpp`'s header comment
    (`SAR`-by-imm was fixed as part of item 15; these are its documented
    siblings). All extremely common in ordinary `-O0`/`-O1` output (loop
    counters, byte-to-word widening), so every occurrence previously
    degraded silently to UNDEF.
    - `INC`/`DEC` needed a genuinely new case this lifter hadn't hit
      before: real x86 leaves **CF completely untouched** (not merely
      unknown/UNDEF) so a loop counter doesn't disturb a carry chain from
      a preceding instruction - modeled by simply emitting no `Op` for CF
      at all, rather than extending the shared `emitArithFlags` helper
      (which always emits *something*, UNDEF included, for every flag not
      given a setter) with a third "leave alone" state - safer/more
      surgical than changing a helper every other flag-setting op already
      depends on. Verified with a real semantic round-trip test (not just
      an IGNIL-shape one, since this is the highest-risk part): `cmp` sets
      CF=1, then `inc`/`dec` on unrelated registers, full register+flag
      state compared between real Unicorn execution and the lifted+JIT
      version - passes, CF genuinely survives.
    - `NEG`'s flags (unlike INC/DEC) are fully defined: CF = `(src != 0)`,
      OF = `(src was the minimum negative value for its width)` - both
      implemented and go through the normal `emitArithFlags` path.
    - `SHL`/`SHR`-by-immediate mirror item 15's `SAR`-by-immediate exactly
      in structure (shift-by-0 leaves everything untouched; OF only
      architecturally defined for a 1-bit shift) but have genuinely
      different CF/OF formulas per opcode - `SHL`'s CF is the last bit
      shifted out the *top* (opposite end from SAR/SHR), and `SHL` can
      overflow in either direction (OF = did the sign bit change) whereas
      `SHR`'s OF-when-count-1 reduces to just the original sign bit (a
      logical right shift always clears the new sign bit).
    - 6 new IGNIL-shape tests (19 assertions) + the 1 semantic round-trip
      test above. Caught one real miscount in my own first draft of the
      IGNIL-shape tests while verifying (expected 4 UNDEF flags per
      INC/DEC, forgot DF, real count is 6) - fixed before it shipped, not
      after. 152/152 total tests passing in both build dirs.

17. **Batch A of the obfusheader.h gap-hunt: `AND`/`OR`/`XOR`/`TEST`-immediate
    (`ri32` + `ri8`, 32- and 64-bit), `MOVSX32rr8`, `IMUL32rr`, and the
    dedicated shift-by-1 opcodes** - found by decoding the real compiled
    `examples/obfusheader/obfusheader_repro` binary end to end (see its
    README's three-category gap breakdown: harmless padding, deliberate
    anti-disassembly junk, and real gaps). This item is the first batch of
    fixes from that "real gaps" list.
    - `liftAnd`/`liftOr`/`liftXor`/`liftTest` were **generalized** rather
      than given parallel `*Imm` siblings: their register-form bodies
      already matched `liftAdd`/`liftSub`'s `op[0]=dst, op[1]=tied,
      op[2]=src`-or-imm shape exactly (confirmed with the same
      instrument-the-dispatch-default-case-then-rebuild trick used for
      every prior gap this session) - the `ri`/`ri8` opcodes only change
      what kind operand[2] is, so adding an `isImm()` branch (identical to
      `liftAdd`'s existing one) let 12 new opcodes (`AND/OR/XOR` x
      `{32ri,64ri32,32ri8,64ri8}`, plus `TEST32ri`/`TEST64ri32`) reuse the
      same four functions instead of duplicating ~90% of their bodies.
      `TEST64ri32` was added by opcode-family symmetry/convention rather
      than individually compiled and verified - the one deliberate
      exception to this round's "verify before implementing" discipline,
      called out here rather than silently slipped in.
    - `MOVSX32rr8` is dispatch-only (the existing `liftMovsx` is already
      width-agnostic SEXT, no new function needed).
    - `IMUL32rr` (2-operand register-register form) is a genuinely
      different operand shape from `IMUL32rri`/`64rri32` (item 15):
      `op[1]` really is tied to `op[0]` here (both operands are registers,
      matching the `ADD`/`AND` rr convention), whereas the immediate
      forms' `op[1]` is a distinct source register - confirmed by
      instrumented dispatch, not assumed. Same all-flags-UNDEF rationale
      as `liftImulImm`.
    - `SAR32r1`/`SHL32r1`/`SHR32r1`/`SAR64r1`/`SHL64r1`/`SHR64r1` (the `D1
      /n` implicit-shift-count-of-1 encoding, a distinct opcode from
      `SAR32ri` etc. with an explicit imm byte of 1 - the existing
      `SAR32ri` test already has a comment noting the assembler prefers
      this shorter encoding for a literal `sar eax, 1`) are thin wrappers
      that synthesize an `MCOperand::createImm(1)` onto a copy of the
      `MCInst` and delegate to `liftSarImm`/`liftShlImm`/`liftShrImm` - the
      flag formulas are identical once the count is known, so the logic
      only lives in one place.
    - All new operand shapes were confirmed via the established
      instrument-the-default-case + rebuild + `objdump`-verified-bytes
      methodology, using `ecx`/`rcx` as the destination to force the
      general ModRM `ri`/`ri8` forms rather than the accumulator short
      encodings (a gotcha this session had already been bitten by once
      before - see item 16's methodology note in git history).
    - 5 new IGNIL-shape tests. `TEST32ri`'s test specifically asserts `ECX`
      is never written (not just that flags come out right) - the
      discard-vs-writeback distinction is the entire point of `TEST` over
      plain `AND`, so a regression that silently turned it into a
      writeback would otherwise slip past a flags-only check. 157/157
      total tests passing in both build dirs.
    - **Explicitly deferred to a documented follow-up ("Batch B")**:
      `DIV32r`, `TEST8i8`, and the seven accumulator-implicit-register
      short-encoding forms (`ADD/OR/AND/XOR/CMP/TEST/SUB 32i32`) - these
      have **zero explicit register operand** for the implicit
      accumulator/EAX:EDX, a "hardcoded register lookup" pattern this
      lifter doesn't have yet (`regRSP_`/`regRBP_`/`findGprReg` in the
      constructor is the closest existing precedent, built for a different
      purpose). Also memory-operand `MOVZX`/`MOVSX`/8-bit-ALU forms
      (`MOVZX32rm8`, `MOVSX32rm8`, `MOVSX64rm32`, `ADD8mr`, `MOV8mi`,
      `CMP8mi`, `SHL8mi`, `SHL8mCL` - the last needing a new
      variable-shift-count-from-`CL` capability) and real SSE2 `double`
      arithmetic from `obfusheader.h`'s `int_proxy` (confirmed via the
      same binary, not x87 - see "Not started" below) - none of these are
      started.

18. **SSE2 scalar-`double` support in IGNIL + X86IGNILLifter, plus a real
    Unicorn-trace-capture demo against `sum_to_n`** - triggered directly by
    manually analyzing `examples/obfusheader/obfusheader_repro` in Binary
    Ninja together with the user, who found a genuinely nice example: a
    `sar`/`je`-style opaque predicate (`xor rax,rax` / `je`, unconditionally
    taken) whose fall-through and branch-target bytes *overlap* - the
    fall-through decodes (linear-sweep style) as `add [rsi+0xf],ah` / `out
    dx,eax` (privileged, would fault) / two garbage `shl`s / another `out`
    / `leave`, while the real target 2 bytes later, from the *same bytes*,
    decodes as three `pxor xmm,xmm`. Confirmed by hand with `objdump
    --start-address` at both offsets. This is a step beyond the
    obfusheader.h README's original "harmless junk in unreachable padding"
    framing - the junk isn't just padding, its bytes are literally aliased
    with the real, reachable code.
    - **IGNIL design**: `Const`/`Reg`/`Temp` stay exactly as they are (no
      new `Value` variant, no width/type tag added) - six new opcodes
      (`FADD`/`FSUB`/`FMUL`/`FDIV`/`SITOFP`/`FPTOSI`) added to
      `IGNIL.hpp`/`IGNIL_OPS.def` in lockstep (the `.def` file's ordering
      comment is load-bearing - the name array is indexed by enum value).
      Floatness is decided by the *operation* consuming a Value's bits,
      exactly like `SDIV`/`UDIV` already decide signedness for the same
      underlying bits rather than tagging `Const` itself - the natural,
      minimal extension of a design already in place. `IGNILSimplify.cpp`
      needed zero changes: every one of its `switch(Opcode)` folding rules
      is an opt-in whitelist with `default: break`, so the six new opcodes
      safely pass through unsimplified (float constant-folding is left to
      LLVM's own `instcombine` once lifted, not reimplemented in IGNIL).
    - **`LLVMLifter`**: a new `asDouble()` helper bitcasts a raw-bits
      `llvm::Value` to `double` (skipping the bitcast if it's already
      `double` - keeps Temp-to-Temp float chains in LLVM's native float
      domain instead of laundering through i64 at every step). The shared
      `emit()` lambda gained one branch: storing a `double`-typed result
      into a `Reg` dst now bitcasts to the integer slot type instead of the
      existing `CreateZExt` (which asserts on a non-integer type).
      Confirmed empirically (scratch driver, not just reasoning about it)
      that `mem2reg`+`instcombine` recognize the resulting
      bitcast-store/bitcast-load round trips as redundant and cancel them,
      so a `sitofp` feeds directly into a `fadd` with zero bitcast noise in
      the optimized IR, and a load even gets its type sunk straight to
      `double` - the design doesn't just work, it stays legible after
      optimization.
    - **`X86IGNILLifter`**: XMM0-XMM15 added to `x86registers.def`, but
      **only the low 64 bits of each are modeled** - real hardware's
      upper-64-bits-preserved merge semantics for these instructions
      reduce to a no-op under that model (documented as a deliberate scope
      cut; a packed SSE/AVX target would need to revisit this). That
      collapse means `PXORrr` dispatches straight into the existing
      `liftXor` (unchanged - PXOR is bitwise, Intel's own manual classes it
      under SIMD integer, not float) and `MOVSDrm`/`MOVSDmr` dispatch
      straight into the existing `liftMovLoad`/`liftMovStore`. Only
      `MOVSDrr` (tied-merge operand at a different index than plain
      `MOV64rr`), `CVTSI2SDrr_Int`/`CVTSI642SDrr_Int` (-> `SITOFP`),
      `CVTTSD2SIrr_Int`/`CVTTSD2SI64rr_Int` (-> `FPTOSI`),
      `ADDSDrr_Int`/`SUBSDrr_Int`/`MULSDrr_Int`, and `MULSDrm_Int` (found
      by grepping the real binary for bracket operands on these mnemonics -
      a compile-time double literal multiply, ordinary code, not
      obfuscation-specific, same story as item 15's division-by-constant)
      needed new functions. All operand shapes confirmed via the
      established instrument-the-dispatch-default-case methodology, not
      assumed - `CVTSI2SDrr_Int`'s tied `op[1]` genuinely differs from
      `CVTSI2SDrr`'s in a way that would have been easy to get backwards by
      analogy alone. 3 new tests (IGNIL-shape for the register and
      load/store/mem-operand forms, plus an `llvm::verifyFunction`-checked
      test confirming real, correctly-typed `sitofp`/`fadd
      double`/`fptosi` IR - a pure IGNIL-shape test can't catch a
      type-mismatch bug at the LLVMLifter boundary). 160/160 total tests
      passing in both build dirs.
    - **`examples/obfusheader/obfusheader_trace_recovery.cpp`** (new,
      `DRAGON_USE_UNICORN`-gated): captures a real trace of `sum_to_n(n)`
      for two different `n` (10 and 6), lifts each independently, and JIT-
      verifies the optimized result against the real Unicorn execution.
      **Had to re-derive the trace-capture technique rather than reuse
      `examples/obfush/obfush_full_recovery.cpp`'s verbatim**: that file's
      one-time upfront linear sweep (build a `byAddr` map, look up each
      traced address) is unsafe here specifically *because* of the
      overlapping-instruction trick above - a sweep starting at the decoy
      fall-through would never independently re-decode starting at the
      real branch target 2 bytes later, so `byAddr` would have no entry
      for it at all. Fixed by re-decoding fresh at *every* traced address
      via `Disassembler::disassembleBlock(...)[0]` instead - the same
      thing a real disassembler has to do to resolve this trick correctly.
      **Caught and fixed a real bug while first running this**: unlike
      `compute()` (no memory operands at all), `sum_to_n` genuinely spills
      to the stack (`movsd [rsp-0x10], xmm1`, real `-O2` register pressure
      from `CFLOW_BRANCHING`'s inflated body) - `jitRun`'s harness never
      initialized RSP, so a `[rsp-K]` address computed against RSP=0
      wrapped to a huge unsigned offset and corrupted the heap well
      outside the `mem` buffer (`free(): invalid size`, confirmed via
      `gdb`). Fixed by pointing RSP at the middle of the flat `mem` buffer
      before invoking the JIT'd function.
    - **Result, once fixed**: for `n=10` (55) and `n=6` (21), the
      optimized IR for the *entire unrolled trace* (1093 / 1003 raw IGNIL
      ops, 187 / 179 real instructions executed - zero of them the
      fake-`OUT`/junk bytes, confirmed structurally, not just asserted)
      collapses to a single `store i64 55` / `store i64 21` literal into
      the return register, plus register-writeback bookkeeping - under
      1.5KB of IR text either way. This is a stronger result than "the
      junk gets removed": once a trace for one concrete `n` is fully
      flattened and every opaque predicate's `CJMP` is dropped (a trace's
      control flow is already resolved by construction - see
      `LLVMLifter::liftBlock`'s doc comment), *nothing* in the remaining
      arithmetic chain depends on any non-constant input anymore, so
      ordinary constant folding walks it all the way down to the literal
      answer. JIT-verified against the real native Unicorn result for both
      `n` values, confirming this isn't a coincidence tied to one input.
    - **Scope, stated explicitly rather than left implicit**: this is
      trace flattening for one concrete `n` each time, matching this
      project's established honesty norm (see item 12's "not guided
      symbolic execution" correction) - no induction-variable or back-edge
      structure is reconstructed, and there is no single recovered
      function that handles arbitrary `n`. What's demonstrated is that the
      opaque-predicate/overlapping-instruction scaffolding contributes
      *nothing* dragon-tales needs to specifically understand or defeat -
      capturing one real execution and handing it to LLVM's stock
      optimizer is sufficient.
    - **Still not started** (Batch B, unchanged from item 17): `DIV32r`,
      `TEST8i8`, the accumulator-implicit forms, memory-operand
      `MOVZX`/`MOVSX`/8-bit-ALU forms. The other three obfusheader_repro
      functions (`max_op`, `abs_op`, `is_magic`) haven't been traced yet -
      same technique, not yet applied.
    - **Follow-up bug, caught by the user's own build**: the six new
      opcodes were originally inserted *inside* `ignil::Opcode` (grouped
      next to `ADD`/`SUB`/`MUL` and `ZEXT`/`SEXT`/`TRUNC` for readability),
      which silently shifted every enum value after each insertion point.
      `c_shim/dragon_c.h`'s `dragon_ignil_opcode_t` and
      `python/dragon.py`'s `IGNILOpcode` both mirror `ignil::Opcode` as
      independent, hardcoded integer constants - a compile-time
      `static_assert` in `dragon_c.cpp` catches this *only* for the few
      spot-checked values (`COPY`, `LOAD`, `UNDEF`), which is exactly what
      fired (`UNDEF`'s C++ ordinal moved from 35 to 41). The middle of the
      C enum (`ADD` through `RET`) would have been silently wrong with no
      compiler error at all if only the one failing assert had been
      patched instead of the root cause. Fixed by moving all six new
      opcodes to the end of the enum (after `UNDEF`, preserving every
      existing ordinal) in `IGNIL.hpp`/`IGNIL_OPS.def`, adding the six new
      `DRAGON_IGNIL_*`/`IGNILOpcode` constants at 36-41, and adding a
      fourth spot-check assert (`FPTOSI`, the new last value). 160/160
      tests passing in both build dirs, both `libdragon`/`libdragon_c`
      compiling clean, the `sum_to_n` trace demo re-verified unaffected.
      Lesson written directly into `IGNIL.hpp`'s enum and `IGNIL.md`'s
      illustrative copy: new opcodes always go at the end, not wherever
      they'd read best.

All of the above is on the `dev` branch as commits (function-start fix,
sigs, trace lifter, C-shim/Python wiring for the latter two, `liftBlock` +
toy-vm, the IGNIL simplifier + half-adder rule, the obfush constant-recovery
exercise, item 15's 5 lifter fixes, and items 16-18 - the INC/DEC/NEG/
MOVZX/SHL/SHR gaps, obfusheader.h Batch A, and SSE2 float support - bundled
into one commit, `c9121de`, since 3 rounds of work landed interleaved in
the same X86IGNILLifter dispatch chain) plus the earlier IGNIL/coverage/CI/
Unicorn work from before that. Item 19 (the `BasicBlock` overlapping-decode
fix + its regression test) is commit `001266e`, kept separate since it's
cleanly self-contained. Item 20 (this pre-merge cleanup pass) is two more
commits: `4e78fcc` (the standalone RISC-V logging fix) and `1ea6bc1` (the
shim/Python API additions bundled with their documentation, since the docs
are the direct writeup of that same body of work). Not yet pushed to a
remote as of this note - only committed locally on `dev`.

## Agent skills (`.agents/skills/`)

Five Claude Code skills added (not code, not on the priority list above - reference material
for whoever picks up the VM-devirtualization direction next): `ignil-ir-design`,
`vm-devirtualization`, `trace-capture`, `obfuscation-catalog`, `semantic-lifting-tests`.
Synthesized from NeverD's `.agents/skills` (binary-lifting, mlir-development, llvm-obfuscation,
llvm-security, llvm-optimization, compiler-development, dynamic-instrumentation, and the
code-protection slice of anti-cheat-systems - game-hacking-techniques and the
ESP/aimbot/DMA/HWID/input-simulation parts of anti-cheat-systems were reviewed and deliberately
excluded as out of scope for a binary-analysis library) plus four back.engineering posts on
real static-devirtualization engines (Tencent ACE, Themida, VMProtect 2 architecture + tooling).
Notably, writing `vm-devirtualization` surfaced that `SwitchDetector`
(`include/dragon/analysis/SwitchDetector.hpp`) already recognizes the same
LOAD(ADD(base,MUL(idx,scale))) shape a VM dispatcher's handler-table lookup has, and that
`SliceExprMatch.hpp` (a composable pattern-matching DSL over `slice::Expr`) exists with **zero
current call sites** - both are ready-made substrate for the devirtualization passes described
in "Not started" below, not something that needs building from scratch.

19. **Fixed the CFG-construction bug that broke static lifting of
    overlapping-instruction bytes** (found and initially left unfixed
    earlier this session - see "Not started" history below for the full
    original diagnosis). The user asked, essentially, "the bug is in
    `BasicBlock`, isn't it fixable by just following where the branch
    actually goes" - correct, and it turned out to be a small, local,
    one-file fix, not the "real architectural work" this item originally
    warned it might be.
    - **Root cause** (`lib/BasicBlock.cpp`): the constructor and
      `instructions()` both walked `graph->listing()` (a single shared
      `std::map<addr, Instruction>`) via `lower_bound(startAddress)` then
      `++it` - trusting **sorted-map iteration order** to mean "the next
      instruction in this block's own decode path." That's only true when
      decodes never overlap. Deliberately obfuscated code (the `sum_to_n`
      overlapping-instruction trick) can legitimately have two different,
      valid instructions starting at different offsets into the same
      bytes, both present in the same shared listing - sorted order across
      *all* of them is not the same as "this one path's own chain."
    - **Fix**: walk by exact address instead - `graph_->get(cur)`
      (`std::map::find`, same O(log n) cost as `lower_bound`), then
      `cur = insn->address() + insn->size()`, repeat. This only ever
      follows the chain *this specific block's own instructions* define,
      completely ignoring whatever else exists elsewhere in the shared
      listing at other addresses - correct by construction, immune to
      overlap, and behaviorally identical to the old code for every
      non-overlapping case (verified: 160/160 pre-existing tests
      unaffected before adding new ones).
    - **New regression test**:
      `tests/disassemblers/TestX86Analysis.cpp` - a minimal 10-byte
      sequence (`xor eax,eax` / `je` whose target lands on byte index 2 of
      the fall-through's own `mov eax,imm32`, landing exactly on a `ret`)
      verified by hand with `objdump` from both offsets before writing it,
      not hand-guessed. Confirms both overlapping blocks construct
      independently with their own correct instruction content - this is
      what would have caught the original bug had it existed before the
      obfusheader.h investigation surfaced it. 161/161 total tests passing
      in both build dirs.
    - **Verified impact on `sum_to_n` itself** (re-ran the exact static
      lift from the original diagnosis, this time against the fixed
      code): `RECURSIVE_TRAVERSAL` now produces a full, correctly-
      connected 19-block function (was silently dropping to 2 blocks +
      `unreachable`) - and critically, this is **real loop recovery, not
      trace-unrolling**: the lifted IR has a genuine `phi`-based induction
      variable and back-edge (`bb_8576`'s self-loop), unlike item 18's
      trace-based result which is one input's execution flattened straight-
      line. Most of the opaque predicates still fold correctly to
      `br i1 true`/`br i1 false` even in this harder, real-CFG context -
      `mem2reg`+`instcombine` don't need a trace to see through
      `xor rax,rax; je`. The raw (pre-optimization) IR passes
      `llvm::verifyFunction` cleanly (not broken).
    - **Two things NOT fully resolved, stated plainly rather than
      glossed over** (one of these got corrected mid-session - see the
      "corrected" note below, left in place rather than quietly edited
      away, matching this project's own norm of documenting mistakes as
      they're found):
      1. ~~Confirmed benign~~ **Actually a real, confirmed hang - not
         benign.** A couple of remaining paths show LLVM `poison` values
         after optimization (`br i1 poison` on one predicate, one `phi`
         edge). Initial assessment (wrong): `verifyFunction` passing on
         the raw IR meant this was just `mem2reg`'s ordinary, legal
         handling of a PHI edge that looked - from static reasoning alone
         about which predicates fold to constants - like it should be
         unreachable at runtime. That reasoning doesn't hold up: actually
         *running* the compiled function (see item 2 - `clang` straight
         from the `.ll`, sidestepping the JIT issue entirely) hangs in an
         infinite loop for `n=10`, the very first input tried - zero
         output, had to be killed. `verifyFunction` only checks IR
         well-formedness, not termination or correctness, and there was no
         real execution to check against until this was tried. Not
         root-caused (could be the `poison`/`bb_8576` loop area actually
         being reached at runtime despite static appearances, or something
         else) - a genuinely open question, left as such rather than
         re-asserting the earlier "benign" claim.
      2. End-to-end JIT *execution* verification via `llvm::orc::
         LLJITBuilder` remains blocked by a separate, unrelated issue:
         it fails with "Unable to find target for this triple (no targets
         are registered)" specifically when the same process has already
         used `dragon::Graph`/`X86Analysis` (unlike `examples/obfush/
         obfush_full_recovery.cpp` and `examples/obfusheader/
         obfusheader_trace_recovery.cpp`, which only use the Graph-free
         `Disassembler` overloads and JIT successfully). Tried forcing an
         explicit target triple on the `JITTargetMachineBuilder` - same
         error, ruling that out specifically. Traced one layer further:
         `libdragon.so` statically links its own copy of LLVM (no
         `libLLVM.so` in its `ldd` output, but `LLVMInitializeX86Target`
         is defined in its own symbol table) while a JIT-using consumer
         separately links the real shared `libLLVM.so.21` - two
         independent copies of LLVM's global state in one process. Not
         fatal by itself (the two working examples above have the same
         split and JIT fine) - the failure tracks specifically with using
         `X86Analysis` (which pulls in Z3 when enabled), not the mere
         static/dynamic split. Not resolved further.
         **Worked around, not fixed**: compiling the optimized `.ll`
         straight to a native executable with `clang`/`llc` (a separate
         process, no LLJIT, no in-process LLVM conflict at all) and
         hand-adding an LLVM-IR `main()` that sets up the `mem`/`regs`
         buffers and calls the lifted function directly - this is what
         actually ran the code and surfaced item 1's hang. A real
         alternative to LLJIT for verifying a Graph-based static lift
         when the in-process JIT path is unavailable.
      3. `LINEAR_SWEEP` (the default strategy) still cannot recover this
         function at all - it never attempts branch-target-seeded decoding
         in the first place, so the real branch target genuinely has no
         instruction registered at its address. The fix changes its
         failure mode from *silently wrong* (a mismatched block, the
         original bug) to *honestly absent* (the block just doesn't
         exist) - a real improvement, but not a recovery. Extending
         `LINEAR_SWEEP` itself to seed decodes from branch targets (making
         it behave more like `RECURSIVE_TRAVERSAL` for this purpose) is a
         separate, not-yet-scoped idea.
20. **Pre-merge cleanup pass**: code-review-style audit of C++/C-shim/Python
    API parity (via a background Explore agent), a documentation pass across
    every doc file, and a couple of small robustness fixes, all done ahead
    of the user's stated plan to push `dev` and merge into `main`, then
    switch to a branch+issue workflow going forward.
    - **API-parity audit**: the agent diffed every C++ header under
      `include/dragon/` against `c_shim/dragon_c.h` and `python/dragon.py`.
      Confirmed real, zero-exposure gaps in three analysis features:
      `ignil::simplifyBlock` (`IGNILSimplify.hpp`),
      `slice::detectPossibleSwitch`/`readTableTargets` (`SwitchDetector.hpp`),
      and `slice::enumeratedValues`/`solveUnique` (`ValueEnumerator.hpp`,
      Z3-gated) - all three had complete C++ implementations and existing
      Catch2 test coverage (`TestIGNILSimplify.cpp`, `TestSwitchDetector.cpp`,
      `TestSliceExprZ3.cpp`) but were entirely unreachable from C or Python.
    - **Implemented**: `dragon_ignil_simplify_block`,
      `dragon_detect_possible_switch` + `dragon_read_table_targets` (new
      `dragon_switch_candidate_t` plain value struct - not an opaque handle,
      the first of its kind in the shim), and
      `dragon_slice_expr_enumerate_values` + `dragon_slice_expr_solve_unique`
      (each constructs its own `z3::context` internally - no way to pass
      extra Z3 assertions across the C boundary, a deliberate, documented
      scope cut from the C++ API's optional `constraints` parameter). Plus a
      general `dragon_u64_array_free` helper for the two functions that now
      return owned `uint64_t*` arrays. Python wrappers added for all of it
      (`IGNILBlock.simplify()` -> `(IGNILBlock, SimplifyStats)`,
      module-level `detect_possible_switch()`/`read_table_targets()` with a
      new `SwitchCandidate` class, `SliceExpr.enumerate_values()`/
      `.solve_unique()` following the existing `to_z3_string()`
      try/except-`AttributeError` pattern for a no-Z3 build). Verified with
      a hand-written end-to-end smoke script (`Disassembler.analyze()` on
      `mov eax,5; ret`, sliced EAX, `enumerate_values()` -> `[5]`,
      `solve_unique()` -> `5`, `simplify()` round-trips, `SwitchDetector`
      correctly says no-match on a plain `Const` and correctly reads back a
      hand-built 2-entry table) - not committed as a permanent test, since
      the shim/Python layer has no test suite of its own at all in this
      project (a gap noted below, not fixed here). 161/161 existing Catch2
      tests still pass in both build dirs after the shim changes.
    - **Opcode mirror robustness fix**: the `static_assert` block in
      `dragon_c.cpp` that checks `ignil::Opcode`'s C++ values against
      `dragon_ignil_opcode_t` only spot-checked 4 of the (now) 42 values -
      exactly the gap that let the mid-enum-insertion bug earlier this
      session (see item 17's "follow-up bug" above) slip past it. Made
      exhaustive: one `static_assert` per opcode, all 42. Doesn't help
      Python's independent `IGNILOpcode` hand-copy (no compile-time check
      possible across languages), which is the least-protected of the three
      mirrors against a future silent desync - noted below.
    - **Documentation pass**: added prose + tables for `IGNILSimplify`,
      `SwitchDetector`, and `ValueEnumerator`'s enumerate/solve to
      `docs/c_api.md` and `docs/python_api.md`; added `IGNILSimplify` and
      `Disassembler::liftTrace`/`LLVMLifter::liftBlock` (trace-based
      lifting) to `docs/cpp_api.md`, which had neither despite both other
      API docs already covering trace lifting. Fixed a stale claim in
      `docs/python_api.md` (`IGNILOpcode` documented as "35 values" - stale
      from before the float opcodes existed at all; now 42). Added missing
      `README.md` Features/Structure entries for `LLVMLifter`,
      `IGNILSimplify`, `SwitchDetector`, `SliceExprZ3`/`ValueEnumerator`,
      `SliceExprMatch` - none were mentioned at all despite being real,
      tested, (mostly) shipped features. Added a header note to `IGNIL.md`
      clarifying it's a historical build-log/tutorial (stale include paths
      and all), not a current reference - points to `docs/cpp_api.md`
      instead.
    - **Explicitly out of scope this round** (found by the same audit,
      deliberately not implemented - good candidates for the first batch of
      issues once the branch/issue workflow starts, roughly in order of how
      contained each one looks):
      - `BasicBlock::each_predecessor` - `each_successor` exists,
        `predecessors()` exists in C++ and is cached identically to
        `successors()`, but has no shim mirror. Small, symmetric, low-risk.
      - `LLVMLifter::liftBlock` shim wrapper - the C++ method itself got
        documented this round (see `docs/cpp_api.md`), but still has no
        `dragon_llvm_lifter_lift_block`-style entry point; `lift()` (Graph-
        based) and `dragon_disasm_lift_trace` are both already exposed, so
        this is the one asymmetric gap left in an otherwise-complete chain.
      - `ARMInfo`/`X86Info` (`disassembler/{arm,x86}/*Info.hpp`) - prologue/
        epilogue/trap/nop/halt classification, fully unreachable from C or
        Python. A real, self-contained feature addition, not a one-liner.
      - `SliceExprZ3` stateful lowerer - only the one-shot
        `dragon_slice_expr_to_z3_string` convenience function is exposed;
        the actual class (a live `z3::context&`, a symbol cache reused
        across calls, explicit `reset()`) has no shim presence at all, so a
        caller can't share one Z3 context/cache across multiple slices in
        one query. A real design decision (how to represent a shared
        `z3::context` handle across the C boundary), not a quick add.
      - `ignil::Op` structural accessors - `dragon_ignil_block_each_op`'s
        callback only surfaces `(addr, opcode, index)`, never structural
        `dst`/`srcs`; the only way to inspect an op's operands from C/Python
        today is `op_to_string()` + parsing text back out. `SliceExpr` gets
        full structural accessors by contrast (`left`/`right`/`operand`,
        etc.) - `ignil::Op`/`Value` doesn't.
      - `SliceExprMatch` DSL (`m_Const`, `m_BinOp`, `m_AnyOrder`, ...) -
        confirmed template-based/compile-time; cannot cross a C ABI in its
        current form without hand-writing a concrete wrapper per pattern.
        Zero current C++ call sites either (see the skills note below) -
        lowest priority of this list until something actually needs it.
      - `PatternParser::parseLine`/`parseFile` standalone - only reachable
        indirectly today via `dragon_sigdb_load_file`/`load_directory`; no
        way to parse a single `.pat` line or get a `Signature` list back
        directly through C/Python.
      - Register descriptor headers (`x86IGNILRegs.hpp`/`armIGNILRegs.hpp`)
        - `python/dragon.py`'s `x86`/`arm` classes are a hand-maintained,
        byte-for-byte duplicate of the C++ constexpr tables, with no shared
        source of truth or automated check tying them together. Not a
        missing feature so much as a standing maintenance risk (same shape
        as the opcode-mirror bug this whole session started from, just for
        register offsets instead of opcode ordinals).
      - `dragon_block_t` ownership inconsistency - `dragon_graph_get_block`
        returns an owned block (caller must `dragon_block_destroy`), while
        `dragon_function_each_block`'s callback hands back a **borrowed**
        pointer into the `Function`'s internal storage - same typedef, no
        per-function ownership annotation beyond a general note near the
        top of the header. Not a live bug (Python's
        `Function.block_addresses()` only reads `start_address` during the
        callback and never stores/destroys it), but a sharp edge for any
        future caller of `dragon_function_each_block`. Worth a doc-comment
        fix at minimum, possibly worth its own issue if it's ever a real
        footgun.
      - `dragon_graph_create` - has no Python wrapper at all (Python's
        `Graph` is always obtained via `Disassembler`/`Graph.get_*`, never
        constructed standalone), and even its own C++ mirror silently drops
        the `Configuration` parameter the real `dragon::Graph` constructor
        takes, defaulting one internally instead. Lowest priority - nothing
        currently needs a bare, disassembler-less `Graph`.
      - `Graph`'s write-oriented methods (`addInstruction`, `addBlock`,
        `addFunction`, `update`, `merge`, non-const `get()`, `listing()`,
        vector-returning `instructions()`/`blocks()`/`functions()`) have no
        shim presence at all - but this looks like a deliberate design
        choice (the shim routes all graph construction through
        `Disassembler`) rather than an oversight, so it's listed here for
        completeness rather than as a recommended next step.

## Not started / explicitly deferred

- **`LLVMLifter::lift()` (the Graph-based, multi-block path) has no
  tail-call support** - found while scoping item 15. A `JMP` whose target
  lies outside the current function's own discovered block set becomes
  `unreachable` (`lib/LLVMLifter.cpp:358`) even when the target is another
  function already in the same `Graph` - there's no recognition that this
  is effectively a call. Item 15 worked around it with a manual IR patch
  (`CallInst`+`ReturnInst` in place of the `UnreachableInst`, same
  `(mem, regs)` convention both lifted functions already share) for one
  demo; a real fix would detect this pattern in `lift()` itself and emit
  the call automatically. Not attempted - item 15 solved the actual
  question (does LLVM recover the real op?) via the trace-based path
  instead, which doesn't need this at all.
- **x87/SSE floating-point support in `X86IGNILLifter`** - found while
  scoping item 14 (`tests/obfush/`). `X86IGNILLifter.cpp` has no handling
  at all for `fld`/`fstp`/x87 instructions or SSE `movsd`/`addsd`-style
  `double` arithmetic (grepped for it, nothing). This is why
  `tests/obfush/`'s fixture had to be an integer-only adaptation rather
  than the real obfus.h (which uses `long double` throughout, forcing x87
  codegen) - `/opt/work/obfus.h` is cloned locally and ready to revisit for
  real if this ever gets built.
- **Half-adder MBA identity (`ADD(XOR(p,q), 2*AND(p,q)) -> ADD(p,q)`) is
  only caught by item 13's IGNIL simplifier when the chain runs through
  `Temp`s** - real lifted code round-trips ALU results through `Reg`
  between instructions, which `defOf`/`copyOf` don't track (Temp-only, by
  design - see item 13). Two candidate fixes discussed, **neither started
  yet**, user's preference for now is the second:
  1. Generalize `defOf`/`copyOf` to also track `Reg` via forward
     last-write-wins (sound for a single straight-line `Block`, unlike the
     cross-block case).
  2. A small LLVM-IR-level pattern rule (`llvm/IR/PatternMatch.h`) added
     to `LLVMLifter`'s optimization pipeline alongside `runInstCombine` -
     preferred, since by the time `LLVMLifter`'s existing
     mem2reg/instcombine/GVN/DCE have run, the chain is already exposed in
     clean SSA form with no register-file noise left to fight through
     (verified directly on `op_add`'s optimized IR from
     `toy_vm_handler_recovery`).
  Either way, verify against real generated code (not just a hand-built
  IGNIL unit test) before considering it done - that's exactly how the
  original "already handled by LLVM" claim turned out to be wrong.
- **ARM64/Thumb switch-table recognition** in `SwitchDetector` - found while
  scoping item 13's simplifier work. `detectPossibleSwitch`'s pattern
  (`LOAD(ADD(Const{base}, MUL(SymReg{idx}, Const{scale})))`) and
  `readTableTargets` are shaped for x86-style flat pointer-array jump
  tables only (`readTableTargets`'s own doc comment says "suitable for
  x86/x86-64"). ARM64 compilers commonly emit `adr`+`ldrsw`+`add` over a
  table of **relative** offsets instead of absolute pointers, and
  Thumb/AArch32 has `tbb`/`tbh` (byte/halfword-sized relative entries, not
  pointer-sized at all) - neither matches the current pattern or entry-size
  assumption. Needs a second pattern (or a generalized "add base to a
  loaded relative offset" match) plus a non-pointer-sized
  `readTableTargets` variant.
- **Memory/`Load`-address canonicalization** (e.g. recognizing `[RSP+8]`
  and `[RSP+0x8]`, or two structurally-different-but-equal address
  expressions, as the same load) - explicit scope cut of item 13's
  simplifier. Would likely subsume the hand-rolled
  `concretizeStackPointer()`/`rewriteStackPointerBack()` pass in
  `examples/cpp/toy_vm_handler_recovery.cpp`. Also the natural place to
  eventually add cross-block/join-node simplification, tied to the
  not-yet-started CFG-discovery worklist item below.
- **VM-obfuscation deobfuscation, real target** (raised in discussion as a future direction):
  `tests/toy-vm/` (see "Done" item 10) proved the *mechanics* end-to-end on a from-scratch toy
  VM using only generic LLVM optimization (mem2reg/instcombine/GVN/DCE) — no VM-specific passes
  were needed for a toy this small. Still not started: capturing a trace from a *real* VM
  protector (Unicorn/QBDI - no capture glue for a real target exists yet, see the
  `trace-capture` skill), and the VM-specific pieces a real target will actually need that the
  toy didn't exercise - range-scoped constant promotion (distinguishing VM-private memory from
  real program memory), VMEXIT classification, VIP tracking, and the vjcc/dispatch-table
  pattern matching described in the `vm-devirtualization` skill (`SwitchDetector` and the
  unused `SliceExprMatch` DSL are the ready-made substrate for that last piece).
- **8-bit MOV** (and possibly other 8-bit forms beyond what the coverage
  tool already tracks) - the one piece of the original toy-vm-exercise gap
  list (see "Done" item 10) not yet fixed. INC/DEC/MOVZX/NEG/SHL/SHR/SAR by
  immediate are all done now (items 15 and 16). Same shape of fix as the
  rest: add dispatch-table entries + `liftXxx` helpers, verify with a
  semantic round-trip test, not just an IGNIL-shape one.
- **Parameter recognition + function reconstruction** (priority #4) —
  **analysis done; typed emission + stack slots remain.** The forward
  liveness analysis and calling-convention argument/return recovery now
  exist (`dragon::analysis`, `include/dragon/analysis/ArgumentRecovery.hpp`):
  `liveInRegisters()` runs an ordinary live-variable fixpoint over the
  function's IGNIL CFG (the forward counterpart to `BackwardSlicer`), and
  `recoverSignature()` intersects the entry live-in set with the target's
  calling convention (x86-64 System V and AArch64 AAPCS64 tables) to produce
  a `FunctionSignature` - the integer/pointer arguments in convention order,
  each sized by its widest read, plus a return value when the convention's
  return register is written in the body. `LLVMLifter` records the recovered
  prototype on every lifted function as a `"dragon.signature"` attribute and
  exposes `recoveredSignatureString()`.

  **Still remaining** (the harder half):
    1. **Emit a real typed `llvm::FunctionType`** from the recovered
       signature - `iN @sub_x(iN %arg0, ...)` - instead of the current
       `(i8* mem, i8* regs) -> void`. The blocker is the memory model: the
       `mem` pointer has to be replaced (stack slots below, a global/opaque
       for the rest) and arguments seeded from the LLVM parameters rather
       than from the `regs` blob.
    2. **Stack-slot promotion** - route frame-relative (`[rbp-N]`/`[rsp+N]`)
       LOAD/STORE to per-slot `alloca`s instead of `GEP mem`, so mem2reg
       promotes them to SSA locals (the "variables"). Needs frame-pointer /
       stack-offset detection on the address expressions.
    3. **Convention breadth**: Win64, SSE/float and stack-passed arguments,
       struct classification; sub-register-precise def/use in the liveness.
- **AND/OR/XOR/TEST/ADC/SBB memory-operand forms** and **8/16-bit ALU
  forms** — real gaps the coverage tool already surfaces; same shape of
  fix as the MOV/ADD/SUB/CMP work, just not done yet for these mnemonics.
  Item 17's obfusheader.h gap-hunt found concrete real-binary instances of
  this: `MOVZX32rm8`, `MOVSX32rm8`, `MOVSX64rm32`, `ADD8mr`, `MOV8mi`,
  `CMP8mi`, `SHL8mi`, and `SHL8mCL` (variable shift count from `CL` - a new
  capability, every other shift lifter so far only handles a compile-time
  immediate or the implicit-1 form).
- **`DIV32r`, `TEST8i8`, and the accumulator-implicit-register short
  encodings** (`ADD/OR/AND/XOR/CMP/TEST/SUB 32i32`) — found alongside the
  above in the same obfusheader.h gap-hunt (item 17). All have zero
  explicit register operand for the implicit accumulator (`DIV`'s
  EAX:EDX, the rest's AL/EAX) - needs a new hardcoded-register-lookup
  pattern in the lifter, analogous to but distinct from the existing
  `regRSP_`/`regRBP_`/`findGprReg` construction-time lookups.
- ~~Real SSE2 `double` arithmetic~~ **Done, item 18.** (Was: confirmed
  present in the real `examples/obfusheader/obfusheader_repro` binary,
  `int_proxy`'s opaque-predicate arithmetic - now implemented: `MOVSD`
  rr/rm/mr, `CVTSI2SD`, `CVTTSD2SI`, `ADDSD`/`SUBSD`/`MULSD` rr and
  `MULSD` rm, `PXOR`. Still genuinely distinct from - much smaller than -
  the x87 `long double` gap below, which remains not started.)
- **Broader x86/ARM prologue byte-pattern matching** (NeverD's
  strict/relaxed tiers) — only worth doing if the baseAddr-seeding fix
  turns out to be insufficient in practice (e.g. whole-section scanning
  for unreferenced, un-called, prologue-less functions). Deliberately
  skipped for now due to false-positive risk from scanning every
  instruction position in the disassembled region.
- ~~**Real-world `.pat` file testing** to verify the CRC16 variant~~ —
  done: the variant was wrong (CRC-16/ARC); `computeCRC16` is now FLAIR's
  (poly 0x8408, complemented, bytes swapped) and is tested against `.pat`
  lines generated by rizin's `rz-sign`.
- **FLIRT binary `.sig` format**, **referenced-name (`^offset`)
  disambiguation**, and **alternate-branch pattern extensions** — explicit
  scope cuts in `PatternParser`, documented in its class doc.
- **`LLVMLifter::lift()` segfault on second call** (`LifterGaps.md` #2) —
  still open, not touched this round.
- **Loader/container-format support (ELF/PE/Mach-O metadata, `.eh_frame`
  unwind tables)** — **ELF and PE/COFF done** (`dragon::loader`,
  `include/dragon/loader/`): `LoadedImage` parses the container through
  `llvm::object` and exposes sections (virtual placement + R/W/X),
  symbols (import/export flags), entry point, image base and the
  header-detected architecture; `loader::disassembleImage()` goes from a
  file to a populated `Graph`, seeding entry + function symbols as function
  starts. Wrapped in the C shim (`dragon_loader_*`) and Python
  (`dragon.LoadedImage`). **Still not started:** Mach-O (the third
  `llvm::object` backend — the `LoadedImage` abstraction already allows for
  it), and the metadata that most helps function-start detection and
  debug-info ingestion the way NeverD does — `.eh_frame`/unwind tables and
  DWARF/PDB (the `debuginfodwarf`/`debuginfopdb`/`symbolize` LLVM components
  are already available when that work begins).

## Where to look for more detail

- `ToDo/LifterGaps.md` — the three originally-documented lifter bugs (#1
  and #3 now fixed and marked as such; #2 still open).
- `scripts/audit_x86_lifter_coverage.py --allow-missing` — live, current
  view of exactly which ALU-family opcode forms are still unhandled (does
  not yet track the INC/DEC/MOVZX/NEG/SHL-imm gaps found by the toy-vm
  exercise - those were found by hand, not by this tool).
- `tests/toy-vm/ToyVM.hpp` — full writeup (file header comment) of every
  X86IGNILLifter/LLVMLifter gap and bug found while building the
  VM-devirtualization proof-of-concept, with the exact instruction
  sequences that trip each one.
- Git log on `dev` for the full commit messages (each is a self-contained
  writeup of what/why for that piece).
