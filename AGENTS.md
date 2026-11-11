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

The full correctness rules - validation layers and their blind spots, rules for
test harnesses (prove they catch errors, edge-value inputs, the only allowed
exclusions, never loosen a check to pass), what to do when the reference
disagrees, known deliberate differences, reporting - are in
`docs/correctness.md`. Read it before changing any semantics.

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
- **Commit substantial milestones** (owner request, 2026-11-09). Keep a Git
  commit for a completed implementation milestone after its focused and full
  tests pass. State any ongoing corpus validation explicitly; commit the final
  results separately and finish required validation before merging into `main`.
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
  in aux for clients; the abstract executor and Legacy do not pop the guest stack.
  StatefulV2 models machine CALL/RET at its execution boundary. A64 ordinary RET aux
  is 0 for legacy X30, or 1..32 for X0..X31 (X31=XZR); still no IGNIL operands.
  Difftests show 3 expected diffs on
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
  and explicit x86-32 source segments remain unmodelled and are explicitly
  recorded as unlifted.
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

Checked-memory incoming edges (2026-11-11, baseline `50f698c`): see
`docs/llvm-memory-checked-edges.md`. After all ordinary GuestMemorySSA worklists,
bounded copies specialize one proved predecessor of a shared checked LOAD.
Exact same Guest base/64-bit byte-GEP address proofs use PHI translation plus
restricted flag-free Add/Sub structural matching. No frame, image, alias or
callee-entry assumptions. Visible undef/poison and unsafe flags stay unknown;
freeze is an opaque stable SSA boundary, never stripped or reused across a
new region execution. All original guards, unknown incoming paths, original
LOADs and failure exits remain. Outside PHIs and all live-outs (including loop
incoming uses) are repaired. Cross-block and PHI options must both be enabled;
public ABI and default optimize order are unchanged. Private per-path/module
caps bound proof visits, cloning and SSA repair; partial safe IR may remain
with incomplete explicitly reported. New blocks cannot recursively expand the
same run. `blocksVisited`/`phisInserted` include copied blocks/SSA repair.
Final same-source 16-case regressions: old 153 failed assertions, v1 50 failed,
v2 873/873 pass. V1 native gains correctly fail on Sub, not silently accepted as
no-op compatibility. Final main/no-Z3 focus 16/873, related 70/2393; CTest
903/903 and 768/768. Final integrated native: 2176 matches, 88/88 specified
faults detected in 192 IR-control observations and 5987 comparator controls.
Compare 512-byte regs, full 64 KiB, seven Exit fields, result and call count.
Previous irreducible native matrix rerun: 1056 matches, result/memory controls
1056 each. Its new 1-edge copy retains the original LOAD; the old local-fact
test now checks both disabled-copy independence and explicit-edge structure.
Real raw and matched post-GVN pipelines each complete C16/LLVM16, with full
state and all field controls; repeat inputs are not new sample coverage.
Matched post-GVN extra forwarding old->new: f-cff O0 0->1, f-combo O0 0->4,
both O2 zero. Eight frozen IRs verify, six retain explicit budget incomplete.
Actual CFF 1->1 and original switch 1->1; table LOAD/defaults remain. RIP cleanup
is separate, and root execution match is not analysis completeness. No new
single-instruction/IGNIL or 69-input corpus run is claimed. Backend frozen as
`bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003` under
`~/vmc-runs/checked-edge-memory-20261111/integrated-runtime-v2/`; independent
native/real evidence is under `~/vmc-runs/guest-memory-checked-edges-20261111/`.

Local state transfers and conservative memory coverage (2026-11-11, baseline
`26e1fce`): see `docs/llvm-local-state-repairs-20261111.md`. Deflattening scans
backward within one predecessor for the nearest exact state STORE, allowing
retained pure operations, simple LOADs and explicit readonly/readnone calls.
Unknown/partial/other writes, unknown calls and synchronization remain barriers.
A direct select with a proved defined condition can reuse that exact condition
to branch to two independently proved targets; outer guards, live-out values,
target PHIs and unknown incoming paths remain. Scan and value proof share each
edge's expression budget, with incomplete reported on exhaustion. Both dispatch
passes reject flagged trunc/zext and disjoint OR constant proofs through freeze.
GuestMemorySSA now locally scans reachable blocks missed by the predecessor
scheduler, after all ordinary loop proofs and deferred rewrites. Every missed
block starts empty; no predecessor/successor facts or PHIs, and no new alias or
loop invariant assumptions. This is not a general SCC fixed point.
Old-code regressions actually fail: reaching-store 15, base conditional 71,
irreducible coverage 22, flagged casts 32, disjoint OR 10 failed assertions.
Final main/no-Z3 focus: 20 cases / 1211 assertions; related components 54/1502.
CTest 887/887 and 752/752. Frozen integrated native groups: reaching-store 864
full-state matches plus two faulty-IR controls; conditional state 1024 matches
and four specified controls; irreducible memory 1056 return/full-64-KiB matches
and 1056 detections each for result/memory controls. No exclusions, skips or
normalizations. Conditional fixture rewrites increase 1->14 edges; cross-call
counters remain zero. Real frozen four-ELF replay C16/LLVM16 completes with full
state, but real target switches remain 2->2 and all four deflat passes rewrite
zero edges. RIP routing 1->0 is not CFF removal. Eight real-IR probes improve
scan coverage but still forward zero loads and retain identical output. Further
work needs edge-local Guest address/value proofs across success checks; never
globally assume RBP=entry SP-8 or immutable image tables. No new instruction
campaign or full 69-input corpus run is claimed. Frozen runtimes, failure logs
and real replay are under `~/vmc-runs/llvm-local-state-20261111/`.

Same-function dispatcher hardening (2026-11-11, baseline `9020c70`):
`dispatch-deflatten` now threads each proved predecessor independently; unknown
CALL/state edges retain the original dispatcher. A dispatcher can count as both
rewritten and unresolved. Never bypass live-out PHIs or volatile/atomic selector
loads, and never update only the destination list of an `indirectbr`.
The experimental cross-call entry fallback was removed: `CallMemoryConstant`
describes a normal-return postcondition, not a callee-entry fact. Distinct nested
pointer objects and intervening callee writes were also unsoundly conflated.
Compatibility fields remain, but even explicit `consumeCallEffectSummaries=true`
does not activate that path; all three cross-call counters remain zero.
GuestMemorySSA still consumes valid normal-return GuestMemory write effects.
Rare cross-function state transformations belong in a separate, proved pass.
See `docs/same-function-dispatch-fixes-20261111.md` and
`docs/same-function-cff-validation-20261111.md` for controls and final counts.
Final CTest: Z3 867/867, no Z3 732/732; dispatcher/call focus 20 cases / 403
assertions in each build. Native same-function 160, CALL 336 and isolated-entry
24 comparisons match; old live-out PHIs and cross-call entry assumptions both
have actual native wrong-result controls, not only structural/verify failures.
On real Tigress, regs+128 switches are RIP entry routers, never CFF gains.
Actual next state is a Guest stack slot at O0 or RCX at O2; decoded-region
coverage retains real table LOADs, target checks and default exits. Do not turn
`.rodata`, file table bytes, or a successful replay into immutability proof.

System/exception boundaries (2026-11-10, baseline `d0fab8d`): see
docs/system-transition-boundaries.md. Appended IGNIL UNSUPPORTED=72 (C/Python
mirrors) stops before an unmodelled environment/exception transition, including
trace execution; it has no operands/destination/aux and produces no post-state.
Do not replace it with normal CALL/RET or claim a kernel return. Architecture
gates precede generic control dispatch; HLT is explicit because LLVM does not
flag it as a trap. Simplify/DSE preserve the block, LLVM Legacy refuses and
Static/V2 report the exact Unsupported/Instruction boundary. Analyses reject
suffix facts even without a marker, while retaining real incoming CFG paths
as Unknown (never turn an unknown environment return into proven unreachability).
Old final tests compile and fail: 12/13 cases, 724 failed assertions. New main
13 cases/2424 assertions, no-Z3 10/1736; CTest 827/827 and 692/692, plus 31 pure
validator controls. Main/no-Z3 native: 1536 full-state pre-instruction boundary
matches (zero complete roots), 256 detected single-field controls; no exclusions,
skips or normalizations. Frozen Tigress two-ELF replay totals C32/LLVM64 complete
roots. The current implementation also completed 69 inputs / 138 strategy
records, 205018 valid function checkpoints and 6476 raw+optimized modules with
zero lift/verify failures. This is IR validity coverage, not a whole-corpus
semantic proof. The old-structure rerun had one frozen-interpreter path
infrastructure failure; retain its stderr and do not describe it as a semantic
pass. Component success is not completed S2 memory forwarding. Use only the owned
`~/dragon-tales-overlap` worktree and frozen runtime pairs.

Deobfuscation S1b provenance (2026-11-09/10, baseline `9ff29d8`): internal
RegionLowering captures original op indices, actual Guest accesses/guards and
post-access points, CALL/RET auxiliary stack roles, real switch/defaults, call
request/normal/propagate/invalid edges, seven-field Exits and non-op boundaries.
Views expire after synchronous read-only consumption, including exceptions;
borrowed pointers must not escape. Consumer errors do not roll back the completed
module; discard failed targets. Seal is trusted-producer structural checking,
not value/alias proof or arbitrary external certificate validation. Max 1048576
records; failed/budgeted lowering skips consumption and restores declarations.
No lifter instance fields, Guest/C ABI, default pipeline or value rewrites.
Eight new cases/3725 assertions, main/no-Z3 group22/4828; CTest813/813 and681/681.
Native main/no-Z3 each240 (96 roots/48 Transfer/96 reject); specified result and
memory controls240 each. Frozen two-ELF main/no-Z3 eachC16/LLVM32 full roots.
Ordinary old/new eachC16/LLVM32incomplete, all seven Exit fields equal; not roots.
78 IR/Graph comparisons:72 byte-identical,6 exact ModuleID-path first-line-only.
Public raw probe old/new140 attempts each:122 accepted/18 rejected/0 errors,
140 output bytes equal,0 CPU/native/opt/verify; not semantic evidence. Old final
tests cannot compile absent internal header (saved). Orchestrator Transfer enum
mistake corrected against Execution.h (2 vs3); adopted seven raw groups and only
ran the eighth, without checker/input/field changes. No new exclusions/skips or
deobfuscation gain. Build9ff29d8+patch, artifact `~/vmc-runs/llvm-lift-provenance-20261109/`.
Details: docs/deobfuscation-progress-20261109.md. S2 first-stage exact Guest
memory forwarding is now implemented; see docs/llvm-memory-forwarding.md.

S2 Guest-memory SSA (2026-11-10): `LLVMLifter::runGuestMemorySSA` and C/Python
mirrors optionally promote registers, then forward exact-width and proven byte
Guest LOADs from prior LOAD/STORE facts. Acyclic CFG edges carry facts; joins keep facts present
on every edge and insert PHIs for differing values. Calls, unknown aliases,
volatile/atomic accesses and partial overlaps remain conservative barriers.
Calls preserve facts only when their LLVM memory effects prove readnone/readonly;
ordinary calls remain barriers.
Loop facts cross a backedge only after an exact entry/backedge check; a canonical
two-block loop may use a loop-carried PHI when its backedge value is loop-independent.
Unknown loop effects and other shapes remain barriers; no general loop fixed point is guessed. Default
optimize() is unchanged. The first-stage four cases/34 assertions remain green;
the cross-block/loop/byte/call focused group is 9/9 cases/62 assertions;
main/no-Z3 CTest 836/836 and 701/701. The old implementation fails the
narrow-load and readnone-call forwarding assertions. This is not heap analysis,
general loop fixed points or internal callee-effect summarization.

Deobfuscation S1a preparation (2026-11-09, baseline `ced8210`): internal
LLVMPreparedRegion owns a const Graph at a stable heap address. Strict and
rebuild region paths lower the same frozen view; strict keeps custom IGNIL and
markers, machine mode rebuilds every selected split prefix/suffix. Origin uses
instance/block/op index, not PC alone or a runtime memory snapshot. Different
preparations cannot share origins. Ordinary lift/ABI/default optimization stay
on their original paths; strict region now also pays whole-Graph clone cost.
prepare is not full lowerability or operational-CFG proof; lifter mode/entry
checks remain mandatory. Four new cases/223 assertions; main/no-Z3 region group
14/1103, full CTest 805/805 and 673/673. New internal API tests cannot compile
old headers (saved); this is not old execution error or deobfuscation gain.
Actual builds ced8210 + patch; artifacts `~/vmc-runs/llvm-prepared-region-20261109/`.
Main/no-Z3 native each 240 matches (96 root/48 Transfer/96 rejected); result
and memory controls each detect 240 specified single-field differences. Frozen
two-ELF main/no-Z3 each C16/LLVM32 full roots, no incomplete/errors. Against S0,
36 IR+16 Graph files: 48 byte-identical; four optimized files differ only in
exact input-path ModuleID first lines, all remaining bytes identical. No new
exclusions, skips, normalization or deobfuscation gain.
See docs/deobfuscation-progress-20261109.md; LLVM access/guard mapping is S1b,
and explicit S2 Guest-memory forwarding now covers acyclic CFG joins with PHIs.
Stack-slot reconstruction beyond the current Guest base, heap relations,
external-call effect summaries and loop fixed points remain outside this snapshot.

Deobfuscation S0 harness gates (2026-11-09, baseline `12b0043`): see
`docs/deobfuscation-progress-20261109.md`. Optional symbolic diagnostics probe
the C symbol; absent means unavailable/null proof counts, never semantic success.
Available diagnostic exceptions are saved and rethrown. --require-diagnostics
also requires complete diagnostics. Exact requested case/plan/trial coverage
rejects missing, extra, duplicate and malformed records, including bool/float IDs.
13 Python cases; complete CTest 801/801 and no-Z3 669/669. Frozen main/no-Z3 each
C16 and LLVM32 complete root matches,
all fields/1 MiB/Exit unchanged. No-Z3 RAX/memory controls each detect 16 specified
single-field differences; strict no-Z3 has 16 matches but correctly returns 1.
Old exact-ELF build-error control falsely returned 0 despite an absent case;
new returns 1 with the same remaining C8/LLVM16 matches. Available AttributeError
and RuntimeError controls propagate, never unavailable. Old no-Z3 missing-symbol
failure retained. No production library/comparator/ABI changes or new exclusions.
Final source-pinned raw and dual-host audit: `~/vmc-runs/deobf-s0-20261109/`.
This repairs validation, not dispatcher/MBA/heap deobfuscation.

