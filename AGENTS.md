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
  merges join paths that agree on a constant. Proven zero upper bits may be
  retained while the low bits stay unknown.
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
- Tests: `cd build && ctest -j3` (fast suite, ~380 tests with Z3/Unicorn);
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
- An XMM register is **two 64-bit halves**: XMMn (bits 63:0, offset 143+8n,
  the scalar-double lane) and XMMnH (bits 127:64, offset 271+8n). Every
  instruction that writes an XMM states what happens to XMMnH (movq/movd/
  movsd-load zero it, scalar arithmetic keeps it, packed ops compute it);
  an unlifted instruction writing XMM/YMM/ZMM clobbers both halves. Bits
  above 127 are not modelled. AArch64 V registers are 16 bytes.
- A64 scalar B/H/S/D destinations explicitly clear the rest of V128; this
  includes unknown scalar definitions. Lane insertion preserves other lanes,
  including FMOV to/from `Vn.D[1]`. Vector load/store widths determine address
  scaling and writeback, independently of the generic integer MC opcode used
  to dispatch them. LLVM names unscaled vector forms `LDURBi`/`STURQi`, etc.
  MOVI D/2D expands imm8 bits into byte masks; MSL forms encode the shift as
  264/272, whose low byte is the actual shift.
- A64 scalar FP32/FP64 operations share IGNIL's exact FPA primitives. FCMP
  uses NZCV=0011 for unordered. FCVTZS/FCVTZU saturate infinities/overflow
  and return zero for NaN; the source is guarded before integer conversion
  so the LLVM path cannot evaluate an overflowing FP-to-int into poison.
- x86 memory-operand forms of shifts/mul/bt*/shld/bsf/xadd/BMI are lifted
  through their register form over a scratch GPR (`MemViaReg`,
  `rewriteThroughScratch`): any fix to a register form fixes its memory form.
- IGNIL CALL does not push a return address and RET has no operand (x86 and
  ARM alike) — a deliberate abstraction. x86 RET stores any imm16 byte count
  in aux for clients; current backends do not pop the guest stack. Difftests show 3 expected diffs on
  AArch64 `bl/blr/ret` because of it.
- Float ops (FADD/FSUB/FMUL/FDIV/SITOFP/FPTOSI/FLT/FEQ/FUNO) treat 64-bit
  slots as IEEE doubles. A NaN result keeps its class but has independent
  unknown sign/payload bits (Z3 does not fix NaN bit patterns).
- Binary32 uses distinct `*32` arithmetic/comparison/conversion operations;
  it never computes through binary64. `FPEXT` and `FPTRUNC` change precision,
  `UITOFP` / `FPTOUI` provide unsigned conversion, and `FSQRT`, `FFLOOR`,
  `FCEIL`, `FTRUNC` (with binary32 variants) provide unary operations.
  Arithmetic and precision/int-to-float conversion use nearest ties-even;
  float-to-int uses toward-zero. Instruction lifters must guard exceptional
  integer conversions before evaluating them (LLVM otherwise produces poison).
  `lib/utils/FloatOps.hpp` shares exact Z3 FPA semantics between the executor
  and slice-expression lowerer; LLVM temps always hold raw integer bits.
- LLVM guest-memory accesses and external byte-packed register snapshots use
  alignment 1. A guest vector address or XMM snapshot offset need not have
  native alignment; stack allocas retain their own alignment.
- SSE lane operations snapshot both sources before writing either XMM half.
  PMOVSX/PMOVZX and scalar memory sources read only the architectural operand
  width, so a valid operand at a mapped-page end does not overread. Saturating
  integer ops widen before arithmetic and clamp before narrowing. MIN/MAX
  return the second operand's raw bits for either NaN and for equal signed
  zeros. CVTTSS2SI/CVTTSD2SI return integer indefinite for NaN/overflow; their
  invalid source is replaced with zero *before* FP-to-int (avoiding LLVM
  poison). Range checks use the truncated value to accept fractional inputs
  just below the signed minimum whose toward-zero result is representable.
