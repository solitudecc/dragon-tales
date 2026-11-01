# AGENTS.md — handoff notes for AI agents working on this fork

This is `solitudecc/dragon-tales`, a fork of `Fare9/dragon-tales` (C++20 /
LLVM 21 binary-analysis library: disassembly, CFG/function recovery, the IGNIL
IL, x86/ARM IGNIL lifters, backward slicer, switch detection, Z3 symbolic
execution, LLVM IR lifting, FLIRT signatures, ELF/PE loader, C shim + Python
cffi API). Read `README.md`, `IGNIL.md` and `docs/` for the public surface;
this file is about *how to work here* and *what state things are in*.

The repository owner communicates in Chinese; reply to them in Chinese.

---

## 1. Ground rules

- **Never give wrong semantics.** An instruction the lifter cannot express is
  lifted as UNDEF for everything it defines and recorded in
  `ignil::Block::unlifted()`. A partial or approximate lift that can produce a
  confidently wrong value is worse than an honest UNDEF. The same applies to
  analyses: the switch detector rejects a bound it cannot prove (a masked index
  is *not* a bound — see `lib/analysis/SwitchDetector.cpp`), the slicer only
  merges join paths that agree on a constant.
- **Every fix gets a regression test, and the test must fail on the old code.**
  Check that before committing (apply only the test change to `origin/main`,
  build, run, see it fail; then apply the fix). If the test needs a new API and
  cannot compile on old code, say so in the commit message and show the
  before/after numbers from a real binary instead.
- **Validate semantics against a reference, not by reading code.** Use the
  Unicorn difftests and ground-truth scripts in `scripts/validation/` (§4).
  Unicorn values are the expected values in unit tests.
- **Git workflow** (what the owner asked for): one branch per fix
  (`fix/...` or `feat/...`), commit with an explanatory message ending in
  `Co-Authored-By: <model> <noreply@anthropic.com>`, push the branch,
  fast-forward merge into `main`, push `main`, delete the branch locally and on
  the remote. Commit author is configured repo-locally (`solitudecc`).
- **Do not open PRs or push to upstream `Fare9/dragon-tales`** without the
  owner's explicit consent.
- `ToDo/` is in `.gitignore` but its files are tracked: stage them with
  `git add ToDo/<file>` or `git add -u`, never `git add ToDo`.

## 2. Build and test environment

Development happens on macOS; **building and testing happens on an Ubuntu VM**
(aarch64, 3 GB RAM): `ssh cc@192.168.10.169` (key-based; ask the owner if keys
are missing). It has LLVM 21.1.8, Z3 4.13.3, Unicorn 2.1.4 (+ Python bindings),
pyelftools, pefile, gcc, `i686-`/`x86_64-w64-mingw32-gcc`, `llc-21`.

- Clone/build: `~/dragon-tales`, build dir `~/dragon-tales/build`
  (RelWithDebInfo, Ninja). `cmake --build build -j3` (more jobs can OOM).
- Tests: `cd build && ctest -j3` (fast suite, ~360 tests);
  `./build/bin/dragon_tests "[semantic]"` (JIT round-trip vs Unicorn, slow);
  `"[expensive]"` exists too.
- Real-world binaries for whole-program checks: `~/bench/lua32.exe`,
  `~/bench/lua64.exe` (mingw -O2, with symbols), `~/bench/lua_a64` (native gcc
  -O2), built from Lua 5.4.7 sources in `~/bench/lua-5.4.7`.

Workflow that works: edit locally, `git diff origin/main > all.patch`
(**diff against origin/main**, not HEAD, if you have unpushed commits),
`scp` it, on the VM `git reset --hard origin/main && git apply ~/all.patch`,
build, test. Use `ssh -n` (otherwise ssh eats heredoc stdin).

Gotchas:
- Catch2 treats commas in a test-name filter as separators: name tests
  without commas or filter by tag.
- A private nested struct must be declared before member functions that use
  it in the header, or GCC reports "does not name a type".
- Python `Disassembler.disassemble()` does not lift; use `analyze()` (or
  `LoadedImage.disassemble()`) when you need IGNIL.