Static decoded-region coverage (2026-11-09, baseline `d200f05`): see
`docs/llvm-static-region.md`. Explicit C++/C/Python liftStaticRegion requires
StaticAnalysis + FunctionStart, keeps original members and accepts additional
machine blocks; optional rebuildDecodedBlocks uses a private Graph clone and
existing instruction lifting with exact fallthrough. Real target switch has
all covered starts and a Transfer default; no Graph target/publication change,
image-content assumption, or V2/Legacy extension. Calls/RET retain protocols.
Caps4096 blocks/65536 total dispatch cases; rebuilding caps65536 instructions;
clone cost still depends on whole Graph. Failed used declarations restore attrs
and argument names. No MCInst cannot silently become an empty machine lift.
Ten cases/880 assertions main and no-Z3; CTest800/800 and668/668. New API tests
cannot compile old headers, not an old execution semantic defect. Native
three-architecture STORE-before-table-JMP oracle: main and NZ each240 matches
(96 complete roots/48Transfer/96rejected entries), result/memory controls240
each; frozen old0roots/96incomplete plus144boundary/reject matches, rc1 kept.
Two real O0 Tigress ELFs, linear/product O0,O2/eight inputs: oldC16/LLVM0root+
32incomplete, newC16/LLVM32 complete full-state/1MiB root matches; real Guest
megaInit runs, no heap snapshot. Graph semantic fingerprints unchanged.
Ordinary lift compatibility16 IR/8Graph files byte-identical; extra O2 files
differ only actual input-path ModuleID first line (raw originals retained).
Real dispatch switch/backedges remain; do not claim de-flattening or new opaque
proofs. No new exclusions/skips/normalizations. Optional NZ Tigress run stopped
before C/CPU/native/LLVM because original diagnostic requires unavailable
SymbolicExecutor; no16-match claim. No full69 corpus/single-insn campaign;
artifacts ~/vmc-runs/llvm-static-region-20261109/. Use owned VM worktree only.

Opt-in LLVM control-flow cleanup (2026-11-09, baseline `4487600`): see
`docs/llvm-control-flow-cleanup.md`. New `control-flow` pipeline builtin and
`LLVMLifter::runControlFlowSimplification` compose SCCP/DFAJumpThreading/
JumpThreading/SimplifyCFG/InstCombine/DCE with before/after IR counters. No
new entry/memory/call assumptions; historical default six steps unchanged.
Four C++ regressions fail old code (also with final tests and frozen old libs),
then pass new; CTest 789/789 and no Z3 657/657. Native state-machine oracle:
384 before/after comparisons (192 post-pass), result/memory controls 384 each.
Unchanged full Guest static-entry oracle: 3776 matches (1040 complete roots /
480 declared boundaries / 2256 rejected entries); result/memory controls 224
each. Exact V2 budgets 0/1/2/3: 192 matches (96 roots / 96 budget boundaries),
budget control 192. No-Z3 selected native entries: 336 (144 roots / 192 rejects).
No new exclusions/skips/normalizations. Pure LLVM PHI state machine loses its
dispatcher; historical Tigress f-combo O0 39-case dispatcher remains a Guest
LOAD switch despite small IR cleanup. Do not claim generic de-flattening or
heap-predicate removal. Implementation milestone `1a24a21`; final fresh two-ELF
O0 replay (two strategies / product O0,O2 / eight inputs): C32 match, LLVM
0 complete / 64 incomplete / 0 different / 0 other errors; original rc1 retained.
Actual Guest megaInit executes. Default Graph publishes no corresponding table
edges, so fresh IR already lacks those switches before cleanup. Both strategies
stop at f-mba JMP 0x10167 -> 0x101ac and f-combo JMP 0x10adc -> 0x1154c.
CFG wrong/unlifted zero; recursive executed-undecoded counts132/5704, linear0.
Do not describe those incomplete paths as full-function semantic matches. Next
component must prove dispatch value/memory relations within the IR input contract.
No full corpus or new instruction/IGNIL campaign in this opt-in step.
Artifacts: `~/vmc-runs/llvm-control-flow-20261109/`.

Decoded automatic entries (2026-11-09, baseline `83e8c8c`): see
`docs/decoded-entry-registration.md`. x86/ARM commitPending registers only
targets with a decoded first instruction; real branches/callees/conditional
fallthroughs remain, including zero. Actual code at zero is accepted. Public
Graph::addBlock/addFunction still permit manual undecoded/pure-IGNIL graphs.
Historical real edges reclassify already decoded late fragment interiors at
every commit, after direct recovery and each tentative expansion, before
lifting/revalidation. Missing targets stay with loader recovery to preserve its root
policy; replaying missing conditional successors early failed a permanent test.
Loader also reads instruction evidence directly, recollecting per section;
original eight-round limit remains. No decoder/lifter/API/ABI/optimizer changes.

Final13cases/2550assert pass in both builds; fresh CTest main785/785, NZ653/653.
Old main9/1864 fails5cases/66assert, four controls pass; added2/516 controls pass
old but guard-only intermediate fails100assert. Old NZ initial11/2380 fails5/66,
errors/skips0. Original old9 source is a separate exact dependency, not final13.
Two final timing regressions catch callee classification lost after direct
overlap/indirect expansion: actual v3 corrected13/2550 fails15assert; final0.
Old83 main/NZ final13 each77fail includes pre-existing flag differences, not all
new view failures. Initial test wrongly expected A64linear no-table constant BR
resolution; retained same bytes/all combinations but strictly checks that fixed
case empty as an existing policy negative control, all other targets/fn/IR checks
unchanged. Initial16/78/1 failed runs retained separately from actual15/77/0.
Initial main779/781 failures and conditional-root intermediate failure preserved;
blockless LLVM test now explicitly constructs its same invalid Graph view and
keeps original refusal/no-partial-module/neighbor checks. Real API image checks
pass after late PLT classification restoration; no failed-version raw completeness
claim. CPU fourfixtures64roots+64boundaries,8192fields/8388608memoryB; controls
result/memory/AF/target128each (reused CPU evidence, no Dragon). Final v4 native
1024complete roots (main768/NZ256),
65536fields/67108864memoryB; result/memory/edge64each. Old native256also match,
so this is compatibility, not newly successful root returns. All normal semantic
diff/incomplete/exclusion/skip/normalization0. Independent raw review covers
1216newnormal+control rows,88programs/176IR bindings,77824fields/79691776B
without new execution; actual1216native traces=V2Ea/Fb256+standalone960,
11frontends finalpin. CPU128 and old256 are retained references, not freshv4;
prior v3 matrix/audit1472rows stays separately archived. Runtime mainEa77/Fb,
NZFe619/C514; exact10source pins under timing-regressions-v4. Independent
native/unit reports are native-independent-review-v4, dual verificationd29e996f.

V3 corpus16pairs/32743checks/273raw is partial and retained; it cannot count
toward final v4. Implementation commit43ca7e6 records an early10/69 snapshot.
Final v4 has69/69 sealed inputs and138strategy pairs. Each side records205018
valid-function checks; the new side freshly verifies1671V2 raw modules,0optimized.
Baseline83's69Graph records/138canonical streams are reused, not fresh old
executions;138new streams are generated and all276preserved. No new-success reuse.
Five changed inputs/8canonical strategy pairs have exact root reviews:5instruction rows
restore existing entry flags,15rows remove empty metadata (6blocks/6null IR/
3invalid functions). No decoded bytes/decode properties/targets/callees/members
or valid-entry changes.
Full69 alerts missingIGNIL6->0/invalidFn3->0; undecoded26 and objdump146
remain. Actual unlifted markers231798 unchanged. Three combined IGNIL/unlifted
digests differ because null kind2 rows were removed; false flags are retained,
not normalized into equality. These are metadata repairs, not new lift coverage.
The independent retained-metadata audit and27-field typed alignment pass; they
do not rerun canonical/IR/CPU execution or prove whole-corpus semantics.

Input61 corpus2-sys_python3 attempt001 failed kernel-confirmed global OOM:
opt-21 invoked OOM and saved worker PID1582195 was killed(-9). Original source,
plan/chunk128/budgets remain unchanged. Its77linear+29recursive partial receipts
do not count toward1671successful modules. The user rebooted with8GB configured
RAM (guest7757115392B,boot4c497468-9091-4f9c-9dc5-241ad6d127bb);
the prepared8GiB swap script was not executed. The unverified120027374B tmpfs
orphan disappeared before capture: no bytes or contentSHA were saved. Explicit
loss exception17b2a57 records this one failed artifact, not a semantic exclusion
or permission for future loss. Surviving336files plus old.json/logs/dependencies
are actually preserved and dual-member verified under receipt94085dcb; all
surviving001files remain, and old.json's pre-retry bytes stay in the archive.
Attempt002 then reran both strategies completely;
no failed partial modules were reused. Final recovery accounting is70newworker
starts,69successful inputs,1OOM failure and1retry. Earlier60-input/pending and
pre-reboot orphan-preserved observations remain historical, not current state.
See v4/resource-incident-input61-v1/ and failed-input61-preservation-v1/.
Old side reuses frozen previous new83 Graph
inventories/138 canonical streams, not old401 or fresh old workers/LLVM. New
side runs every input and all valid raw modules afresh; no new-success reuse.
Fresh cxx_i386 O2 keeps13 encoded E9-to-zero edges per strategy while removing
empty zero registration; root checked all four retained streams and rel32s.
Crypto removes only4empty starts/1invalidFn; root checked full old/newlinear
streams retain four raw references, ELF section mappings and01010101<unknown>.
Its earlier targeted helper does not measure crypto edges. Full69counts above
come from the original producer and independent typed final audit.
Artifacts: `~/vmc-runs/cfg-entry-audit-20261109/` plus final
`~/vmc-runs/decoded-entry-registration-v4-20261109/`; old v3 partials remain in
`~/vmc-runs/decoded-entry-registration-20261109/`. Only owned overlap worktree;
original dirty113 preserved. This is not whole-program semantic proof or a new
heap/deflat capability. Storage cleanup converted only29952 historical snapshot
files in seven groups into a dual-verified recoverable content-addressed tar;
net26.06GiB released, restore tools/receipts in storage-cleanup-20261109.
Sources/libraries/objects/exes/logs untouched; current six test exes remain on VM.

Finite write footprints and snapshot bounds (2026-11-09, baseline `401ece6`):
see `docs/finite-write-footprints.md`. Both proveLeafCalls settings use precise
weak-update footprints for complete finite destination sets. Only singleton
destinations install values; multiple candidates invalidate their whole union,
retaining disjoint cells/bytes. Wider data remains unknown but its exact byte
extent participates in alias checks. Width is uint8_t: actual tests reach255,
never claim256 or round255 to wrapped0. Unknown address/zero width/different
unconstrained bases retain barriers. Modular interval overlap replaces byte
Cartesian intersection; extra candidate/wide work is budgeted. STORE now checks
fact growth before continuing, including terminal query-free blocks.

Private per-block snapshot DAG proves equal-width1..64 `(x|mask)-x=(~x)&mask`.
COPY/register intermediates preserve identities; reaching Temp definitions,
independent LOAD/UNDEF, partial writes, parent clearing and CALL are distinct.
No equation of abstract Unknowns, image-memory assumption or runtime Z3.
Per-run immutable-block plans/values count as facts; scratch min(remaining,
65536) and8192nodes cap construction. All scans/results spend; failed work is
not refunded and no partial plan is published. Existing finite bounds intersect
the tautology domain. Defaults1M/value64/block4096/fact262144 unchanged. No
public API/opcode/ABI/lifter/LLVM optimization or parser-policy change.

New17cases/5698assert pass in both builds. Old actual main17/5698 fails11cases/
2566assert; old NZ16/5662 fails10/2554 (terminalSTORE budget was only run on
old main,12/36 failures). Explicit v3 loader traces pin729/b743 runtime; earlier
v1/v2 only SHA/ldd evidence is preserved, not retroactively called actual maps.
Initial singular-tag filter ran only7cases and remains partial. Fresh CTest
main772/772 and NZ640/640 pass, not an aggregate from earlier phases.
Independent math:128UNSAT/8SAT/0unknown, width1..64;1396736 circular byteset
checks0diff. This is formula validation, not C++/whole-program proof.

CPU5fixtures/80complete roots,5728fields/5242880memory bytes; result80/memory80/
DF64/NF16/edge80 controls. Zero exclusions/skips/normalizations. Per config
readonly40 reports (20positive Finite1100,20aliasUnknown); old401 positives
Unknown. No observed target becomes a hint. Native x86 fourfixtures/s0-s1/
O0-O2/16inputs:main V2-static-product768+NZproduct256=1024root matches,
65536fields/67108864memory bytes. Controls result/memory/edge64each; edge64
semantic matches/64missing with empty publication/unresolved64, not old
63-of-64 target semantics. A64 STRQ0 is CPU/API only: x86-only recovery Pass
unchanged. Old native256 incomplete Transfer/rc1 preserved; pilot4 separate.

Real TG conditional queries:each config16complete/160Finite32Unknown,
ops700828..921926; unchanged frozen constraints/12queries/defaultGraph fp.
Disjoint8reports each12F0U; alias8 each8F4U. x86 vector stores were already
two64-bit STOREs; actual loss was100d4's `(x|7)-x` index and weak-address barrier.
Original8 CPU prefixes reused; no new TG CPU/megaInit/heap snapshot or complete
kernel/automatic-deflat claim. Stage `finite-write-footprints-20261109` uses
only owned `~/dragon-tales-overlap`; main/NZ actual binary snapshots are separate
and each tested config's3executables were precisely offloaded, not objects.
Fresh69 corpus replay completed: old/new main each205018 valid-function
checkpoints/138strategies, new1671 V2raw modules verify; old LLVM and optimized
corpus not rerun, no historical record reuse. All276 canonical streams sealed;
decode/target/callee/member/entry/strict deltas0. Both unlifted totals231798;
existing warnings6missingIGNIL/26undecodedEdges/3invalidFn/0emptySymbol/
146objdump retained, not new successes. No worker/resource failures or analysis
retries. This is structure/IR validation, not whole-program semantic proof.
Deployment output collision and a temporary status-aggregation KeyError were
metadata-only; the latter has an after-the-fact note, not a retained original
heredoc/traceback. Original producer/worker/plan bytes remained unchanged.

Finite-analysis cost (2026-11-08/09, baseline `a9f916d`): exact XOR-set
acceleration and unused report replay removal; see
`docs/finite-analysis-cost.md`. Both proveLeafCalls settings use the cost changes.
Literal direct-JMP reports avoid a full block replay only without ordinary
end-state value queries; indirect targets and actual LOAD prefixes still execute.
Complete bit cubes are proved from real sorted unique members. A second exact
path projects/deduplicates to the destination width and builds a joint GF(2)
span of dimension<=6. Strict |A|+|B|>|H| proves the full translated image;
otherwise 64-bit membership/XOR-index permutation computes the actual image.
No input holes are filled by assumption. Scans/elimination/enumeration cost
operations, failed attempts keep their cost, and all global budget failures
still discard every value. No new persistent cache, public API, opcode or ABI change.
Default1M/value64/block4096/fact262144 limits remain. Default false can change
budget outcomes, so old default operation-count identity is not a contract.

Old matching a9 SDK:7cases/10183assertions,3fail; same new runtime7/10334pass.
New span regression on intermediate old0ba:3/1089 with1fail, final same exe
3/1098pass in both builds. Cost group10/11432 and old calls/memory29/2234 pass
in both. Formal unique sets are main755/noZ3623, assembled from actual main754,
NZ621passed+one repaired mirror retry, and one newly registered pure check
each; never claim a fresh single full755/623 execution. Prior parser false
positive and disk-guard partials remain historical failures.