- x87 uses physical raw 80-bit `FPU0..7` at offsets `400+10*i` (64-bit
  `FPUiL` plus 16-bit `FPUiH`), TOP8 at 480, full TAG16 at 482, CW16 at
  484 and SW16 at 486. `ST(i)=FPU[(TOP+i)&7]`; LLVM ST register numbers
  must never be mapped directly to physical registers. FPTOP is authoritative
  when composing SW's TOP bits. Register-file extents include all 10 bytes.
  Loads, stores, integer conversion and comparisons use integer bit operations,
  preserve extended precision and obey CW.RC; PC-dependent arithmetic stays
  UNDEF + unlifted. Masked stack faults produce extended indefinite and IE/SF.
  Unmasked exceptions conservatively make affected data/stack/status unknown; this
  library does not deliver hardware/OS traps or model FIP/FDP. Undefined
  condition bits are independent UNDEF. Legacy x87 environment stores invalidate the
  actual written byte range; unsupported MMX invalidates its aliased FPU state,
  and EMMS empties tags. FXRSTOR/XRSTOR-family restore fallbacks also invalidate
  all modelled XMM halves. x87 addresses honor 16/32/64-bit wrapping and
  RIP-relative next-PC setup; unknown segments/base registers are explicitly
  unlifted rather than replaced with zero. Control-word reserved bits normalize to `(CW&0x1f3f)|0x40`.
- Every executor UNDEF execution gets a fresh Z3 constant, including repeated
  operations at one address and destinations of different widths. Arithmetic
  NaN results have independent sign/payload bits constrained to quiet NaN;
  Z3's single underspecified `fp.to_ieee_bv(NaN)` term must not correlate them.
- `LABEL` / `LOCALJMP` / `LOCALCJMP` use `Op::aux` for block-local labels.
  They model runtime REP loops without creating fake machine instructions or
  CFG addresses. Temp definitions must dominate uses; loop-carried state is
  held in registers, which LLVM promotes to PHIs. A trace retains local flow.
  Symbolic execution requires uniquely determined local conditions and stops
  with an explicit error after 100000 local operations; it never guesses a
  symbolic repeat count. Linear IGNIL optimizers preserve these blocks and
  slicing keeps linear prefixes/suffixes and registers untouched by the local
  region; loop-written values and memory crossing the region stay unknown.
- MOVS/STOS/LODS/CMPS/SCAS use DF, architectural element width and address
  width (including ECX/ESI/EDI in x64 addr32). REP checks zero before any
  memory access or architectural write; REPE/REPNE comparisons check ZF
  after each subtraction. Each iteration reads memory before writes, so
  overlapping copies retain sequential hardware behavior. FS/GS source bases
  remain unmodelled and are explicitly recorded as unlifted.
- `Disassembler::AnalysisHints{entryPoints, readMemory}`: entry points are
  function + block starts and recursive-traversal roots; `readMemory` lets
  jump-table resolution read tables outside the analysed bytes.
  `loader::disassembleImage` fills both and re-analyses sections for
  cross-section call/jump targets (e.g. `.plt`).
- `LoadedImage::functionEntries()` contains sorted metadata seeds independent
  of symbols: ELF FDE / EH header, init/fini arrays and code REL/RELA targets;
  PE x64 exception records, exports and TLS callbacks. Only file-backed
  executable addresses are accepted. `disassembleImage` feeds these into
  AnalysisHints in both strategies. ELF relocation reads must use checked
  `ELFFile::rels/relas/symbols` results: `RelocationRef::getType()` can abort
  on a corrupt relocation table. LLVM's DebugInfoDWARF component is linked
  for the FDE parser. AArch64 decode failures advance one 4-byte word, never
  one byte: byte-wise recovery invented unaligned instructions and swallowed
  an FDE's BTI entry after embedded data in libcrypto.
- Resolved indirect-jump targets are stored as branches of the jump
  instruction, so they are CFG successors, function members and an LLVM
  `switch`. An unresolved indirect jump has no successors.
- `Configuration::callingConvention` (Default/SysV/Win64) drives argument
  recovery; `disassembleImage` picks Win64 for x86-64 PE.