- IGNIL opcodes are mirrored by value in `c_shim/dragon_c.h`
  (`dragon_ignil_opcode_t`) and `python/dragon.py` (`IGNILOpcode`): **append
  new opcodes at the end** of `ignil::Opcode` and `lib/IGNIL_OPS.def`, add the
  mirrors and a `static_assert` in `c_shim/dragon_c.cpp`.
- Appending fields to a struct mirrored in the C shim
  (`dragon_switch_candidate_t`): add them at the end, update `dragon_c.cpp`
  and the Python wrapper.

## 3. Model invariants worth knowing

- Registers live in a byte-addressed register space (`Reg{offset, width}`):
  RAX/EAX/AX/AL share offset 0, AH is byte 1. **A 32-bit write zeroes the rest
  of its 64-bit parent** (x86-64 EAX→RAX, AArch64 W→X); 8/16-bit writes merge.
  SymbolicExecutor, dead-store elimination, BackwardSlicer and LLVMLifter all
  follow this — keep it that way.
- XMM registers are modelled as their **low 64 bits only**; AArch64 V
  registers as 16 bytes. 128-bit x86 stores are therefore left unlifted.
- IGNIL CALL does not push a return address and RET has no operand (x86 and
  ARM alike) — a deliberate abstraction; difftests show 3 expected diffs on
  AArch64 `bl/blr/ret` because of it.
- Float ops (FADD/FSUB/FMUL/FDIV/SITOFP/FPTOSI/FLT/FEQ/FUNO) treat 64-bit
  slots as IEEE doubles. A NaN *result* is left unknown by the executor (Z3
  does not fix NaN bit patterns).
- `Disassembler::AnalysisHints{entryPoints, readMemory}`: entry points are
  function + block starts and recursive-traversal roots; `readMemory` lets
  jump-table resolution read tables outside the analysed bytes.
  `loader::disassembleImage` fills both and re-analyses sections for
  cross-section call/jump targets (e.g. `.plt`).
- Resolved indirect-jump targets are stored as branches of the jump
  instruction, so they are CFG successors, function members and an LLVM
  `switch`. An unresolved indirect jump has no successors.
- `Configuration::callingConvention` (Default/SysV/Win64) drives argument
  recovery; `disassembleImage` picks Win64 for x86-64 PE.

## 4. Validation tools (`scripts/validation/`)

Run on the VM from any directory; they import `python/dragon.py` relative to
themselves. Binaries are passed as paths (`~/bench/...`).

| Script | What it checks |
|---|---|
| `difftest_x64.py` | ~530 single-instruction x86-64 templates vs Unicorn (regs, flags, memory). Expected: 0 unlifted, 2 known harness false positives (16-bit `shld/shrd` with count > 16 — architecturally undefined). |
| `difftest_a64.py` | ~530 AArch64 templates vs Unicorn. Expected: 3 diffs = CALL/RET abstraction. |
| `difftest_sse.py` | 35 SSE templates vs Unicorn incl. NaN/±0/inf; NaN results counted as "unknown", not wrong. |
| `opt_equiv.py` | IGNIL `simplify()`/`dead_store_eliminate()` preserve semantics on random blocks (`CONTROL=1` adds a deliberately unsound variant to prove the check bites). |
| `slice_diff.py` | BackwardSlicer vs SymbolicExecutor on random blocks mixing 8/16/32/64-bit writes. |
| `llvm_diff.py` | LLVM-lifted code (compiled with `llc-21`, run natively) vs SymbolicExecutor. |
| `analyze_bin.py BIN OBJDUMP` | Whole-binary report: objdump agreement, function recovery, empty functions, IGNIL coverage + top unlifted, indirect jumps, LLVM lift success. `STRATEGY=0/1`, `LLVM=0/1`. |
| `table_truth.py BIN OBJDUMP STRATEGY` | i386 absolute jump tables: resolved targets vs the table in the image. |
| `truth_rel.py BIN OBJDUMP STRATEGY` | x86-64 PIC / AArch64 relative tables vs the image. |
| `fstarts.py BIN` | Classifies function starts: symbol / inside a symbol body (false) / other. |
| `unlifted.py BIN [N]` | Census of unlifted instruction *forms* (mnemonic + operand shape). |