Tigress frozen-prefix queries, unchanged constraints/12queries/1M budget:
each config16/16complete,0cap (old4complete/12cap),96Finite/96Unknown;
operations699538..917103. All Graph fingerprints stay equal. These are
conditional read-only reports, not complete Guest/kernel deobfuscation.
Independent references:128full-state roots,3968scalar returns,30224additional
scalar returns; scopes remain separate. Native-v2 normal2560root matches,
163840fields/167772160memory bytes; result/memory controls256each and edge
control4omissions with256semantic matches. Zero exclusions/skips/normalizations.

Validation corrections are isolated fixes: V2 Python mirror now selects its
configured shim (old missing ENV could mix main shim/no-Z3 backend). A blanket
unreachable-text guard falsely rejected128 O2 returns; only rigorously proved
dead switch defaults now clear that IR check. Raw and selected IR are checked,
all execution comparisons remain. Helper22pure tests and independent152 Z3
queries (18UNSAT/134SAT/0unknown) validate actual certificates/negative controls.
See `docs/llvm-unreachable-validation.md`; original failed native group remains
forensic evidence, never a success. Final snapshot01e2f1... has310 actual
members/4SO/6exe/SDK37/445object metadata; all6 test exes were later precisely
offloaded under separate receipts, not .o restoration or timestamp tricks.
Only owned VM `~/dragon-tales-overlap`; stage `call-analysis-cost-20261108`.
Fresh two-corpus replay:69 inputs/138 strategies, each old/new library205018
valid-function checkpoints; all1671 new V2 raw modules verify, optimize0.
Old LLVM was not rerun; no prior raw result was reused. All276 canonical
streams are sealed; decode/branches/callees/blocks/functions/members/entry
deltas are zero. Input/source/library SHA rechecks match. Unlifted records
remain231798 on each side; existing6 missing-IGNIL/26 undecoded-edge/3 invalid
function/146 linear-objdump warnings retain their exact details. Largest
actual verified IR362429756B. Independent retained-metadata forward audit
checks module coverage and seal chains without rescanning large raw archives.
This is default structural/IR coverage, not whole-program CPU semantics.
Actual test context is a9+frozen patches; final Git/Mac/VM code289 and SDK37
map byte-for-byte. Docs/publication do not imply a fresh post-rebase build.

Caller-memory inputs and determinate CFG loops (2026-11-08, baseline `90da1f8`):
see `docs/call-memory-loops.md`. Existing opt-in proved-call effects now pass
proved memory byte constants/full SP-relative pointers, within the callee-SP
1MiB window, to root/nested callees. Fresh machine-CALL return slots are never
seeded. Missing bytes, different spaces and truncated pointers stay unknown;
i386 high numeric memory must not wrap into low addresses. Both cache keys
include complete canonical memory. Unknown LOAD precision can retry an already
proved universal effect; failed specialization retains safe universal effects,
with actual numeric decoded-code write footprints rechecked before applying.

Exact CFG interpretation follows only completely known one-bit CJMP conditions
whose machine targets and Graph successors match. Iterations do not merge;
real RET/restored SP and all write checks are required. Unknown/failing exact
paths discard their state and restart the original all-successor fixed point;
both attempts share the4096 operation cap. No sampled prefix publication,
runtime snapshots, API-name models, generic symbolic-count loops or REP support.
Only actual written bytes can export final memory constants. All input/cache
ownership and construction remain budgeted; default false/public ABI unchanged.

Also fixes numeric SP restoration: old summaries installed callee entry SP into
caller (c7fc/c7f8 instead of CPU c800). Actual machine-SP bytes normalize only
after restoration proof; i386 upper parent effects remain. CALL cache growth
now includes live SSA temps in the global fact check. Formal budget regression
replaces existing IR in place and asserts25ops/CALL23; same exe old18assert/
4fail vs new18pass. Old budget v1/v2 emplace-injection failures are withdrawn
as product evidence, preserved with an incident record; do not count them.

Old matching90da SDK compiles nine memory cases:5fail/4pass,5/142 assertions
fail; fatal REQUIRE limits actual old variant coverage. Separate safety/UC
three-case run:15/89 fail (12fixed-code gate,2wrong i386 direct SP,1nested
Unknown fatal); all4 UC scalar SP/PC/code controls pass. x64 old wrong SP is
also confirmed by independent old API/UC probes, not all old permanent variants.
Before self-XOR completion, main/no-Z3 each pass27 related cases/2112 assertions
(13new/1018), CTest742/742 and610/610. Those libraries and earlier mainv1/v2
logs are pre-selfxor evidence, not final stage coverage. Same-snapshot XOR
is proved only for matching register bytes or a defined equal-width Temp;
different Unknown values must not compare as identities. Caller use of this
rule is gated by prove_leaf_calls, preserving default finite-mode behavior.
Final selfxor main/no-Z3 each pass29 related cases/2234 assertions (15new/1140),
CTest744/744 and612/612. Old90da same public identity TU compiles:2cases,
112assert/10fail (8caller/helper sameReg,2defined sameTemp capability gaps);
different Unknown/missingTemp/mixedwidth/defaultoff controls pass. Two separate
selfxor machine fixtures have16 full CPU boundary-input matches; their native
default random-memory profile is different and recorded separately. Direct
previous635ec961 product64 stays incomplete;4pilot separate, do not mix its
baseline identity with the original22/90da704 matrix.
Final native:24fixtures/two strategies/O0-O2/eight inputs, main V2/static/
product2304 +no-Z3 product768 =3072 complete root matches; all52/76 modelled
fields/64KiB/PC/SP/seven Exit fields, zero incomplete/differences/infra/
exclusions/skips/normalizations. Result/memory/empty-singleton-edge native
controls each detect48/48; edge control raw execution still matches. These
144controls, old90da704/pre63564 incomplete and duplicate4+4pilots are separate.
Final each configuration has44 loop+4selfxor exact read-only API targets,
unchanged Graph fingerprints. Real conditional TG each gives24Finite/168Unknown:
1004f24F/72U,1011b96U;4/16reports complete,12 hit unchanged1M operations cap.
All report global-site counters are1proven/1rejected; pre635 was0/2 and all16
complete/192Unknown. This is limited conditional-value gain plus significant
resource cost, not default/full TG recovery. Real memset retained; no megaInit
or injected heap snapshot. Cache-context relevance/caller-loop growth remain.
Final22groups all sealed/cleaned separately under /tmp UID1000 usrquota guards
(not df alone), with>=64MiB quota reserve. One pre-selfxor static-s1 EDQUOT
partial and sidecar integration errors are preserved, never final successes.
The22 i386/x64 machine fixtures have176 independent full CPU returns, all52/76
modelled fields plus64KiB/PC/SP/exits; result/memory/DF/emptyedge controls each
detect176/176 and wrong snapshot-overlap targets16. Old same prove=true product
matrix704 remains explicit incomplete;4 zero-loop pilot measurements separate.
No fresh69-input or single-instruction campaign, heap/deflat or whole-program
semantic claim. Artifacts: `~/vmc-runs/call-memory-loops-20261108/` and matching
Mac validation-artifacts directory; only owned `~/dragon-tales-overlap` used.

Caller-context stack effects (2026-11-08, baseline `5a57164`): see
`docs/caller-context-memory.md`. Existing opt-in `prove_leaf_calls` can retry a
failed universal x86 E8/C3 proof using caller-proved register byte constants,
full-width SP-relative pointers and an optional numeric entry-SP anchor.
Root/nested caches key target plus complete canonical context, not call site
alone; independent callee roots stay arbitrary. No callee-entry memory facts,
branch pruning, parameter ABI/name whitelist, public API or Guest ABI changes.
Numeric recognition must not relabel scalar arithmetic as stack identity.
Context construction/cache ownership obey global budgets; each proof attempt's
call tree has its own <=4096 operation cap, with universal and contextual work
both charged globally. Existing four-level/32-block/8192-fact/1MiB limits remain.
Return slots and known decoded-code overlap (including overlapping x86 paths)
reject proofs; this is a fixed-code safety gate, not SMC support.

Matching old5a SDK/DSO compiles/runs all 14 C++ cases: 8 fail / 6 pass,
27/563 assertions fail; seven positive capability gaps and one known-CALL-push
code-overwrite safety case, not 27 CPU output mismatches. New main and no-Z3
each pass 14 cases/1094 assertions; CTest 729/729 and 597/597 respectively.
Same six permanent i386/x64 caller fixtures, two strategies, O0/O2 and eight
inputs: main V2/static/product 576 complete root matches, no-Z3 product 192.
All modelled fields (52/76), 64KiB, PC/SP and exits, zero exclusions/skips/
normalizations/differences/incomplete. Old same prove=true product192 stays
explicit incomplete, never whole-function semantic success or CPU wrong.
Native result/memory/empty-singleton-edge controls each detect12/12; edge
control execution still matches. Separate frozen UC48 roots and8 TG prefixes
are reference inputs, not additional unique LLVM sample counts.
Main/no-Z3 API each gives12 exact Finite (old12 Unknown), unchanged Graph
fingerprints and9 rejected bad-report controls. Old/main/no-Z3 real TG each
gives192 Unknown cells with prove=true; real memset remains, no megaInit or
heap snapshot. No full Tigress recovery gain, generic memset-loop effects,
entry-memory transport, heap predicates or deflat claims. Default false path
unchanged; no fresh69-input corpus or single-instruction campaign claimed.
Cache-field threat audit is source reasoning, not injected bad variants;
pointer-only/entrySP-only cache-key omissions lack isolated collision tests.
Artifacts: `~/vmc-runs/caller-context-memory-20261108/` and matching Mac
validation-artifacts directory. Only owned `~/dragon-tales-overlap` is used;
original dirty checkout preserved.

Conditional entry-state finite queries (2026-11-08, baseline `9993d04`): see
`docs/finite-initial-state.md`. New C++/C/Python read-only APIs accept owned
register/memory facts at already-admitted roots. They constrain initial bytes,
not machine writes: EAX facts never invent zero RAX upper bits. Backedges merge
with entry states; undeclared roots/callees stay arbitrary. Existing alias,
CALL/unlifted/local-flow barriers and opt-in proved-call effects remain.
Results carry conditional=true, bounded owned entry_states and
assumptions_valid (structural/initialization-budget validity, not truth).
No Graph targets, LLVM assumptions or pipeline caches receive these facts;
default analyses and old result layouts/JSON have no new assumption fields.
Initialization descriptors/bytes are charged to operations/facts and capped at
262144. Later analysis exhaustion can retain valid owned assumptions while
making the entire analysis incomplete/Unknown.

Also fixes existing finite register read/write range wrap: after clobbering the
default symbolic SP, Reg{0xfffffff9,64} could read a shifted constant from
offset 0x1ffffff8. A uint32 exclusive-end check now rejects it as Unknown.
The permanent old-API regression compiles/runs with matching 9993 SDK/DSO:
1 case/24 assertions, 9 failures; legal high-offset controls pass. Writer guards
reject overflowing actual/implicit-parent extents and invalidate registers;
legal partial/wide writes retain their effects. Its old-API case gives 35/92
failed assertions (28 range controls +7 separately labelled zero-width invalid
IR controls), not 35 hardware field differences. New-API 19-case tests cannot
compile old SDK, and compile with the new header.
Main CTest 715/715, no-Z3 583/583; each build 21 related C++ cases/896 assertions,
plus 3 C cases/66 assertions and Python API checks.

Final main default Graph campaign: 69 inputs/138 fresh independent processes,
205018 Graph-valid checkpoints of 205021 function candidates. All 23659048
canonical rows/6722719390 uncompressed bytes exactly match retained 9993;
17160761 instruction and 3146633 block queries, including unchanged old alerts.
28 driver controls and full forward audit pass. No new objdump or whole-corpus
LLVM generation/execution; prior 1671 raw modules are not new coverage.
This is Graph/IGNIL structural compatibility, not whole-program semantic proof.

Each final configuration freshly runs 86 API calls/166 cell results:
24 Finite/142 Unknown, zero observed-value omissions and unchanged before/after
Graph/IGNIL/edge fingerprints. Both reference the same 47 CPU root returns
(45 small fixtures +2 CALL-slot cases) and 8 Tigress LOAD prefixes, not duplicate
CPU executions. Only the 2 CALL-slot references check all 46 architecture fields
and 64 KiB; these are CPU references, not new LLVM full-function comparisons.
Old 9993 independently gives 30 unseeded Unknown queries, not errors. All 96 TG
cell results remain Unknown; no default recovery gain. The 8 TG prefixes start
at kernel, do not execute wrapper/megaInit, leave SBOX at ELF.bss zero, stop
before reading it, and keep the actual kernel memset. No heap snapshot injected.
The reference uses writable flat 1 MiB Guest memory, not Linux segment protection.
V1 oracle path-expectation failure, pre-guard libraries and separate 22/56
partial corpus records stay separate from final evidence. Artifacts: VM
`~/vmc-runs/entry-state-memory-20261108/` and matching Mac validation-artifacts
directory. Use only owned `~/dragon-tales-overlap`; original dirty tree preserved.

Table contents at the actual LOAD (2026-11-07, baseline `777712f`): see
`docs/table-content-proof.md`. x86/A64 table publication needs the existing
bound/shape proof AND complete current contents from admitted Guest STOREs or
explicit trusted `AnalysisHints::readKnownMemory`. Image bytes, ordinary
`readMemory` and section permissions are not immutable-memory certificates.
The callback asserts exact bytes at this LOAD on ALL admitted executions/visits.

Raw analysis, LoadedImage and FiniteDispatch route the same hints contract.
Every decoded direct callee remains an independent input, even with explicit
roots; a tail-path initializer cannot constrain arbitrary CALL entry. C++
`finiteMemoryValues` queries fixed cells before a real same-width LOAD, with
alias/CALL/unlifted/local-control barriers and no image/heap seed. This stage
does not implement general initialization, heap modelling or de-flattening.

Reanalysis withdraws inferred edges globally but rebuilds only the supplied
region; other regions' old targets do not recover automatically. Explicit
caller/decoder edges remain. Graph.merge retracts inference from both graphs,
unions explicit evidence on compatible same-byte/control-category collisions,
and retains first-wins for incompatible collisions. Incomplete may remove old
inference and report changed=true. Memory proofs share per-round operation/fact
budgets and caches; Unknown/budget failure never retries unbudgeted defaults.
x86 expansion cap exhaustion aborts the whole tentative transaction. The trusted
resolver can only lower the built-in 1<<20 cap; default-cap exhaustion was not
run, and whole-block predecode makes this no hard CPU/memory limit.