- C/Python expose this as `dragon_config_*calling_convention` /
  `Configuration.calling_convention`. `dragon_analysis_hints_t` owns copied
  entries and borrows a synchronous full-range memory reader; Python
  `AnalysisHints(entry_points, read_memory)` keeps the callback alive.
  `Disassembler.analyze(..., hints=...)` propagates callback exceptions only
  after native analysis returns and frees any intermediate graph. Returned
  graphs do not borrow hints. Closing hints invalidates their Python access.
  C-shim tests use a separate `dragon_c_tests` target to avoid duplicate LLVM
  command-line option registration with the main suite's static LLVM linkage.

## 4. Validation tools (`scripts/validation/`)

**Unicorn version.** Unicorn 2.1.4 (latest release, QEMU 5.0) is wrong on
BZHI with an index >= the operand size and on BLSI's CF; the dev branch
(commit 2a05c41, 2026-10-18) has the QEMU fixes and agrees with x86 code run
under Rosetta. On the VM it is built into `~/ucdev` (run harnesses with
`~/ucdev/bin/python3`). difftest_x64 probes the installed Unicorn and only
compares those two instructions fully when it is fixed.

Run on the VM from any directory; they import `python/dragon.py` relative to
themselves. Binaries are passed as paths (`~/bench/...`).

| Script | What it checks |
|---|---|
| `difftest_x64.py` | 992 x64 templates incl. runtime string/REP forms: 986 match, 0 diff, 6 existing unlifted (16-bit push/pop and LEA; listed below). Compares GPRs, flags and memory; string cases also vary/compare DF. `ONLY=text` filters. |
| `difftest_strings.py` | 165 forms / 3960 Unicorn executions: i386, x64, x64 addr32; counts 0/1/2/5, DF 0/1, overlap and comparison exits. All registers/flags and the complete data buffer must match; no unknown outputs allowed. |
| `difftest_a64.py` | 835 AArch64 templates, including scalar FP edges, SIMD moves and memory; compares all 32 V registers as 64-bit halves. Expected: 3 diffs = CALL/RET abstraction, 0 unlifted. `ONLY=text` filters. |
| `difftest_x87.py` | Raw physical 80-bit registers, TOP/TAG/CW, defined SW bits, GPRs and memory in i386/x64 across all RC modes. Exact Fraction status oracle plus native fixtures cover documented Unicorn bugs. `ONLY=text`, `TRIALS=N`. |
| `difftest_sse.py` | 393 SSE/SSSE3/SSE4.1 templates x 60 inputs; includes saturation boundaries, FP32/64 exceptional values and conversions. Checks FP32 per lane, including untouched lanes; only active arithmetic NaN results may be unknown. `ONLY=text` filters. |
| `opt_equiv.py` | IGNIL `simplify()`/`dead_store_eliminate()` preserve semantics on random blocks (`CONTROL=1` adds a deliberately unsound variant to prove the check bites). |
| `slice_diff.py` | BackwardSlicer vs SymbolicExecutor on random blocks mixing 8/16/32/64-bit writes. |
| `memory_slice_diff.py` | Stack-slot STORE/LOAD forwarding vs 448 Unicorn executions; 192 unique slices, 0 wrong. Includes disjoint/overlapping/unknown-alias stores and SP changes. |
| `loop_slice_diff.py` | Nested-join loop slices vs 128 Unicorn executions: invariants, differing writes, increments and cross-register copies. |
| `guard_truth.py` | 27 CFGs vs 20,736 Unicorn executions: byte/word/dword guards, signedness, boolean holes, narrowing and masks. Also runs without Z3. |
| `llvm_diff.py` | LLVM-lifted code (compiled with `llc-21`, run natively) vs SymbolicExecutor. |
| `analyze_bin.py BIN OBJDUMP` | Whole-binary report: objdump agreement, function recovery, empty functions, IGNIL coverage + top unlifted, indirect jumps, LLVM lift success. `STRATEGY=0/1`, `LLVM=0/1`. |
| `table_truth.py BIN OBJDUMP STRATEGY` | i386 absolute jump tables: resolved targets vs the table in the image. |
| `truth_rel.py BIN OBJDUMP STRATEGY` | x86-64 PIC / AArch64 relative tables vs the image. |
| `fstarts.py BIN [TRUTH_BIN]` | Entry recall and inside-body false positives against the corresponding unstripped binary; instructions and analysis time. PE exception ranges provide sizes missing from COFF symbols. |
| `unlifted.py BIN [N]` | Census of unlifted instruction *forms* (mnemonic + operand shape). |
| `corpus_check.py DIR` | Every binary of a directory, one subprocess each: objdump agreement, empty symbol functions, blocks without IGNIL, edges to undecoded addresses, LLVM lift + `opt -passes=verify`, time. Corpus on the VM: `~/bench/corpus` (sqlite/zlib/Lua × gcc aarch64 -O0/2/3/s, mingw i686/x64 -O0/2/s, stripped, system bash/libz; `~/bench/build_corpus.sh`). |
| `realblock_diff.py BIN [BLOCKS] [TRIALS]` | Real basic blocks of a binary vs Unicorn from random states (x86-64/i386/AArch64). Expected: only the deliberate x86 OF difference after multi-bit shifts (see `emitGatedShiftFlags`). Unicorn truncates addresses >= 2^47; the script keeps values below that and compares only writes into known regions. `REALBLOCK_OPT=1` runs `simplify()` + `dead_store_eliminate()` first. |
| `realblock_llvm.py BIN [BLOCKS]` | Real blocks lifted to LLVM IR, compiled with llc-21, run natively on the AArch64 VM vs Unicorn. Needs `sysctl vm.mmap_min_addr=4096` (shared objects are based at 0). Skips trials that load a 64-bit value >= 2^47 (Unicorn truncates). `REALBLOCK_ONLY=0xADDR` runs one block and keeps its work dir. |
| `asm_roundtrip.py BIN [N]` | Every distinct instruction: text -> Assembler -> Disassembler must give the same text (110k+ instructions, all same). |
| `fuzz_loader.py OUT N SEED...` | Damaged ELF/PE inputs through load/disassemble/LLVM in a child process. Only a loader ValueError is a clean rejection; crashes, hangs and later exceptions are failures and make the script exit nonzero. |