## 5. Current state (main = `9b1f625` at the time of writing)

Work done in this fork (see `git log` for full messages): ARM32 branch
targets; ARM lifter collisions; Python borrowed-pointer lifetimes; block
boundaries; FLIRT CRC16; AArch64 ROR; x86 jrcxz/loopcc + IMUL flags; loader
`.dynsym` and PE import/export; slicer sub-register aliasing; switch detection
on real IR + `jmp [mem]`; LLVM lifter W-register zeroing and AH sync; Z3
independent undefs; CFG entry points / call-following traversal / real
indirect-jump edges; function-start heuristics; Win64 calling convention;
multi-byte nops; reachable-code decoding after resolution; AArch64
register-offset memory + extended add/sub; relative jump tables; slicer
join merging; x86 integer memory forms + moffs; AArch64 integer data
processing; SSE low-half + FLT/FEQ/FUNO.

Numbers on the Lua builds (linear sweep unless noted):

| | i686 PE | x86-64 PE | aarch64 ELF |
|---|---|---|---|
| objdump agreement | 100% | 100% | 100% |
| functions without blocks | 0 | 2 | 0 |
| unlifted instructions | 3.35% (x87 only) | 0.73% | 1.97% (FP only) |
| jump tables resolved / wrong | 43 of 50 / 0 (4 are guardless Lua dispatch, 3 unverifiable) | 29 of 34 (recursive) / 0 | 7 of 8 / 0 |
| LLVM lift | all functions | all | all |

## 6. Backlog (suggested order)

1. **Optimistic, iterative indirect-jump resolution.** The last x86-64 tables
   fail because proving the table base loop-invariant needs the case blocks'
   own edges back to the loop head — which exist only once the table is
   resolved. Resolve tentatively, then re-check. (`truth_rel.py` shows them.)
2. **Stack memory tracking in the slicer** (store→load through `[sp, #N]`):
   the last AArch64 table reloads its base from the stack.
3. **Lift coverage left** (`unlifted.py`): x87 (needs an FP-stack model);
   x86 128-bit stores (`movups [m], xmm` — needs full 128-bit XMM modelling),
   `cmpsd`/`cmpnlesd`, `punpck*`, `pshufd`, `bsr`, `tzcnt`, `popcnt`, `lzcnt`;
   AArch64 FP (`fmov`, `fcmp/fcmpe`, `scvtf`, `fcvtzs`, `frintm`, `ldr/str`
   of D/Q registers — note a 64-bit write to a V register must zero its upper
   half, which the current 32-bit-only zero-extension rule does not do),
   `clz`, `rbit`. Validate each with the difftests and add templates.
4. Expose `AnalysisHints` and `callingConvention` through the C shim and
   Python (today only C++ and `disassembleImage` use them).
5. Design questions, not bugs (ask the owner before changing): CALL/RET
   abstraction; the loader marks defined `.symtab` globals of an executable
   (e.g. `__bss_start`) as exported.

## 7. Where things are

- Analyses: `lib/analysis/` (BackwardSlicer, SwitchDetector, SliceExprZ3,
  ArgumentRecovery, ValueEnumerator).
- Lifters: `lib/disassemblers/x86/X86IGNILLifter.cpp` (table-driven generic
  paths: `memOperandOps_`/`liftMemOperandForm`, `sseOps_`/`liftSse`),
  `lib/disassemblers/arm/armIGNILLifter.cpp` (`memForms_`, `extAddSub_`,
  `intOps_`/`liftIntOp`); analysis passes `x86Analysis.cpp`/`armAnalysis.cpp`
  (block/function discovery, `resolveIndirectJumps`, `integrateNewBlock`).
- Core: `lib/BasicBlock.cpp`, `lib/Function.cpp`, `lib/Graph.cpp`,
  `lib/SymbolicExecutor.cpp`, `lib/LLVMLifter.cpp`, `lib/IGNILSimplify.cpp`.
- Loader: `lib/loader/Loader.cpp`, `lib/loader/ImageDisassembler.cpp`.
- Tests mirror the layout under `tests/`; fixtures for the loader are
  generated by `tests/loader/fixtures/make_fixtures.py`.