Python finite registration retains independent callback bridges, errors and
entries; replacing/closing source hints is safe. Override/close releases old
snapshots and run finally clears internal error references. C known-reader
results: 1=fact, 0/other positive=Unknown, negative=ERROR and stop later passes.
C callback/userdata remain borrowed for the synchronous call or registered
executor lifetime. Active Python loader/disassembler close is rejected; hints
may still close/change. Returned Graphs never borrow readers. Public C++ layouts
changed: REBUILD clients with matching SDK headers/libraries. Existing opaque
C symbols and Guest ABI remain; new hint-routing symbols require the new shim.

Tested production v8 is frozen. CTest 690/690, no-Z3 558/558; component 50 cases/
3936 assertions per build. Guard truth: 27 CFGs/20736 Unicorn runs/0 wrong sets.
A64 region ownership and separate ARM32/A64 classifier statics are fixed; fresh
process tests are Graph classification, not ARM32 execution. Old-library failures,
oracle mode corrections and three OOM recoveries remain in the component report.

Final native audit: 1420 normal exact matches = 1104 complete root returns and
316 precise boundaries; 340 single-result-field/byte-memory controls detected.
Every run compares seven Exit fields, PC/SP, modelled fields and all 64 KiB;
zero exclusions/skips/normalizations, empty Graph unlifted reports. The separate
220 CPU/Graph checks are not 220 native complete functions or new diagnostic inputs.

Both corpora: 69 inputs/138 strategies per version. Old `777712f` has 205406 valid
candidate checkpoints, Graph-only; new has 205018 and 1671 fresh V2 raw modules.
No old LLVM rerun or prior-stage raw reuse. New workers: original first 60,
lifecycle last 9, with all nine cleanup receipts. Independent forward/precision
audits pass, not semantic or Graph compatibility. Candidates removed 388/added 0
are not proven losses of real functions. Retained indirect sites becoming
unresolved: 2957 (input,PC,strategy), 1524 (input,PC) over 54 inputs after folding
strategy, with 42063 target memberships withdrawn. These are observations, not
globally unique functions/programs. Retained instruction byte changes and direct
branch/callee target losses are 0/0/0; source-disappeared edges are separate. Unlifted
233134->231798 is coverage loss, not more supported instructions.

Default Tigress six switch-type ELFs: C 288 matches, LLVM 0 complete/288 incomplete,
all six original rc1 retained, CFG wrong/unlifted 0; executed-undecoded 6857 address
observations, not global-unique addresses or errors. DEFAULT STATIC IR HAS NO
COMPLETE RETURN COMPARISON ON THESE SIX ELFs. Historical 223b 288 complete is
historical measurement only, not fresh old LLVM replay. Wrong=0 or smaller IR
is not deobfuscation gain. Independent aggregate audit passes metadata; semantic
and compatibility completion remain false. Next gap: provable initialization/
readonly-table memory contents with entry/alias guarantees, not automatic flags.

Artifacts: `~/vmc-runs/table-content-20261107/` and Mac
`/Users/c/Desktop/deob/validation-artifacts/table-content-20261107/`; historical
candidate/failure manifests remain distinct. Storage restore chains are in
`storage-migration-artifact-index.json`. All 402 generated object-cache files
were dual-archived; reuse only with matching source/toolchain pins, otherwise let
Ninja rebuild. Current libraries/test executables/source/build graphs stay intact;
storage operations add zero semantic tests. Only use owned
`~/dragon-tales-overlap`; one heavy VM worker.

Legacy whole-function unlifted refusal (2026-11-07, baseline `a19af98`): see
`docs/llvm-legacy-unlifted.md`. Legacy `lift(Function,Graph)` now rejects a
marker on any member block before LLVM/module mutation, including zero-op
unsupported STOREs and non-entry/empty-IR markers. C++/Python return false,
C returns0; callers must consume that result. Supported legal UNDEF and NOOPs
are not markers. Same-instance failures preserve existing/same-address
definitions and allow later valid lifts. V2/Static retain explicit Unsupported
instruction exits; trace refusal was already present. No ABI/default-mode/
Graph/IGNIL/slicer/finite-analysis/deobfuscation algorithm changes. Manual06/07
and C API examples now check acceptance; old partial IR is historical error
evidence, not a supported “continue anyway” flow. fuzz_loader's ignored return
still counts robustness attempts, not lift success; do not silently rewrite it.

Old same-source tests fail: C++7 cases/426 assertions, four cases/70 assertions;
C1/18 with four assertion failures; Python four methods/16 failures. Preconditions,
supported UNDEF and V2/Static acceptance controls pass old too. Final main
CTest653/653 and no-Z3 521/521 pass; final C++7/426, C1/18 and Python four
methods pass. Frozen harnesses pass a final targeted2/2 rerun: inventory rejects
11 bad reports/accepts one valid report; native has nine contract checks.

Independent native-final-audit closes22 jobs. Old Legacy128 CPU observations =
64 actual MOVBE stale EAX/RAX + four DATA-byte counterexamples /64 supported
matches; old non-Legacy three modes add384 matches as compatibility controls.
New Legacy16 module contracts = eight atomic marker refusals/eight accepted;
64 successful pre-RET ABI observations compare SP/all other modelled fields+
64KiB, with PC outside Legacy ABI and no RET-pop normalization. Refusals are
module counts, never per-trial CPU matches. New main V2/static-resumable/
product-FunctionStart each128 =64 roots+64 Unsupported; no-Z3 product adds128.
Thus new full-Guest512 =256 complete roots+256 exact instruction boundaries,
plus Legacy64 yields576 normal matches with different observation scopes.
Result/memory controls each96/96 detect exact single-field differences; source
native snapshots remain unchanged. Never describe Legacy as full Guest roots
or boundary matches as complete functions.

Non-Legacy LLVM88 files:64 raw.ll/unoptimized.ll files byte-identical;24 optimized.ll
differ only in first-line ModuleID comments with the two exact old/main input
paths, every remaining byte equal. Original exact-audit failure and native-run
completeFalse remain; independent final audit closes this artifact-only
distinction. Not88 byte-identical files and no Guest-state normalization.

Acceptance inventory: nine binaries/18 graph views per version;6245 candidate
records =6237 valid+eight invalid. Common accepted755 have identical IR hashes;
67 new marker refusals,5423 existing refusals including eight invalid;822 old
accepted becomes755. Do not attribute all existing refusal causes to this guard.
Independent manifest/schema/exact entry coverage/source SHA/full-path two-DSO
maps are checked;11 bad-report controls reject. Graph skeleton/acceptance/IR
identity only, not full Graph fingerprints, verify or CPU semantics. No new
69-input V2 or Tigress CPU matrix rerun: only Legacy entry refusal changed,
with no upstream semantics edits. Prior-stage counts are not new coverage.
Writable-image-table provenance/alias certificates, TLS-withdrawn targets,
cross-call pointer/heap modelling and deflat remain separate open work. Stage:
`~/vmc-runs/legacy-unlifted-20261107/`, Mac
`/Users/c/Desktop/deob/validation-artifacts/legacy-unlifted-20261107/`.
Keep old failures/initial audit records; execution validation is complete and
archive/Git finalization is recorded externally. One heavy worker; only owned
`~/dragon-tales-overlap`.

x86 effective-address width (2026-11-07, baseline `72c37b8`): see
`docs/x86-effective-address-width.md`. Current candidate-v1 main `223b1239…` /
no-Z3 `c408fe2f…` has completed component/native/reference, fresh-corpus and
limited Tigress validation. Git finalization is tracked by the external receipt.
Private AddressContext carries effective 16/32/64-bit width separately from 32/64-bit
linear Guest pointers, data widths and implicit stack widths. Wrap base +
scaled index + displacement first, then zero-extend once; subsequent vector,
x87 and string data bytes span linearly. Strings update indices/count at
effective width, retaining REP-zero and read-before-write. EIP aliases RIP
privately; pin each complete instruction's next-PC before address arithmetic.
Moffs is Imm + Segment, not a five-tuple: validate it separately in the new
address-register gate. Only NoRegister/EIZ/RIZ mean zero; unknown real base/
index stays unlifted. BT register bitstrings retain signed
max(indexWidth,EAWidth) through word selection before truncating byte delta.
POP reads old full SP, increments by data slot, then computes destination EA;
PUSH reads its source before SP moves. Unsupported LEA16/word PUSH forms
remain unlifted. No public ABI/Guest/opcode/Graph-strategy changes; segment
guards stay in force.

Main CTest 642/642, no Z3 510/510; component 10 cases / 10087 assertions pass,
old same tests fail 75 assertions in 8/10 cases. No-Z3 component 3/212 passes.
Sparse independent UC-vs-IGNIL/Z3: 76 trace configurations (x64 51 / i386 25),
old 35 different / 41 match, new 76 match; these are not 76 LLVM functions or
JIT roots. Native main: 26 fixtures × O0/O2 × 8 × six mode/strategy groups =
2496 complete full-state roots. No-Z3 product-s0 only adds 416, not six no-Z3
groups. All 76/52 modelled fields, PC/SP, 64 KiB and exact RootReturn compare;
312 + 52 root Graph reports verify. Main ldd 324 = 312 matrix executables +
eight controls + four old-extra executables; no-Z3 52 and old24 48 are separate.
Result/memory controls: 32 each, exact one-bit/one-byte errors; zero new native exclusions/
skips/normalizations. Old 416 comparisons are 128 root matches / 32 completed-root
RAX differences / 256 MemoryBounds boundary differences. Old 24-fixture 384
records were freshly executed on 7223 in this stage, frozen, then combined
with extra two-fixture 32 fresh old records for audit; no prior-stage native
cache. They are never new passes or 288 completed-root semantic failures.

reference-audit.json (`5c3f4567…bc5322`) retains nine execution receipts/maps
and all blind spots: single-x64 986 ok are templates (TRIALS=8; actual compare/
fault totals unreported, not 7888 full matches); SSE 393 × fixed 60 = 23580
reference trials, 700 unknown NaN fields. IGNIL raw/opt i386 each: 305 compared /
15 faults, 22 outside writes un-compared; x64 each: 298/22, 37 high-memory and
23 outside writes un-compared. Legacy i386 current/old each: 24 blocks retain
two UC-fault skips, one unlifted skip and one DIFFERENT with byte-identical
stdout. x64: 24 blocks, four high-address + one fault skips, zero DIFFERENT.
The i386 0x43319d FXAM unnormal result is the prior Intel + Rosetta-confirmed
Unicorn reference defect, with exact-seed state/bytes reconstructed; keep the
raw difference, no new normalization. Block scripts omit unknown fields and
limit memory coverage; rc0 is not full-state/whole-program compatibility.

Corpus execution completed: baseline/new each 69 fresh Graph inputs / 138
strategies; new raw V2 verifies 205406 valid-function checkpoints / 1675 modules
(chunk 128, LLVM_OPTIMIZE=0). Old LLVM not rerun, prior-stage raw reuse zero,
no full optimized/static corpus. Old/new markers both 233134; warning counters
6 missingIGNIL / 26 undecoded-edge / 3 invalid / 0 empty-symbol / 146 objdump
missing unchanged. Driver records 60 changed canonical strategy graphs / 3440
kind-2 block records. Independent corpus-forward-audit.json (SHA256
f1c57a8082fbefe75d954809f061a68f3ad1a33e4d8c34422d938ef7332da05e) verifies
255 source identities and complete coverage/definition sets: 3796 PC-level
changes are strictly same-width string-pointer COPY insertion + temp renumbering,
zero hardware/CFG/target/member/unlifted/warning changes or unexplained rewrites.
All 14 TLS observations remain unresolved with unchanged instruction records:
seven input-local PC sites / four Lua ELFs / eight strategy graphs, not recovery.
Deleted full canonical streams/raw IR were not independently recomputed or
reverified by the audit; retained payload/count/provenance checks are not CPU
semantic proof or general compatibility.

Retain resource history: one 512 MiB disk-guard stop before worker22, one
normal resume refusal of its empty001 (reviewed, then explicit --retry-failed),
and input66 worker SIGKILL(-9) coinciding with a kernel Python OOM event.
The frozen worker did not save its child PID: do not claim a saved-PID identity
proof. Preserve partial001, journal and storage/recovery receipts. Its
362429756-byte IR verifies standalone and matches retry recursive module8's
hash/size, but is not an extra successful module or whole-input pass. Retain 22
verified partial modules (14 linear / eight recursive) separately from unique
1675 coverage. Attempts were hash-verified through tmpfs/disk relocations,
logical/physical keys retained via symlinks; final disk TMPDIR recovery used
the same v1 worker/plan1db3302a…ee3a08/checker/chunk/budgets/guard. No observer
v2 or relaxed checks; all 62 remaining real opt module paths use disk TMPDIR.
The first 65 completed same-stage records were rehashed
on resume, not prior-stage LLVM cache. IR validity is not CPU semantics.

Limited frozen Tigress smoke completed: six ELFs × two strategies × 24 inputs ×
one product-O0 FunctionStart plan, 288 complete LLVM roots + 288 C matches;
real megaInit and full 1 MiB memory (64 KiB heap), no heap snapshot. Each of six
groups has outer rc0, independent 48-snapshot full-state/RootReturn/PC-SP/code
audit, gate, archive member SHA and owned-raw cleanup. CFG wrong, unlifted,
executed-undecoded and incomplete all zero. Ordinary goto/indirect and the
six-plan matrix were not rerun; old 960 incomplete is not this stage's count.
No deobfuscation-gain claim. Image-table
content/alias/immutability certificates and Legacy whole-function unlifted
rejection remain unimplemented; generic opaque snapshot prototype stays
withdrawn. No new heap/deflat/VM/binary rewriting. Artifacts:
`~/vmc-runs/x86-addressing-20261107/` and Mac
`/Users/c/Desktop/deob/validation-artifacts/x86-addressing-20261107/`.
Use only owned `~/dragon-tales-overlap`, one heavy VM worker; candidate-v1 and
prior-stage artifacts are immutable; all stage execution/audit records retain
their stated limits and resource history.

x86 segmented-memory hardening (2026-11-06/07, baseline `b95c84b`): see
`docs/x86-segmented-memory.md`. Production/tests/fixture `359df9a` are validated;
final Git state is recorded in the external receipt. Hardened DSOs are
7223b80f... (Z3) / aa127d9f... (no Z3); `fdf138c` is pre-hardening history.
Ordinary x64 FS/GS and x86-32 explicit segments use honest UNDEF + unlifted,
never an invented flat memory address. Inspect original MC before scratch
rewriting; moffs has its own layout. Clobber real defs/flags and both XMM halves;
keep unknown-target CALL/JMP skeletons. Unsupported mayLoad/mayStore covers
implicit MASKMOVDQU. PAUSE/MFENCE/LFENCE/SFENCE are no-ops in the fixed
single-thread Guest model. LEA, hints, implicit SS stack accesses, fixed-ES
STOS/SCAS and long-mode ignored CS/DS/ES/SS bases stay supported. String/x87
specialists remain; explicit x86-32 source-string segments are unknown too.
A narrow register-based addr32 LEA wrap is fixed; no general EA-width repair.
No segment bases/descriptors, IGNIL opcodes, public ABI or Guest fields added.
Pure unsupported STORE may have zero IR ops: consumers must check unlifted.
V2/Static stop before it; Legacy whole-function IR still does not uniformly
reject it (single-block trace does), so clients must reject incomplete IR.