The `[join-width]` C++ tests cover partial join knowledge on x86-64/AArch64,
guarded dispatch and 240 Unicorn loop executions. A narrow guard alone never
proves that a wider index's upper bits are zero.

The two table truth scripts use `table_bounds.py` to trace the actual index
through predecessor instructions. A nearby comparison of another register
is not a bound. Every path must agree; cycles, redefinitions and unsupported
forms produce an unknown oracle. Unreadable table ranges are reported
separately, never counted as verified. `tests/python/test_table_bounds.py`
regresses unrelated comparisons, index redefinitions and bypass paths.

`x87_native.c` is a second x87 reference: compile with `clang -arch x86_64 -O0`
on macOS/Rosetta (or gcc on real x86). It reproduces status flags, reserved CW
bits, stack faults, NaN conversions, FISTTP overflow and exception priority.
Unicorn omits conversion exception/C1 updates, fails to quiet some SNaNs,
returns saturated FISTTP overflow values and preserves AF/SF/OF after FCOMI.
The x87 difftest documents narrowly corrected reference fields. Real-block
harnesses initialize/compare physical FPU state and correct FCOMI flags before
later instructions consume them. They report/skip stack-fault trials and cases
exporting incomplete Unicorn exception status; these exclusions are not passes.
Native LLVM also compares each defined SW bit to symbolic execution.

## 5. Current state

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
processing; SSE low-half + FLT/FEQ/FUNO; optimistic x86 indirect-jump resolution
with whole-candidate revalidation/rollback and nested-join loop invariants.

Historical Lua baseline before the 2026-11-02 task series (linear sweep unless noted):

