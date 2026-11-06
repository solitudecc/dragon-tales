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
The general x86 emitAddress path currently ignores FS/GS segment bases without
an unlifted marker: helper `55 64 48 8b 2c 24 48 83 c4 08 c3`, FS_BASE=0x1000,
saved RBP=0x1040, FS slot=0x1060 actually returns RBP=0x1060, not the saved value.
The proof must reject explicit segment overrides in the real prefix region;
do not mistake immediate/displacement/opcode bytes for prefixes. The underlying
general-lifter segment-base semantics remain an unresolved gap outside this
patch. Empty unlifted lists are not sufficient evidence for those instructions.
This callee-proof guard is not a fix for segmented accesses in ordinary root
finite analysis, legacy slicing or direct LLVM lifting; check those separately.
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