Final CTest 632/632, no Z3 507/507; component 13 cases/1142 assertions,
no-Z3 13/1114. Same test source on b95 fails 348/1338 assertions in9/13 cases;
fdf fails8/1142 in the fence case. Independent segment oracle:390 unique
checks+PC plan,0 failures/skips/exclusions/normalizations;64/64 dropped-LOAD
controls and96/100 flattened controls detected, four ADD-zero controls agree
and are explicit. Forty-two/39 production objects verify; only X86.cpp.o
changes from fdf. All other lowering/production objects are identical.
Hardened native:17 fresh groups/2632 comparisons,896 complete roots+1568
explicit Unsupported boundaries; result/memory controls each80 exact field/byte
differences. Eight fdf hint rejections are separate old-version failure controls.
392 exe/module reports and78 snapshot files hash-verify;0 errors/exclusions/
skips/normalizations, all modelled fields+64KiB compared. Only b95 six groups
1920 are reused after memberSHA+240ldd checks:480 ordinary matches/96 LEA RAX
CPU differences/1344 boundary-contract differences. No fdf success reuse.

Final corpus:69 inputs/138 fresh Graph inventories. Fresh raw3 inputs/6
strategies/10467 valid-function checkpoints/86 modules; verified reuse66/132/
194939/1589, total205406/1675;0 lift/verify/coverage errors. LLVM_MODE=v2;
optimized_corpus_rerun=false. Provenance includes116 actual Git sources,
42/39 complete object sets and138 full delta compositions. Markers233134
(vsfdf233150:16 removed hints); canonical/hardware changes5/0 vsfdf,64/8 vsb95.
Warnings6 missingIGNIL/26 undecoded/3 invalid/0 emptySymbol/146 objdumpMissing
remain. This is IR validity/provenance, NOT semantic compatibility:14
(input,PC,strategy) target-withdrawal observations persist across4 Lua ELFs/
8 graphs,7(input,PC) sites, not14 independent programs/functions. Member-set
changes are also14 strategy records,7 input-local entries; compiled/stripped
variants are not independent source functions. Thirty CPU guard probes support
the old bounds. Do not hide the retained unresolved precision cost.
Final standard Tigress six fresh groups: C288/LLVM1728 complete matches,
0wrong/unlifted/executed-undecoded; twoELF×sixplans×24 per group,72 ldd records.
Ordinary four fresh groups: C256/LLVM576 complete+960 incomplete; GCC each
192/192, Clang each96/288. Retain rc1/semantic_complete=false and all partial
status2 reports;0wrong/unlifted/known-target omission. Retain492 unresolved
transfer and2538 undecoded-reference-hook-PC observations, deduped by group/case
to82 transfers/423 PCs; sixplans repeat them. Pass reports36SUCCESS/60INCOMPLETE.
Official Tigress audit SHAa251c81a...2fd74 verifies all10 fresh groups,168ldd,
9578 archived regular members (4,324,853,358 raw/3,782,666,624 compressed bytes).
validation_complete=true, semantic_complete=false. The six-plan1728 must not
be called recovery gain over historical two-product-plan576.

Known production gaps remain: ordinary addr32 memory can infer target1040
where CPU1060 (explicit guest stores, no CALL/image initial facts); Legacy
whole-function marker refusal; absolute/relative image tables can be modified
by ordinary STORE while old/new analyses still publish original1040/1050.
Fixed code or const readMemory buffers do not certify data immutability.
Final7223 tiny probes directly retain both table-write errors and the ordinary
addr32 wrong complete target1040 vsCPU1060; same-source LEA control is fixed4.
known-defects/known-gap-final has22 dual-end-hash-verified payloads and manifestSHAa4ec751f...
640aff, including observer-free equal-output controls and original failed/empty
measurement drafts; confirmed open gaps are not production-pass evidence.
Generic opaque post-event correlations are withdrawn, never built/merged:
independent table-content provenance/alias/immutability certificates must come
first. TestOpaqueMemoryVersions.cpp and its CMake line are absent from active
production; exact planned source SHA2ebe9652...946465c and patch4292cc3f...
366025 are archived with old combined-tag31/40 failed-assertion evidence.
These prototype numbers include fence/UD2 controls and are not current passes.
The old Lua i3860x43319d x87 sampling1 DIFFERENT is a verified Unicorn2.1.4
FXAM unnormal-reference defect: Intel SDM+standalone Rosetta support Dragon,
raw/Jflip/normal/zero controls and exact-seed replay are frozen. Keep the1 raw
difference, no new exclusion/normalization; not a pending Dragon x87 bug.

Artifacts: VM `~/vmc-runs/x86-segments-20261106/`, Mac
`/Users/c/Desktop/deob/validation-artifacts/x86-segments-20261106/`.
Keep original final/final-noz3 and raw-final as immutable fdf pre-hardening;
use hardened-final/hardened-final-noz3 and raw-hardened for final validation.
The final corpus metadata tar has599 payloads+one exact self descriptor and
219 directory inventory entries, all SHA-verified. Old static15-input raw/opt
supplement and SIGTERM partial16 are not whole-corpus optimized coverage.
Known defects/reference resolution and planned designs are separate artifacts,
not successes. Official stage-root entries: final-artifact-index.json,
final-validation-summary.json, freeze-final-validation.json; validation_complete
is true, semantic_complete/compatibility_complete are false. Git finalization is
tracked externally. Use only owned `~/dragon-tales-overlap`; one heavy VM worker.

Bounded call-tree and stack effects (2026-11-06, baseline `230de43`): see
`docs/stack-call-effects.md`. The opt-in legacy name `prove_leaf_calls` /
`FiniteValueLimits::proveLeafCalls` now proves restored frames, original register
byte identities, stack write footprints, bounded internal E8 calls, and constant
register/stack results agreed by every normal return. Each C3 must restore entry
SP; every store and translated child write must protect all enclosing return
slots. Include saved-register stores and nested CALL pushes in the footprint;
invalidate caller facts before installing proved final constants. No ABI saved
register lists, API names, image/heap snapshots or first-return assumptions.
At most four callee levels, 32 blocks per level, a shared 4096-check root budget,
per-level min(max_facts,8192), and stack writes within +/-1 MiB. Recursive cycles,
unknown calls/writes, indirect calls, unsupported/unlifted/local flow and exhausted
proof budgets retain the full barrier. Different return constants do not yet
form an interprocedural value set. Unknown parameter pointers/global/heap writes,
initialized memory, general alias proofs and de-flattening remain incomplete.
Proofs assume fixed code and a single-thread Guest model, not self-modifying/JIT
code, concurrent writes or asynchronous re-entry; IR verify does not cover those.
Public C/C++ structures, signatures and Guest ABI are unchanged from `230de43`;
default CALL barriers, old slicer, lifters and LLVM optimization order stay intact.
Independent review found two unsound cases in the pre-hardening prototype.
At that stage the general x86 emitAddress path ignored FS/GS segment bases without
an unlifted marker: helper `55 64 48 8b 2c 24 48 83 c4 08 c3`, FS_BASE=0x1000,
saved RBP=0x1040, FS slot=0x1060 actually returns RBP=0x1060, not the saved value.
The proof must reject explicit segment overrides in the real prefix region;
do not mistake immediate/displacement/opcode bytes for prefixes. The underlying
general-lifter segment-base semantics were an unresolved gap outside that
patch. The following x86 segmented-memory stage addresses ordinary root finite
analysis, legacy slicing and V2/Static LLVM entry boundaries by conservative unsupported
lifting; see its separate validation status above. It does not model segment
bases. The original callee-proof guard alone did not repair those other paths.
Also, RIP-relative helper `48 8b 05 00 00 00 00 c3` caused registerConstants to
resurrect callee PC=0x1087 after the RIP clobber. Never export RIP/EIP register constants
as call results. Three new safety cases/102 assertions fail 2 cases/40 assertions
before hardening (the prefix-payload control passes), then pass 3/3 and 102/102.
Three additional LEA-result/stack/nested observations keep RIP Unknown while
RAX=0x1040 matches Unicorn and dispatch targets remain recoverable; see
review-final-guard.py/json. These are targeted observations, not full-state runs.
Final hardened checks: 17 related C++ cases/594 assertions, including 12 new
stack/safety cases/368 assertions. Frozen old `230de43` rerun on those 12:
9 fail/3 controls pass, 47/316 failed assertions. CTest 619/619, no Z3 494/494;
Python and guard/register/memory/loop slice checks rerun successfully. New native
18-fixture matrix rerun: 1728 complete all-state/64 KiB root matches, zero
incomplete/full semantic differences/wrong targets; no-Z3 144 and legacy leaf
product 96 more matches. Result/memory controls each detect 72 single-field
differences; CFG control detects 44 omitted transfers including 16 empty target
sets while 72 semantic runs still match. Zero exclusions/skips/normalizations.
Old native 1728 incomplete records are reused after checking exact fixture and
frozen dual-library hashes, not rerun; distinguish this from the fresh old C++
run. Final real Tigress four groups rerun: 256 C matches, 576 complete LLVM
matches/960 incomplete, zero complete differences/unlifted/new pseudo-functions,
no complete-return gain. Final records are under final-validation/ with stage
validate-final.log; 15 final raw archives are separate from pre-hardening data.
Do not count the following pre-hardening mutations/pilots as final-library runs.
PRE-HARDENING ONLY: new nine C++ cases/266 assertions; 14 related cases/492 pass.
First three tests-only on old source: two fail/one control passes, 5/68 failed
assertions. Nine on frozen old implementation: seven fail/two controls
pass, 33/214 failed assertions. Staged nested-call/constant-output regressions
fail 4/8 assertions before those effects exist. Four omitted-check controls
(footprint, SP restoration, return-slot protection, first-return-only summary)
fail 4/2/14/8 assertions. Previously rejected valid saves/nested calls become
positive cases; unknown effects and damaged returns remain negative cases.
Native 18 permanent i386/x64 fixtures: new 1728 complete full-state root matches
vs old 1728 incomplete with the same opt-in flag; no-Z3 adds 144, existing leaf
product fixtures add 96. Static/product use FunctionStart; all modelled fields
and 64 KiB, zero exclusions/skips/normalizations. Result/memory controls each
detect 72 single-field differences; the strengthened CFG control detects 44
omitted transfers, including 16 empty target sets, while 72 semantic runs still
match. Pre-hardening CTest 616/616, no Z3 491/491; Python and guard/
register/memory/loop slice checks pass. Frozen original-helper Tigress matrix:
256 C matches, 576 complete LLVM matches/960 incomplete, zero complete
differences/unlifted/new pseudo-functions, no added complete-return gain.
No fresh 69-input default/raw/optimized IR corpus, single-instruction or IGNIL
optimizer campaign: those production paths are unchanged. Prior `230de43`
corpus records are existing coverage, not new optional-mode semantics. General
call/alias/heap behavior and whole-corpus optional-mode semantics remain unproved.
Small records: `~/vmc-runs/stack-calls-20261106/`; large frozen dual libraries,
interfaces and per-file checked raw archives: development Mac
`/Users/c/Desktop/deob/validation-artifacts/stack-calls-20261106/`. Copy those
Linux libraries back to the VM to replay. Use only `~/dragon-tales-overlap`;
do not touch the original dirty checkout or run concurrent heavy VM workers.

Proved leaf-call effects (2026-11-06, baseline `5c68ce7`): see
`docs/leaf-call-effects.md`. Opt-in `FiniteValueLimits::proveLeafCalls`, C
`dragon_graph_finite_values_with_calls`, Python `prove_leaf_calls=True`, and
finite-values/finite-dispatch integer option `prove_leaf_calls=1` derive effects
from actual current x86 code. No ABI/API-name/argument/return-value assumptions.
Only unprefixed E8 near calls to completely lifted no-STORE/no-SP-write leaf CFGs
with direct flow and ordinary C3 returns qualify. Check all paths, at most 32
blocks/4096 checks per callee within global budgets; reject unknown/nested/local/
unlifted/system effects. Retain untouched register bytes, account for 32-bit
parent clearing, and invalidate the machine CALL return slot. Unknown SP/alias
still invalidates memory; saved/restored stack frames do not qualify. Proof
caches are per immutable analysis and rebuilt on expansion. Default full CALL
barriers, original slicer, lifters, LLVM sweep and Guest ABI are unchanged.
C++ limits/result structs append fields: C++ clients must rebuild; old C ABI
stays available. Final per-analysis unique-site counters are diagnostic, not
deobfuscation completeness or evidence that a rolled-back graph was published.
Five C++ cases/242 assertions; first four tests-only on old source fail, final
five on frozen old implementation fail 5/20 assertions at the feature entry.
Each deliberately omitted return-slot/register-clobber control fails four
assertions, so unsupported-option failures are not the sole validation.
Native six permanent i386/x64 fixtures: 576 complete full-state root matches vs
old finite Pass 576 incomplete, plus no-Z3 48 matches. Static/product cases use
FunctionStart; all modelled fields/64 KiB, zero exclusions/skips/normalizations.
Result/memory controls each detect 24 single-field differences; edge omission
detects 12 missing transfers while 24 semantic runs still match. CTest 607/607,
no Z3 482/482; Python and guard/register/memory/loop slice checks pass.
Frozen Tigress original-helper GCC/Clang/two-strategy matrix: 256 C matches,
576 complete LLVM matches/960 incomplete, zero complete differences/unlifted;
no additional complete-return gain. O2 goto helper effects can be proved;
indirect stack frames, unknown-pointer stores and unproved image table loads
remain explicit obstacles. Both corpora: 69 inputs/138 strategies/205409
inventoried function entries (including 3 existing invalid candidates; the CET
valid-function count stays 205406); exact default Graph/IGNIL/function/unlifted
fingerprints against frozen CET libraries. This is compatibility, not a new
whole-corpus semantic or raw/optimized IR campaign. No new single-instruction,
IGNIL optimizer, heap-predicate, de-flattening or binary-rewrite claims.
Artifacts/frozen dual libraries/interfaces: `~/vmc-runs/leaf-calls-20261106/`;
8436 raw files preserved in a per-file verified archive. Only use the owned
`~/dragon-tales-overlap` worktree; the original dirty checkout stays untouched.