| | i686 PE | x86-64 PE | aarch64 ELF |
|---|---|---|---|
| objdump agreement | 100% | 100% | 100% |
| functions without blocks | 0 | 2 | 0 |
| unlifted instructions | 3.35% (x87 only) | 0.73% | 1.97% (FP only) |
| jump tables verified / wrong | 46 of 50 / 0 (4 guardless opcode dispatches) | 36 of 37 / 0 (linear and recursive) | 7 of 8 / 0 |
| LLVM lift | all functions | all | all |

The x86-64 relative-table script now follows the base register used by
`movsxd`, including a hoisted LEA outside its old 12-instruction window. This
finds 37 tables: 33 verified, 2 without a script-visible bound and 2 unresolved.
The two additional unresolved forms compare memory then reload it after a
possible write; they are kept unknown until non-aliasing can be proved.
`str_pack` (jump at `0x140021b50`) needed the slicer to preserve the zero upper
half of RBX across joins after an EBX write: its guard compares EBX while its
table uses RBX. The slicer queries the upper bytes independently and retains
`ZEXT(TRUNC(SymReg, 32))` when they are proven zero; low values from different
paths are never equated. Explicit entry hints seed speculative reachability
(plus direct callees); heuristic function starts after padded RETs can actually be cases
and must not inject independent unknown register states into that proof.
Performance: slices share one predecessor map per resolution round (x86)
or until the graph changes (ARM). Lua x86-64 analyses in ~2.4 s, sqlite
aarch64 -O3 in ~7 s, bash in ~4 s. What is left is std::map/set lookups in
BasicBlock construction (Graph::get / isValidBlock). The slowest corpus
samples are x86 sqlite (x86-64 -O2 ~36-40 s, i686 -O2 ~18-22 s): the
speculative x86 resolver copies the graph twice per round
(copyGraph/reachableGraph) - the next profiling target.

Second corpus `~/bench/corpus2` (`~/bench/build_corpus2.sh`, 35 samples):
clang aarch64/x86-64 ELF, gcc x86-64 ELF, C++ (tinyxml2) for gcc/clang/mingw,
system libc/libstdc++/libcrypto/python3. Clean after: AArch64 `+all`
features by default, blocks that run into the end of the code, incremental
lifting in the x86/ARM resolvers (sqlite x64 36 s -> 11 s, python3 570 s ->
32 s), BSR/BSF of zero, `push rsp`/`pop rsp`, STLR and other side-effect
stores (were silently dropped), ELF non-SHF_ALLOC sections answering
address lookups at 0 (`Section::mapped`), PE imports without/with a bad ILT, non-UTF-8
names in Python.

The full x64 instruction difftest has six existing unsupported forms:
`push ax`, `pop ax`, `push word ptr [rbx + 8]`, `pop word ptr [rbx + 8]`,
`push sp`, and `lea ax, [rbx + rsi*2 + 3]`. Each was independently reproduced
against the saved pre-task shared libraries; they are not REP regressions.

2026-11-02 recheck of the current corpus2 files found two existing exceptions,
confirmed with the unchanged `e0224b9` implementation (plus API bindings) and
again after stack forwarding. `cxx_mingw32_O2.exe`: linear/recursive missing
IGNIL blocks 2/1, edges to undecoded addresses 14/13 (mostly target 0),
linear objdump missing 70. `sys_libcrypto.so`: linear missing IGNIL blocks 13,
one undecoded edge `0x2b9a80 -> 0x2b83c0`, objdump missing 102; recursive has
none of these block/edge issues. All LLVM modules verify. Do not describe
these corpus files as fully clean; compare against this measured baseline.

After metadata seeding and aligned A64 invalid-word recovery, `sys_libcrypto.so`
improves to 4 linear missing IGNIL blocks, 0 undecoded edges and 70 objdump
missing instructions; recursive remains clean. The i386 C++ and three x64 Lua
exceptions above are unchanged. All 69 samples still pass LLVM verification.

`corpus_check.py ~/bench/corpus` is clean on all 34 samples except a known
false alarm on the x86-64 Lua builds: 2 "blocks without IGNIL" and 2
"objdump-only" addresses are constants embedded in .text before x87 code.