CET function boundaries (2026-11-06, baseline `70a1fab`): see
`docs/cet-function-boundaries.md`. A bare x64 ENDBR is not independent function
evidence; keep explicit roots/callees, full frame prologues and existing post-RET
leaf discovery. Do not force every landing pad into a block: that breaks the
post-RET evidence rule (intermediate control fails 2 assertions). No API-name
noreturn/returns_twice models or new call effects. Five regressions: old 20/55
failed assertions, final 55/55. Native new 288 complete roots vs old 288 incomplete;
no-Z3 adds 48, result/memory controls each 12 differences, zero exclusions/skips/
normalizations. CTest 602/602, no-Z3 477/477; existing Python/guard/slice checks pass.
Frozen original-helper Tigress four groups: 256 C matches each version, LLVM
old 512 complete/1024 incomplete -> new 576/960, zero complete differences or
unexecuted/unlifted. GCC extra entries linear 42/recursive 15 ->0/0. Retain the
64 GCC linear indirect V2 runs that lose accidental resume coverage through
false functions; they are incomplete, never passes. Remaining indirect target
proofs/block-entry support are separate work. Both corpora: 69 inputs/138 strategies; new 205406 valid-function checks and
1675 raw V2 modules verify (old 206649/1686). Hardware decode/known-edge hashes
unchanged; 172570 protected entry checkpoints retained, symbol recall and
invalid-function counts do not regress. Removed 1243 candidates: 121 inside
ELF symbol bodies, 1122 PLT candidates (1071 .plt / 51 .plt.sec) without ordinary
function extent truth. Do not claim all PLT entries were wrong. 1164 removed
candidates remain outside recovered functions, 1163 outside valid
blocks; bytes remain decoded, no guessed CFG edges. 86 affected static owner
definitions verify raw+optimized (172 checks), not the whole optimized corpus.
Lua unlifted histograms unchanged 188/49/90. Detailed scope is
in the component document. Artifacts: `~/vmc-runs/cet-boundaries-20261106/`.
Only use owned `~/dragon-tales-overlap`; one heavy worker and low VM disk space.

Finite-set analysis and opt-in dispatcher recovery (2026-11-06, baseline
`1907235`): see `docs/finite-dispatch.md`. `analysis::finiteValues` is a separate
forward finite over-approximation; the old constant-only slicer stays unchanged.
`finite-values` is a read-only cached Graph step. Register `FiniteDispatchPass`
(C++/C/Python) with owned code/entries to opt into x86 target growth and final
all-incoming-path checks on a cloned Graph. Global budget failure makes every
query Unknown; capacity widening never truncates a target list. Only Guest
STOREs supply memory values; CALL/alias/unlifted/local-flow barriers remain.
No image/initialized-heap snapshots, API-name models, call summaries or deflat.
Direct CALL targets discovered during expansion are independent inputs for BOTH
the legacy and finite proofs; filtering heuristic entries must never erase
real CALL boundaries. A pre-hardening regression fails 4 assertions; independent
64-run / 160-transfer Unicorn entry checks detect 18 wrong edges before and
zero after, while a missing-root-target control detects 16. Pre-hardening
snapshots/results are in `pre-entry/` with relocation hashes and are not final
coverage. The legacy Tigress 2.1 array-opaque regeneration attempt also remains
explicitly failed (56 C matches / 40 reference errors / 240 unexecuted LLVM
comparisons), superseded for combination coverage by real-megaInit frozen
challenge replays. Extra heuristic function entries and remaining incomplete
ordinary goto/indirect paths are retained, not passes. Final 15 C++ cases/25282 assertions; CTest 597/597 and no-Z3 472/472. Native
new 576 full roots vs old 576 incomplete; no-Z3 adds 96 roots. Ordinary Tigress
per version 512 C matches: new 1024 complete/2048 incomplete vs old 960/2112,
zero complete differences; GCC recursive extra function entries 1->15 remain.
Frozen real-megaInit 10-ELF replay per version 320 C,384 complete/256 incomplete,
CFG wrong 0/unlifted 0; 17123 accumulated executed-undecoded observations remain
unchanged in harder profiles. All 120 new Pass invocations there are unchanged.
Both real corpora: 69 inputs/138 strategies/206649 function checkpoints per
version, 1686 raw V2 modules each verify; exact old/new default Graph,IGNIL and
raw-IR fingerprints, zero lift failures/gaps; existing alerts retained. No new
full single-instruction/IGNIL or optimized-static corpus campaign (unchanged
lifters/optimizer). Detailed counts and scope are in the component document; artifacts:
`~/vmc-runs/finite-dispatch-20261106/`. Only use `~/dragon-tales-overlap`.

Composable pass pipelines (2026-11-06, baseline `04c6d91`):
`dragon/passes/PassPipeline.hpp` supplies typed Graph/LLVM registries, ordered
steps, option/dependency preflight, bounded sweeps, per-run analysis caches and
per-invocation reports. C opaque handles/callbacks and Python callable classes
use the same native scheduler. Python default optimize() keeps its six-step
order/options, public run_* hooks and None return value. Optional graph_pipeline
hooks run after initial analysis; initial disassembler algorithms are unchanged.
At that stage only graph-audit was built in at Graph level; no new
cross-call/value-set/deflat algorithms or shared-library loading. See docs/pass-pipeline.md for contracts.
Mutating steps invalidate all caches conservatively; errors stop without
rollback, so discard failed targets. Report ok/converged never means complete
deobfuscation. Plugins are trusted code and must prove/validate transformations.
Callbacks default to ERROR and commit status last; bad exception formatting and
unwritten C results are regression-tested. Python forbids closing active targets
or nesting pipelines on the same target. Pre-existing Graph views expire on a
Graph run; automatic Graph hooks close failed results. Pre-hardened output is
archived separately and is not final validation coverage.
Eight new C++ cases / 158 assertions; original API tests cannot compile because
the header is new. CTest 582/582, no Z3 458/458. Custom class pipeline: 3776 state
matches (1040 root / 480 declared boundaries / 2256 rejected entries); repeated
pipeline: 976 (400 root / 576 rejected). No-Z3 custom pipeline: 84 (36 root /
48 rejected). Result and memory controls each detect 84 single-field differences;
zero new exclusions/skips/normalizations. Native pass reports also check Graph
cache reuse and post-mutation invalidation. On frozen real Tigress ELFs, 48
kernel modules (two strategies/entry policies/MBA enabled or disabled) match old
manual pipeline IR byte-for-byte. This is compatibility, not a new deobfuscation
gain. Frozen Tigress replay gives 288 C matches and 576 complete LLVM root
matches, with zero wrong/incomplete/unlifted/executed-undecoded cases. Real Guest
megaInit still executes; no initialized heap snapshot is injected. Both real
corpora: 69 inputs / 138 strategies / 206649 valid-function checkpoints;
6530 raw + 6530 optimized modules verify, zero lift failures or coverage gaps.
Input hashes and recorded structure retain the baseline, including its known
anomalies. All 138 Graph / 6530 LLVM reports have audited order/status/budgets.
MBA totals: 8 queries / 8 proved rewrites / 0 unknown / 0 rejected; ordinary-code
counters also include state/flag helpers. One unfinished sys_python3 run was
cancelled when concurrent large inputs exhausted VM swap; its logs remain in
memory-retry/, and the isolated complete retry passed. No input was skipped.
Use one heavy corpus worker on this VM. IR verify is not whole-corpus semantic
proof. Final frozen
interfaces, source manifests, controls and results are under
`~/vmc-runs/pass-pipeline-20261106/`. Only use owned `~/dragon-tales-overlap`.

Static function-entry views (2026-11-05/06, baseline `23f46f2`): see
`docs/llvm-static-entry.md`. Explicit `StaticEntryPolicy::FunctionStart` in an
empty StaticAnalysis module emits `dragon_static_entry_*` symbols accepting
only Function::startAddress(); invalid input PCs return Unsupported/Entry and
preserve Guest registers/memory. Default Resumable, V2, internal CFG edges and
call continuations are unchanged. Direct static internal calls use the same
policy, including forward declarations in either definition order. C and Python
expose the policy. No initialization, heap, callee-effect or API-name assumptions.
Four C++ cases / 242 assertions; tests-only old build fails because the API is
new. Native 42 core + 4 import-image cases: 3776 matches, comprising 1520
valid-entry checks (1040 complete root returns / 480 declared boundaries) and
2256 rejected-entry checks; all modelled fields and 64 KiB, no exclusions/skips/
normalizations. Image checks verify new symbol/import metadata; their 128 valid
entries are all boundaries, never complete-function successes.
Old three-case subset: 288 matches / 48 expected entry-contract differences,
six modules missing the restricted contract; all 144 valid-entry comparisons
still match. Do not describe old resumable execution as incorrect semantics.
Result/memory controls detect 84/84 single-field differences each; accepting
interior entries control detects 12/12 while 36 valid-entry checks still match.
CTest 573/573, no Z3 449/449; frozen no-Z3 interfaces/libraries also give 84
native matches (36 complete returns / 48 rejected entries). Frozen Tigress
2.1 seed 4242 three-profile matrix:
288 C and 576 complete LLVM matches, wrong/unlifted/incomplete zero. A separately
regenerated matrix also matches; keep its inputs distinct from the frozen replay.
On the same six frozen ELFs, product kernel IR decreases in both strategies;
f-opaque O0 9134->5395, but extra LLVM O2 grows f-mba O0 3486->4112.
Counts include generated state/memory checks, not just original obfuscation.
Both real corpora: 69 inputs / 138 strategies / 206649 valid-function checkpoints,
6530 raw + 6530 optimized modules verify. Every definition's sole external entry
and policy attribute are checked before/after optimization; zero lift failures
or coverage gaps. Input hashes and recorded structure match baseline, including
known anomalies. Independent workers saved disjoint checked inputs; the final
forward audit validates all 69 records and exact library hashes.
No whole-program semantic or general heap-predicate/deflat claims. Artifacts:
`~/vmc-runs/llvm-static-entry-20261105/`; use only the owned VM worktree
`~/dragon-tales-overlap`, not the dirty original checkout.

LLVM MBA simplification (2026-11-05, baseline `fafea17`): see
`docs/llvm-mba-simplification.md`. New C++/C/Python synthesis API and Python
optimize() stage before predicate folding. Every rewrite needs an UNSAT
full-word equivalence check; 0/1 truth signatures only propose candidates.
Defaults 100 ms / 16 queries / 128 nodes / 3 SSA inputs / 1000000 resources;
width <=128, depth <=64, hard node cap 1024 and total expansion cap 1048576.
No assumptions about image contents, paths or call effects;
PHIs/casts/memory events remain SSA boundaries.
No Z3: unavailable no-op. No Graph/IGNIL/Guest ABI changes or nonlinear/heap
predicate/deflat claims. Eight C++ cases / 320 assertions, 1152 full native root
returns match (102 A64 / 76 x64 fields + 64 KiB), zero exclusions/skips/normalizations.
Old same 1152 semantics also match but 16 static modules miss simplification;
existing optimize() regression fails old. New API C++ tests cannot compile old
headers; that failure is preserved. Result/memory controls each detect 32/32
single-field differences. CTest 568/568, no Z3 444/444 (component 1 case / 5 assertions).
Tigress selected three profiles: 288 C + 576 LLVM matches, zero incomplete,
CFG wrong or unlifted. Default per-kernel proven rewrites f-mba O0/O2=11/1,
f-combo=16/11, f-opaque=0/0, both strategies. f-combo's 2/1 fewer branches also
occur with matched InstCombine/DCE only; do not call them new predicate proofs.
Full two-corpus static checks: 69 inputs / 138 strategies / 206649 recovered
valid function checkpoints; 6530 raw + 6530 optimized modules verify, zero lift
failures or coverage gaps. All input hashes and reported structural fields match
the baseline, including known anomalies. Aggregate MBA: 8 queries,
8 proved rewrites, 0 unknown, 0 rejected; unknown never rewrites.
Ordinary-code counters include flags/state helpers, not only obfuscation.
Artifacts and resumable strict corpus/summary drivers are in
`~/vmc-runs/llvm-mba-20261105/`. They wrap unchanged check_one.py to record
proof counters and actual mapped libraries. IR validity is not whole-corpus
semantic proof. No new single-instruction/IGNIL campaign (unchanged lifters and
optimizers). Existing original VM checkout is dirty; only use the owned
`~/dragon-tales-overlap` worktree. Frozen old/new dual libraries and interface
snapshots are in baseline/ and final/.

LLVM integer predicate folding (2026-11-05, `docs/llvm-predicate-folding.md`):
`LLVMLifter::runPredicateFolding` / C `dragon_llvm_lifter_fold_predicates` /
Python `run_predicate_folding` prove scalar integer branch/select conditions
without path or image-memory assumptions. Python `optimize()` now runs this
after mem2reg/InstCombine/GVN and before DCE. Defaults: 25 ms/query, 64 queries
per module, 256 expression nodes, 200000 Z3 resource units. Only UNSAT/exact
constants rewrite conditions; unknown/unsupported/budget exits retain them.
Distinct SSA loads remain independent; PHIs/freeze are opaque inputs. Reject
explicit undef/poison, overflow flags, variable/invalid/exact shifts, FP,
division, call results and atomic/volatile reads. No Z3: API reports unavailable
and leaves IR unchanged. Graph/IGNIL/dispatch targets and Guest ABI are unchanged.
Ten C++ regressions / 395 assertions, 250 independent A64 condition computations,
192 full-state native root returns (102 fields + 64 KiB), and 16 result + 16
memory negative controls. Old existing `optimize()` regression fails; old 192
semantic comparisons match but eight static modules retain constant branches.
New C++ API tests cannot compile against old headers; that failure is saved.
CTest 556/556; no Z3 439/439. Tigress 2.1 seed 4242 f-mba/f-opaque/f-combo,
O0/O2, both strategies: 288 C and 576 full LLVM matches, no incomplete or
unlifted. Important attribution: all measured Tigress kernel reductions also
occur with zero queries; newly proved SMT conditions are zero, including a
larger-budget followup. Do not call literal CFG cleanup opaque-predicate proof
or claim heap-dependent predicates/deflat/MBA rewriting are implemented.
Full static corpora: 69 inputs / 138 strategies / 206649 function checkpoints,
6530 raw + 6530 optimized modules verify, zero lift failures and complete
coverage of recovered valid functions. All 69 input hashes match the baseline;
reported structural fields in all 138 strategy records remain unchanged,
including known anomalies. This is IR
validity, not whole-corpus semantic proof. VM reboot recovery reused 25 audited
records and checked the remaining 44; the damaged no-Z3 log was retained and
all 439 tests rerun successfully. New and old tested dual libraries and C/Python
interface snapshots are frozen under final/ and baseline/ respectively.
Artifacts and frozen dual libraries are in
`~/vmc-runs/llvm-predicates-20261105/`. The initial V2
optimized corpus attempt is partial and preserved separately after high RAM/
swap use; it must not be reported as full V2 corpus success.

Tigress VM and function-level study (2026-11-04, `docs/tigress-vm-challenge.md`,
`tigress_vm_challenge.py [--function-level]`): Virtualize switch and MBA +
switch-Flatten + true/false opaque predicates execute fully (all 6 plans);
computed-goto/indirect dispatch and nested VMs stay explicit incomplete;
junk-byte overlap is fixed by the follow-up below. C, dynamic-CFG and LLVM wrong 0. Fixed two x86 table
gaps: SMT timeouts on computed indices (abstraction for UNSAT, concrete
witnesses for SAT only) and register-state flattening dispatchers (last-resort
one-block slice at the guard). Corpus tables now 1086/0/190/526/38.
Tigress needs `CILLY_NATIVE_CC=x86_64-linux-gnu-gcc` on the aarch64 VM, its
`megaInit` must run before the kernel, and `/tmp` there is a 1.7 GB tmpfs.

Overlapping x86 decode paths (2026-11-04 follow-up): linear Graph analysis
recovers missing direct jump/call/explicit-entry targets, both strategies split
common exact-address suffixes, and late splits relift every owning decode path
via exact sequential predecessors. No Graph redesign, lifter/API change or
relaxed guard/final-candidate validation. Five regressions: tests-only on
773196f gives 4 failures/17 failed assertions (negative control passes);
fixed 5/5 and 209 assertions. A partial fix retaining stale IGNIL fails 20
assertions. Permanent i386/x64 full-function fixtures: old V2 12 different / 20
match; new V2 32/32 and static 32/32, no exclusions/skips. Same f-opaque O2 ELF:
both strategies x six plans x 24 inputs = 288 complete full-state matches.
Entire matrix per strategy: C 336/336, LLVM 1440 match / 576 incomplete /
0 different; remaining goto/indirect are not passes. CTest 512/512; no Z3 with
Unicorn enabled 409/409 (older no-Z3 builds disabled Unicorn). Guard/slice
harnesses unchanged and clean. All 69 corpus inputs freshly checked: 206649
valid-function checks / 1686 V2 raw modules verify, no lift failures; tables
1086/0/190/526/38 unchanged. Historical structural differences for 24 inputs
were rerun on the frozen old libraries: only this change is two extra decoded
instructions in C++ i386 exception-table data, not a semantic success. Its
linear missing-IGNIL/undecoded-edge counts 2/14 -> 1/13; existing alerts stay
visible. Lua unlifted records/forms unchanged 188/49/90. Worktree/builds on VM:
~/dragon-tales-overlap/{build,build-noz3}; original ~/dragon-tales dirty checkout
preserved. Artifacts ~/vmc-runs/context-audit-20261104. The corrected diagnosis,
remaining exact stop sites and staged repair plan are in
docs/tigress-vm-challenge.md. No whole-corpus semantic proof, new full
single-instruction campaign or generic de-flattening/MBA cleanup is claimed.

Constant x86 control-flow targets (stage 2, 2026-11-04/05): private APInt evaluator
handles pure integer slice trees without optional SMT enumeration, including
wide intermediates and explicit bit widths. Unknowns, free LOADs, floating point,
faulting divisions and exhausted budgets stay unknown. Proven STORE-to-LOAD
values can now resolve memory JMPs; constant address expressions participate in
the existing alias checks. Image bytes/readMemory are not immutability proofs.
New BackwardSlicer::predecessors(graph, entries) overload stops backward walks at
independent entries; x86 recomputes them after expansions, including newly decoded
callees. Candidate rejection/final completeness checks are unchanged. Tests-only
on 1c61039: 33 failed assertions; final e2e tests with its frozen libraries:
44 failures. Fixed: 8 tests/9397 assertions, including 3082 APInt/Z3 expressions;
no Z3: 7 tests/151 assertions. Intermediate entry mistakes fail 4 and 1 assertions.
Four permanent i386/x64 full-function fixtures: old V2 64 different (early
unresolved exits), fixed V2
64/64 and static 64/64 root returns, all modelled state/64 KiB memory, no new
exclusions/skips. CTest 520/520, no Z3 + Unicorn 416/416; guard/slice harnesses
clean. Lua unlifted records/forms identical 188/49/90. Tigress goto O2 first
target 0x103d8 -> 0x106be is now recognized then withdrawn after CFG expansion:
its complete target set is still unknown. Indirect O2 0x1017a still loads an
unproven image pointer. These are incomplete, not completed deobfuscation.
Final identical-image matrix remains 2880 match / 1152 incomplete / 0 different,
C 672/672, CFG wrong 0. All 69 corpus inputs freshly rerun: 206649 function
checks / 1686 V2 raw modules verify; structural counts exactly match stage 1,
including its existing alerts. All input hashes unchanged; table records remain
1086/0/190/526/38 in all 138 strategy records. This is not whole-corpus semantic
proof. Artifacts ~/vmc-runs/constant-flow-20261104; final matrix/corpus checks and scope
are recorded in docs/tigress-vm-challenge.md.

IGNIL snapshot hardening (2026-11-05): simplifyBlock's Reg copy/expression
records now all invalidate on a 32-bit Reg write. This architecture-neutral
API has no parent extents, so guessing eight bytes misses wider parents;
retain the original Temp snapshots instead. Byte/word disjoint propagation
and constant/Temp propagation stay available. Tests-only on 69ee6f1: 26 failed
assertions; adding the x64/A64 Unicorn oracle gives 78 (52 wrong output fields).
Fixed: 4 tests / 13151 assertions, 96 machine references / 192 IGNIL state
comparisons, no exclusions/skips. CTest 524/524, no Z3 419/419. Random opt
equivalence: each old/new four plans x 1148 comparisons, 0 wrong; control
1145 differences (13 unencodable generator candidates reported separately).
Optimized real blocks: 450 blocks / 1235 compared trials / 115 faults,
0 DIFFERENT, old/new logs identical; existing memory/high-address and block
selection limits remain explicit in docs/ignil-optimizer-hardening.md. Slight
precision cost: simplify+DSE ops 11437 -> 11444 out of 25348 initial ops.
No new raw-IR corpus/Tigress campaign: those production paths do not call
simplifyBlock; changed optimization paths and full CTest were rerun.
Artifacts ~/vmc-runs/simplify-snapshots-20261105; original dirty VM worktree
preserved, development/builds still use ~/dragon-tales-overlap.

IGNIL integer-fold hardening (2026-11-05, baseline 454fb45): NEG uses unsigned
modular subtraction, signed division/remainder leave INT64_MIN/-1 intact,
and zero-width constants never enter host integer folding. Tests-only:
11 failed assertions; fixed integer group 4 tests/4761 assertions, no Z3
3/145. Unicorn A64 arithmetic oracle: 416 calculations, 832 original/optimized
result matches; native A64 assembly confirms the extreme results. A permanent
standalone target instruments the production optimizer with UBSan when the
compiler/runtime support it: four old-source probes fail, all five new CTests
pass, including 109568 consumed fold combinations over every width 1..64.
This sanitizer sweep is not proof of arbitrary mixed-width IR semantics.
Latest complete CTest 533/533, no Z3 427/427. Random equivalence/control and
the same 450 real blocks/1235 comparisons were rerun, 0 new differences;
existing 115 faults and memory limits stay visible, repeated inputs are not
counted as new unique samples. No instruction/API/ABI changes. See
docs/ignil-optimizer-hardening.md; artifacts ~/vmc-runs/simplify-integer-20261105.

IGNIL COPY-width hardening (2026-11-05, baseline 5aaf6f0): copy propagation
now resizes constants to the destination Temp width, masking source payload
first; Reg/Temp copies propagate directly only at equal nonzero widths.
Keep actual COPY truncation/zero extension otherwise. This fixes wrong signed
values, lost truncation/double-NOT cancellation and invalid mixed-width LLVM.
Tests-only: all 5 regressions fail, 545 assertions (449 wrong Unicorn fields,
48 invalid modules); fixed 5/119074, no Z3 4/550. Reference matrix: 720 A64
Unicorn executions / 1440 original-optimized GPR/flags/64-byte-data comparisons,
no exclusions/skips; 120 x64/A64 LLVM modules verify. Four minimal IR fixtures
compiled natively at O0/O2: old 8 original matches, 6 optimized differences,
2 optimized compiler rejections; new 16 matches. Latest CTest 538/538,
no Z3 431/431 including all five UBSan checks. Random four plans x 1148
old/new comparisons pass, control 1145 differences; 13 unencodable candidates.
Same 450 real blocks/1235 compared trials rerun, no new differences; 115 faults
and previous memory/selection limits remain. simplify+DSE still 11444/25348 ops.
No new raw-IR corpus/Tigress/single-instruction campaign, no ABI/lifter changes;
see docs/ignil-optimizer-hardening.md for scope and artifacts in
~/vmc-runs/simplify-widths-20261105. Do not claim arbitrary mixed-width ALU IR
or the entire optimizer has been proved correct.

IGNIL DSE transfer hardening (2026-11-05, baseline 5bcdb5e): later writes
must not kill state observed at an earlier CALL/JMP/CJMP/RET. Mark all mentioned
register bytes (including configured implicit parent clearing) live at CALL
and interior transfers. A final CALL also retains unknown implicit callee
inputs despite empty caller live-out; final non-CALL explicit live-out still
works. CALL can occur before later ops in an actual analysed IGNIL block:
mov rdi,rcx; call callee; mov rdi,9; ret was reduced from 4 to 3 ops by old DSE,
losing its argument. Callee mov rax,rdi; ret then returned the initial RDI.
Four old regressions fail: 432 assertions, including 416 wrong Unicorn fields;
fixed 4/47350, no Z3 3/30. Permanent x64/A64 prefix matrix: 160 references,
640 GPR/flags/64-byte-data comparisons, no exclusions/skips; it stops BEFORE
the machine transfer, so do not count prefixes as complete CALL/RET execution.
Actual caller/callee V2/native vs Unicorn: old original 16 full matches,
old default/pipeline 32 wrong RAX results, new 48 full matches at O0/O2 across
8 inputs, all 76 state fields and 64 KiB memory, actual CALL/RET and root exits,
no exclusions/skips/normalizations. Latest CTest 542/542, no Z3 434/434,
all five UBSan checks included. Random four plans x 1148 old/new comparisons
pass, control 1145 differences, 13 unencodable candidates; same 450 real
blocks/1235 compared trials rerun with 115 faults and previous limits intact.
These real-block harnesses skip CALL/system blocks, so the new call checks
cover a previously untested optimization path. No API/ABI/lifter/runtime
semantic changes, no new raw-IR corpus/Tigress/single-instruction campaign.
See docs/ignil-optimizer-hardening.md; artifacts
~/vmc-runs/optimizer-transfers-20261105. Whole-optimizer correctness, callee
effects and flattened dispatch coverage remain separate work.

Real-binary external call audit (2026-11-04, `docs/external-call-audit.md`):
69 samples x 2 strategies, all caller functions lowered. Fixed two analysis
bugs: import provenance on fall-through edges/non-thunk blocks (wrong and
spurious names on Lua/zlib/sqlite/C++) and register slicing across CALL (wrong
A64 `br x0` target after `bl`). After both: provenance wrong 0, spurious 0.
Tables 1124/0/116/558/42 -> 1084/0/192/526/38 (exact/wrong/unresolved/
nobound/unreadable). Guest semantics: 80/80 V2 and 80/80 static boundary
matches, also on the old code (execution was already correct). Measured, not
changed: fall-through edges after noreturn APIs (no API-name model), `endbr64`
after `_setjmp` taken as a function start, missing names after unlifted PAC.
CTest 504/504, no Z3 355/355; slicer harnesses unchanged (3000, 448/192,
128/4, 27 CFGs/20736 executions, 0 wrong).

Tigress + external API study (2026-11-04): `tigress_external_api.py` builds real
ELF caller/provider DSOs, eager RELA binding, PLT/GOT calls and anonymous targets.
11 cases: return gates, alias writes, memcpy/memcmp/strlen, indirect call,
callback, varargs, FP return, tail and unknown noreturn boundary. Independent
ARM C/Python specifications and x64 Unicorn; provider IR is optimized separately
and linked only after caller optimization. No production API effects models.
Main 16-input campaign: 16128 executions = 3936 root matches (1984 obfuscated),
6784 exact API boundaries, 5408 incomplete. 0 differences/errors/skips/exclusions.
1920 host checks and 5920 C-spec observations match (1920 unique x64 inputs).
528 caller + 8 provider modules verify; entry errors/unlifted/forbidden API
attributes 0. Loader recognizes all 480 named checks; IR provenance absent in
120 unresolved obfuscated O2 modules; anonymous pointers do not acquire names.
Static PLT/tail transfers need a consumer; registered provider code does not
automatically complete them. The 32 plain recursive tail runs that stop before
the PLT were separately checked at that declared boundary: 32 exact matches;
the remaining 5376 incomplete runs are obfuscated O2 dispatch gaps.
Extra memcmp 17-input campaign includes equality: 408 root / 680 API boundary /
544 incomplete, no differences; overlap with the original 16 is explicit.
Current default is 17; main used the preserved harness snapshot with 16 inputs.
Controls reject a wrong provider result (32 host + 32 C), stale RAX (16 of 32
known returns; inputs whose API result is zero legitimately match), lost writes (32); unknown
prefixes stay correct. Four plain-C CTests: 499/499, no Z3 350/350.
No production/interface changes or new whole-corpus/single-instruction campaign.
See docs/tigress-external-api-validation.md; never count API boundary snapshots
as complete functions or the mixed O2 matrix as passed.

Complete SHA-256 experiment (2026-11-04): `tigress_sha256.py` and
`tests/fixtures/tigress-sha256.c` cover all 64 rounds, schedule, streaming updates,
padding and actual internal calls. Tigress 2.1 seed 2561901 transforms compress,
update and finish with switch Flatten or EncodeArithmetic then Flatten.
C O0/O2, both strategies, V2 O0 and product static O2: 400 full-state matches
(200 plain, 200 obfuscated O0), 200 incomplete obfuscated O2 exits. No complete
semantic differences, exclusions, unexecuted comparisons or successful skips.
Host C/hashlib checks 156/156 incl. million-a; Unicorn digest checks 150/150,
25 inputs up to 4096 bytes. Million-a is NOT part of the LLVM memory experiment.
Entries 12/12; unlifted 0. O0 compress grows from 10/249 blocks/instructions to
59/351 (Flatten) and 59/1598 (arithmetic+Flatten). Obfuscated O2 has three
unresolved dispatches per image; never claim the campaign passed or its tiny
partial CFG is successful simplification. Observed-edge omission controls 12/12.
IMPORTANT: Tigress moves K's initializer into K_i$nit/megaInit, called by its
generated main. Library-entry validation must execute the real Guest megaInit
before kernel; preserve its arguments. Do not inject correct K bytes instead.
The initial omission failed both digest references while LLVM matched the wrong
entry's bytes. Keep that diagnostic; it was a harness omission, not a Tigress bug.
Controls reject wrong C digest (host/Unicorn 5 each), damaged RAX (10), and
damaged memory (10); opposite layers stay correct. Four plain-C CTest checks
need no proprietary generator: CTest 495/495, no Z3 346/346. No production
interfaces/semantics changed; no new whole-corpus or single-instruction campaign.
See docs/tigress-sha256-validation.md for exact failures, outputs and limits.