Corpus findings fixed in this round: PXOR set flags; adds/subs (ARM) and
add/sub (x86) computed flags from the result when the destination is also
a source; LLVM IR lacked a dedicated entry block and terminators after
traps; a store to disjoint bytes of the same object ended a load's
snapshot. Every lifted module of the corpus verifies.

Slice values carry producing-op/program-point identity, including a bounded
loop context. LOADs share values only for the same event or a proven equal
memory snapshot and address. STORE/CALL/opaque instructions invalidate that
snapshot; a join creates a new one. Debug strings are not semantic identities:
conversion widths and origins matter. Graph-entry register inputs retain their
public Z3 names; unresolved values at different internal points are distinct.

Exact-width, equal-address STOREs can forward their saved value to a LOAD.
Disjoint writes can be crossed; CALL, unlifted/opaque instructions, unknown
aliases and overlapping writes of another width stop forwarding. Values are
sliced at the STORE, before later register changes. At joins, block-entry
address unknowns are rebased onto each predecessor edge and only agreeing
constant expressions are merged. Cycles and a shared memory-query budget
stop conservatively. A stack slot is not preserved across CALL merely because
it looks local. Tests use `[memory-forward]`; validation results for all 69
samples are recorded in `docs/agent-validation-2026-11-02.md`.

Jump-table prefix enumeration only proposes a count. GuardProof proves it
over all inputs using exact interval sets for supported predicates, otherwise
SMT with byte-aliasing register inputs (50 ms per check). UNKNOWN rejects the
bound. A reachable final case and a non-vacuous guard are required. Without
Z3, unsupported predicates stay unknown. CFG target discovery on both x86
and ARM requires exhaustive enumeration; a capped list is never a target set.
`solveUnique` also requires a proof of exhaustion.

## 6. Backlog (suggested order)

Known LLVM lowering gaps at call boundaries and out-of-function control flow, with
the implementation plan: `docs/llvm-control-flow.md`.

Ready-to-paste task prompts for other agents (shared preamble + one prompt per
item, with a status table to update when a task is done): `docs/agent-tasks.md`.

1. Stack-slot forwarding is implemented. The last AArch64 Lua table remains
   unresolved because a CALL invalidates its saved base; crossing that barrier
   requires a separate proven memory-effects model, not an optimistic exception.
2. Metadata entry seeding is implemented. Stripped AArch64 sqlite now recalls
   1874/1874 symbol entries under recursive traversal (formerly 4/1874 and
   only 48 instructions). PE leaf functions can lack unwind records; Lua's
   `register_tm_clones` has no FDE. Existing heuristic inside-body starts
   remain measured false positives; see the per-sample validation report.
3. **Lift coverage left** (`unlifted.py`): x87 extended arithmetic with CW.PC,
   transcendental/environment operations; AVX/VEX; AArch64 PAC/AUT, crypto,
   vector arithmetic and structured loads. Common scalar A64 FP, SIMD data
   movement, CLZ/RBIT, listed SSE/SSSE3/SSE4.1 forms and runtime x86 strings
   are implemented. Do not replace remaining extended arithmetic with double.
4. C shim/Python analysis hints and calling conventions are exposed; regression
   tests: `tests/TestCAnalysisHints.cpp`, `tests/python/test_analysis_hints.py`.
5. Extend the speculative resolver to ARM when useful. Failed hypotheses remain
   unresolved; never publish partial target sets or skip the final recheck.
6. Design questions, not bugs (ask the owner before changing): CALL/RET
   abstraction; the loader marks defined `.symtab` globals of an executable
   (e.g. `__bss_start`) as exported.

## 7. Where things are

- Analyses: `lib/analysis/` (BackwardSlicer, SwitchDetector, SliceExprZ3,
  ArgumentRecovery, ValueEnumerator).
- AArch64 `writesMemory()` names the side-effect instructions that write
  nothing (HINT/barriers/PRF/CLREX/TME); LLVM's side-effect flag is also on
  real stores (STLR, ST64B, ...), so never use it alone.
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