Tigress function checks (2026-11-04): `tigress_function_diff.py` uses bounded
unsigned C functions, a separate Python specification, original/transformed
Unicorn observables, then each ELF image's own full-state LLVM comparison.
It preserves ELF code/data/BSS, uses actual Guest memcpy code, and checks
function entries against STT_FUNC symbols without injecting them as repairs.
Public GitHub Tigress 2.1 revision 2128 is pinned in
`docs/tigress-function-validation.md`; no newer-version coverage is claimed.
Three seeds / six profiles / C O0/O2 / six LLVM plans in both strategies:
3840 complete returns match; 1152 explicit incomplete exits; 1920 planned LLVM
comparisons unexecuted after reference errors. No exclusions or successful skips.
The old array opaque output has a zero divisor; the first example also raises
SIGFPE under Rosetta. Do not count these as Dragon semantic comparisons.
Three controls detect wrong C results (32/32), damaged LLVM RAX (192/192), and
damaged LLVM memory (192/192); `--expect-control` also requires the other oracle
layer and function-entry checks to pass. Four CTest cases exercise the harness
with plain C, independently of proprietary Tigress availability.
The new checks found LLVM's INT3 descriptor lacks isTrap: x86Analysis now skips
single-byte 0xcc only as post-epilogue alignment. Do not classify every INT3 as
an epilogue: that invents entries in jump-table regions and breaks three
SwitchDetector regressions. Instruction execution/public interfaces stay intact.
RET and padding can also separate indirect cases. Regions use the entry
candidates present before epilogue guessing; an unresolved indirect jump in
that region prevents RET-only promotion of another unsupported entry candidate.
Separate known regions and existing call/symbol/prologue candidates remain.
x86Info also recognizes
LLVM 21's RET32/RETI32 names when discovering i386 epilogues.
RET-only candidates that are also concrete memory-reference targets stay
ambiguous unless another entry cue already exists. Recognize only default-width
absolute/RIP-relative addresses with no index/segment; skip address overrides.
This prevents i386 `one/limit` literal pools from becoming functions (an
intermediate RET32 fix exposed six spurious AAS/POP unlifted records).
Five C++ tests fail on the original backend and pass after
(30 assertions, i386/x64, explicit INT3 entry, RET imm16, separate known regions,
and code/data dual use with explicit entry evidence).
With the same 26 executable ELFs, 24 padding entries and three internal case
entries disappear; linear entry failures 26 -> 0, recursive stays 0.
Switch O2 and goto O0 unresolved dispatches remain incomplete, never successful
whole-function validations. CTest 491/491; no Z3 342/342.
All 32 SwitchDetector tests / 199 assertions pass. Snapshot both libdragon_c.so
and its libdragon.so dependency for old/new comparisons; set LD_LIBRARY_PATH
to the complete snapshot. The harness hashes its actually mapped backend.
Real-corpus final audit: 69 inputs / 138 strategies / 206651 function checks /
1686 V2 raw IR modules, zero new baseline anomalies. Reran all 38 x86 inputs
after the last x86-only change; reused 31 unchanged ARM records with input
hashes/provenance. This is IR verification, not whole-corpus semantic proof.
Lua table checks: exact 104, wrong 0, unresolved 10, no bound 76. Unlifted
records/forms remain identical: i386 188, x64 49, A64 90.
Real blocks: 450 executions, 449 strict matches, one retained reference DIFF;
144 skips (83 high address, 57 Unicorn faults, 4 unlifted). The i386 block
0x43319d loads raw80 7a5c4474d2addbb03b5b (J=0 E!=0). Unicorn dev's FXAM
classifies it as normal; Intel SDM Vol1 8.2.2 and Rosetta support Dragon's
unsupported classification (C3/C2/C0=000). x87_native.c reproduces it;
TestX87.cpp fixes the correct status/EAX/CF/SF in 12 assertions, passing old
and new backends. No production change or new exclusion/normalization; the
strict real-block DIFF remains visible. See the Tigress document for raw logs.
Real Lua still has internal false entries (84 GCC, 83 Clang) and GCC recall
699/700; do not describe arbitrary stripped function recovery as correct.

Generated function semantics (2026-11-04): `generated_function_diff.py` builds
seeded x64 diamond/alias/bounded-loop/call functions and NOP layout variants.
Unicorn independently executes each version's bytes; six V2/static/product
O0/O2 plans compare all 76 modelled fields and all 64 KiB, with no exclusions.
64 generated functions / 6144 root-return comparisons match, zero boundaries
or skips. Known entry checks 128/128, full no-CALL CFG checks 96/96; all edges
observed in 192 nonempty branch coverage checks (not proof of all paths).
Wrong-branch and stale-callee-state controls each report 96/96 differences;
`--expect-control` rejects infrastructure errors or missing comparisons.
The CFG checker also rejects omitted edges and spurious entries. CTest now
482/482, no Z3 333/333. No production semantics or ABI changes. See
`docs/generated-function-validation.md` for commands, raw failures and limits.

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
| `x87_harness_control.py MODE BIN [N]` | Negative control for `diff` / `llvm`: corrupt only tested raw FPU/CW input after seeding Unicorn; each harness must report positive DIFFERENT. Normal harnesses are untouched. |
| `difftest_sse.py` | 393 SSE/SSSE3/SSE4.1 templates x 60 inputs; includes saturation boundaries, FP32/64 exceptional values and conversions. Checks FP32 per lane, including untouched lanes; only active arithmetic NaN results may be unknown. `ONLY=text` filters. |
| `opt_equiv.py` | IGNIL `simplify()`/`dead_store_eliminate()` preserve semantics on random blocks (`CONTROL=1` adds a deliberately unsound variant to prove the check bites). |
| `slice_diff.py` | BackwardSlicer vs SymbolicExecutor on random blocks mixing 8/16/32/64-bit writes. |
| `memory_slice_diff.py` | Stack-slot STORE/LOAD forwarding vs 448 Unicorn executions; 192 unique slices, 0 wrong. Includes disjoint/overlapping/unknown-alias stores and SP changes. |
| `loop_slice_diff.py` | Nested-join loop slices vs 128 Unicorn executions: invariants, differing writes, increments and cross-register copies. |
| `guard_truth.py` | 27 CFGs vs 20,736 Unicorn executions: byte/word/dword guards, signedness, boolean holes, narrowing and masks. Also runs without Z3. |
| `function_llvm_diff.py` | Historical Legacy whole-function baseline. Strict default remains red (A stale state; B now explicitly rejected). `--assert-baseline` describes the saved 0aa4b73 implementation, not current V2 semantics. `--corrupt-output` is a negative control. See `docs/llvm-flow-step1-validation.md` for counts and Legacy CALL/RET normalizations. |
| `function_dispatch_diff.py` | V2 x64 control-flow prefixes vs Unicorn at O0/O2. Includes branches, indirect/zero/missing targets, loops, budget and unsupported-instruction boundaries; compares PC/SP/full snapshots and memory with zero Legacy normalizations. prefix_match is not full CALL/RET success. |
| `function_call_diff.py` | x64 direct CALL/state/resumption oracle vs Unicorn, O0/O2. C models implement exact fixture code plus Guest RET; the root stops before its still-unimplemented RET. Covers parameters, non-ABI GPR/vector clobbers, flags, raw x87 and missing-model resume. No Legacy normalizations; only declared undefined flags/status bits excluded. |
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
| `external_call_audit.py BIN OBJDUMP` | Every call/jump through an import slot of a real ELF/PE (truth: pyelftools/pefile slots + objdump stubs, thunk chains, `rex.W`/`bnd` forms, register-loaded slots): decoding, callee edge, continuation, noreturn fall-through edges, and `!dragon.imports` correct/missing/wrong/spurious of every lifted caller. `--control names|fallthrough` must fail. `external_call_corpus.py OUT DIR...` runs it on corpora, both strategies. |
| `external_guest_diff.py [--static]` | Import-slot calls through actual Guest API code vs Unicorn (i386 stdcall `ret 8`, non-ABI clobbers, API returning a function pointer, Win64 home space, A64 PLT); fixtures `tests/fixtures/llvm-external-guest.json`. `--static` compares at the declared API-entry boundary. |
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

An IGNIL CALL ends register slicing: every register after a call is a fresh
unknown identified by that call (`SymReg` origin = block, CALL index, context).
No callee-saved/ABI preservation is assumed (stdcall pops, non-ABI code, APIs
returning function pointers); verified corpus tables 1124 -> 1084 (x86-64
-42, sqlite A64 +2), the owner-approved choice. Recovering them needs a *proven*
callee summary, never an ABI assumption. `!dragon.imports` names only machine
call/jump transfers and pure thunks (temps, PC, A64 X16/X17 writes only);
block fall-through JMPs are never import transfers.

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

The owner approved `docs/llvm-static-analysis-contract.md` on 2026-11-03.
StaticAnalysis=2 (`DRAGON_LLVM_STATIC_ANALYSIS`, Python STATIC_ANALYSIS) emits
four-pointer `dragon_static_*` functions and opaque `dragon_static_external`
calls without a V2 budget/runtime. Step 1 reloads state and guards normal call
continuation. Step 2 emits direct calls to known static entries, accepts forward
declarations in either lift order, and preserves declarations after a failed body
lift. Three-architecture static execution: 91 fixtures / 1456 matches, zero skips;
Return900 (804 root returns, 96 changed-target return boundaries) /
Transfer220 / NeedTarget96 / Unsupported240. The 18 Exit
protocol variants compare 288 snapshots, and non-returning stubs terminate all
16 children with the expected code; CTest 478/478. Static JMP/thunk/altered-return
boundaries are explicit and do not imply complete cross-function execution.
The static Guest oracle declares those boundaries before comparing Unicorn;
no state fields or memory are normalized. Artifacts: static-step2-guests-final
and static-step2-exits-final. Step 3 is complete (2026-11-04): CTest 479/479,
no Z3 330/330. Product optimize() then O0/O2: 1728 matches, zero skips (1070
root returns and 658 declared boundaries, including 96 other-target returns);
288 Exit snapshots plus 16 expected non-returning children also pass. Memory
and defined x87 C1 controls each produce 4/4 differences. Static corpora: all
69 samples / 138 strategies / 203113 function checks, 5227 raw plus 5227
optimized modules verify, all structural baselines unchanged. Import metadata
159595 references are diagnostic, not semantic passes. LLVM_MODE=static in
check_one.py enables both verifies and optimize(); static chunks default to 32
after memory pressure with 128 (no functions dropped). Summary reporting has
10 deliberate negative controls. Real Legacy blocks: 450 match / 142 registered
skips; six Lua table runs exact104/wrong0/unresolved10/nobound76/unreadable0,
identical to their prior outputs. Unlifted x32/x64/A64 stays 188/49/90, full
histograms unchanged. These block/table tests do not replace static function
execution, and corpus verification is not semantic execution of real libraries.
Artifacts: VM validation-results/static-step3-*, progress.json records all jobs.
See `docs/llvm-static-analysis-validation.md` for failures on old mode selection,
CTest 474/474 and 272 full-state/memory static comparisons at O0/O2. The native
validation links C fixture stubs only after opt and needs no libdragon runtime;
the product installs no external models. Invalid exits must not retain stale state.
No IGNIL CALL/RET or existing V2 ABI/name changes. The standalone
`scripts/validation/static_call_contract_probe.py` checks a hand-written LLVM
call-effect contract only: 88/88 correct native comparisons; stale-cache control
66 differences; false-memory(none) control 33 differences (O2 only); zero skips;
66 IR verifies. It does not validate machine CALL/RET or static deobfuscation.
The initial control assertion incorrectly expected llc O0 to exploit memory(none);
it failed, and now asserts the observed O0/O2 behavior separately for every input,
without excluding any records. Artifacts: VM validation-results/static-contract-probe-final.

Task 8 LLVM call boundaries and out-of-function flow are implemented and validated;
plan and history: `docs/llvm-control-flow.md`.
The owner requested continuing with the submitted contract on 2026-11-03.
Step 2 adds explicit StatefulV2 mode and uniform `dragon_v2_<arch>_<addr>` four-pointer
implementations (state, memory, runtime, exit), mirrored in C/Python. See
`Execution.h` and the contract for the ABI/preconditions. `executeV2` dispatches by
Guest PC; missing targets, budgets and unsupported instructions are explicit exits.
Step 3 handles ordinary x64 direct CALL: it pushes the Guest return address,
flushes state, exits to the dispatcher, and resumes via `dragon.entrypoints`
with fresh parameter loads. No caller SSA survives a callee. Legacy rejects CALL
as well as unsupported branches; trace operations after a true RET stay ignored.
No A/B expected-failure markers remain. Step 4 implements near x64/i386 CALL/RET
(including i386 word forms) and ordinary A64 BL/BLR/RET Xn; targets are evaluated
before SP/LR changes. Only rootExitPC ends execution; altered returns dispatch
by their actual Guest address. Far/system/authenticated transfers and long-mode
66-prefixed CALL/RET remain explicit Unsupported. The new three-architecture
function oracle and existing V2 runners execute the root RET and separately
count normal returns and declared terminal boundaries. No V2 Legacy normalization.
See `docs/llvm-flow-step4-validation.md`. Step 5 carries copied import-slot metadata
through Graph's existing pimpl and records conservative `!dragon.imports` provenance
(site, slot, name) without renaming any V2 symbol or calling the host by name.
Provenance is local to a block, invalidates aliases/calls, and follows at most
8 simple JMP thunks. Conflicting merged names stay discarded. Unknown names
still dispatch by actual Guest address. See `docs/llvm-flow-step5-validation.md`;
step 6 is complete: `docs/llvm-flow-step6-validation.md`.
Original task 8 acceptance: CTest 469/469 (no Z3 320/320); 1792 native V2 records match
(1226 root Return, 262 NeedTarget, 288 Unsupported, 16 Budget), zero skips.
Both corpora: 69 samples, 203113 functions / 1656 modules verify, no new baseline
alerts; table oracles exact1124 / wrong0 / unresolved116 / nobound558 / unreadable42.
Real LLVM blocks: 450 comparisons, 142 registered skips, zero differences; toy-vm
6 tests/107 assertions; guards each27 CFGs/20736 executions with and without Z3.
Unknown decoded CALL/RET cannot bypass Legacy refusal; complete trace blocks must
have no unlifted markers. Far/word-memory JMP targets stay UNDEF and unlifted.
V2 also refuses unsupported x86 branch prefixes/word forms and authenticated A64
branches before execution. No new ABI/name or CALL/RET-abstraction changes in step6.
x87 SW.TOP is materialized from authoritative FPTOP at V2 boundaries. The two
call-x87 fixtures exclude only manual-undefined FSTP C0/C2/C3 bits (8/10/14),
not TOP, C1 or exception bits; `--corrupt-fpsw` flips defined C1 and must fail.
See `docs/llvm-flow-step3-validation.md` for counts, exact masks and limitations.
For corpus IR verification use `LLVM_MODE=v2` (check_one.py); default Legacy now
reports intentional refusal of functions whose transfers it cannot represent.

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
6. External calls (see `docs/external-call-audit.md`): proven callee-saved
   summaries for analysed internal callees would recover the 40 x86-64 tables
   lost by strict CALL slicing; noreturn/returns_twice knowledge of imports
   (fall-through edges, `endbr64` after `_setjmp`) is an API-model design
   question - ask the owner; A64 PAC lifting would restore missing names.
7. Design questions, not bugs (ask the owner before changing): CALL/RET
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
