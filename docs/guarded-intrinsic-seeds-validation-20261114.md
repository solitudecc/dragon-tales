# Executed intrinsic seeds: validation checkpoint (2026-11-14)

This child changes admission of already-executed mathematical seeds for the
opt-in guarded-store-expressions profile. The child implementation is commit289bb23. The parent implementation is on main
at 4bde2e80fcb32cb1abbaeadaab5e055f16682b00; its production evidence is documented
in [the parent checkpoint](guarded-store-expressions-validation-20261114.md).

At this checkpoint, source-linked prototype tests, actual child production
focused tests, bounded native fixtures, frozen real replay and independent
saved-data audits have completed. Child historical full CTest is main1163/1163 and no-Z31028/1028, both exit0.
Actual main/no-Z3 IR transfer checks and the old-no-Z3 regression are also
complete. The aggregate evidence and storage-operation receipts are sealed
and independently verified on both hosts. These are fresh child results,
not inherited parent counts.

## Change and semantic boundary

The new profile distinguishes a seed that already executed from the corridor
that will be copied. It admits recognized LLVM fshl/fshr observations in that
seed, or in earlier instructions outside the copied path that dominate the
seed. Actual calls and arguments stay in their original execution. Their
results do not become trusted facts, and no call is duplicated into a corridor.

Every copied-path call remains a barrier. Ordinary calls are not admitted by
the whitelist; bundles, convergent calls and direct undef/poison arguments
remain refused. A legal independent suffix after a call may still use its
block as an executed seed. The contract is no copied call, not zero rewrites
for every function that contains one.

STORE values and image bytes remain untrusted candidate suggestions. Actual
operations, state/target LOADs, range/memory checks, flags, shared business
effects and complete same-observation fallbacks remain authoritative.
There is no immutable Guest memory, read_known_memory, noalias or guessed
historical value. Other profiles/default recipes and all budgets are unchanged.

## Focused regression and retained fixture failures

Initial test source a654... constructed a UINT64_MAX count directly as an i32
constant. The first child after run, on main only, aborted in LLVM APInt with
"Value is not an N-bit unsigned value":11 cases/1064 assertions had been reached,
1063 assertions passed. This is a test-constant construction failure, not a
transformed-program execution failure. That initial after run was not repeated
on no-Z3; this constructor failure is a main-only history record.

Corrected test source6117... uses
llvm::APInt(64, count).zextOrTrunc(width) both for constructing the constant and
for checking its expected typed value. The count input remains, including its
modular i32 projection. No input/assertion was deleted and core d4e... was
unchanged by the fixture repair.

| Saved source-linked focused run | Result |
|---|---|
| Initial main before |2 cases/422 assertions;1 case/1 assertion fails |
| Corrected main before |2 cases/422 assertions;1 case/1 assertion still fails |
| Corrected child after |12 cases/2695 assertions pass |
| Same corrected object against actual parent no-Z3 |2 cases/422 assertions;1 case/1 assertion fails |

These receipts are under child focused-v1/v2-before/after-receipt directories.
The corrected before run still detects the missing intrinsic-seed precision.

## Actual production focused/full validation

The focused clients contain tests.o and Catch only, linked against each actual
child shared backend. LD_DEBUG confirms only the corresponding child backend
is loaded, with no core/pipeline object or shared-LLVM interposition.

| Actual child backend | Focused | Historical full CTest |
|---|---:|---:|
| Main |12 cases/2695 assertions pass |1163/1163, exit0 |
| No Z3 |12 cases/2695 assertions pass |1028/1028, exit0 |

Main backend SHA256:
27402ba7d4a69cb1a3bc4d9e81e3863c0b090e1e630668ad9d3da57951c51fac.
No-Z3 backend SHA256:
c41909bbda1ab44785146ab67dfcb79752cced1ee91dd1439a82c4615487e0ac.

The first main focused-client link used clang rather than clang++ and failed
on Catch's missing C++ standard-library dependency. The corrected link uses
clang++-21; main and no-Z3 then pass. The failed link/receipts remain, without
changing backend semantics or assertions. Historical CTest counts do not add
the separately built12 focused cases to their totals. Full validation retains
the1GiB start/64MiB running/180-second guards.

The actual main and no-Z3 probes each regenerate all four frozen real products
with unchanged64/32/128/16384 options. Every parsed-before module, after module
and report is byte-identical to the corresponding source-linked child product:
12 files per backend, also12/12 identical across main/no-Z3. No report field is
normalized. Loader traces bind each probe to its actual child backend; no
shared LLVM is loaded. Thus the exact IR used by the independent native/real
campaigns is also produced by both actual shared backends.

The same corrected test object linked only to parent no-Z3 d816... fails
one case/one assertion among2 cases/422 assertions at result.changed=false,
with expected exit1 and no guard failure. Its loader is the parent backend.
The already saved main before has the same failure. Public API acceptance is
unchanged here: these are missing-precision regressions, not missing builtins
or proof of an old execution error.

Production-validation summary SHA256:
1716e069c2d1291f3545a9b9d18d19a8502ee14b6f91d3f5ec9128466b2e4b7a.
Actual-probe audit SHA256:
3009b525145483702994adf506ca5eb9552b5d50ac97c8a61fa84693cfad1a50.
Old-no-Z3 regression audit SHA256:
429aaeb986e0a7427417a966b08ae30ab1b296d278bcc905475cc58ac2cfc06a.

## Independent bounded native domain

Frozen emitter/reference:
scripts/validation/guarded_store_intrinsic_seed_oracle.py, SHA256
0861f17d006e9b4118c6b24ee902b0240e936525e6c084c2b09fe40055d7baed.

There are16 fixtures:32/64-bit fshl/fshr by four placements: executed seed,
LOAD-then-table-write, earlier dominating observation and copied-path call
barrier. Each width has16 fixed input keys. A seed writes an observable counter
and rotation trace, chooses enc0/enc6 from actual data, and retains state6/range
and memory-error exits. Prediction metadata contains only the admissible index0
entry. Counts0, width-1, width, width+1 and2*width-1 exercise defined modular
funnel-intrinsic behavior.

Each loop visit reads fresh word/count data and carries separate old-rotation
and old-target PHIs. Independent Python bit-string rotations and memory
execution calculate a u64 return plus every73728 arena bytes. Transformed
LLVM is never the expected value. Native fixtures use identical first/second funnel operands (rotations); the
structural C++ cases also use different operands. The pass never evaluates
funnel semantics. Native fixture code contains no unknown system calls,
heap or VM model.

The saved products in this campaign were emitted by the source-linked child
prototype. Actual shared-backend focused tests above are separate; relinking
production is not counted as another native campaign. Four actual runs cover
host O0/O2 and ordinary/wrong prediction manifests.
Each reports512 before/after complete-state matches and768 comparator controls.
Each executes384 fault observations;253 mathematically specified differences
are all detected. Faults change count, omit/double the seed effect, reuse an old
freeze, replace history with the fresh value, or reload after a table write.
Poison and ordinary closed-wrapper calls are structural-only refusals, never
native fault executions.

Actual totals:2048 full-state matches,3072 comparator controls,1536 fault
observations and1012 specified detections. These repeat one domain four times.
Each width has16 effective initial arenas; x values number9 for i32 and10 for
i64. Per run there are256 unique function/input tuples and384 unique
fault/function/input tuples, with253 specified differences.

The new saved-frame checker audits all eight gzip streams: exact4I headers,
domains/lengths/CRC/EOF, complete input bytes, frozen pure reference and all
return/arena bytes. All1536 actual fault outputs match their fault models and
their changed-byte sets exactly equal the specified sets. Descriptor, Exit,
padding and remainder bytes are not ignored or normalized. This reuses the
frozen reference, not a claimed second semantic oracle or another native run.

## Real outputs and independent conditional business SCC

All four child parsed-before modules are byte-identical to parent real-v2.
Manual C-O0 and both flatten-switch after modules also remain byte-identical
to the parent. Only manual C-O2 changes.

| Frozen image | Parent rewrites | Child rewrites | Child cloned blocks | Core loop contexts |
|---|---:|---:|---:|---:|
| manual C-O0 |0|0|0|0|
| manual C-O2 |4|6|40|0|
| flatten-switch C-O0 |16|16|126|0|
| flatten-switch C-O2 |25|25|124|0|

Manual C-O2 has24 candidates/attempts,6 rewrites,1286 copied instructions and18
rejected/unresolved candidates. It remains incomplete and retains its
whole-module budget diagnostic. Copies are not recovered-operation counts.

The two new seeds are memory.valid103 and memory.valid113, not125. Seed103
retains fshl %1428 and guards actual target LOAD %2825 for case4988; seed113
retains fshl %1581 and guards LOAD %2886 for case4552. Misses consume those
same observations and preserve the entire original switch case/default map,
without reloading. All40 new blocks have zero calls; original per-block math
call multisets remain exact.

A separate graph audit finds one new48-block conditional inner-business SCC:
24 original business/shared blocks plus24 new blocks. Parent has no such SCC.
The minimal conditional view removes only original central LOAD/switch block
memory.valid109 and each new equality guard's miss edge. All other business
shared blocks, branches, memory errors and final-output exits remain. Removing
the additional original dispatch-prefix blocks gives the exact same48-member
set, so the result is not produced by deleting business shared blocks.

SCC-members.txt lists every member; CYCLE-WITNESS.txt gives a40-node cycle
through both new seeds. The SCC retains40 outgoing edges. This is conditional
business-cycle topology, not an exitless/infinite loop, guaranteed guard hits,
or complete dispatcher removal. Core loop_contexts=0 counts another
construction property and does not negate this independently observed SCC.

## Remaining125 continuation and limits of the next change

Memory.valid125 still branches to original bb_4752. Its original fshl %1736
remains, and the outer-round continuation still uses the original dispatcher.

The separate instrumented LLVM-only diagnosis finds this seed eligible and
its selected root PHIs retained. Its first actual refusal is a generic
safeHistoricalBoolean/activeHistory cycle, %100 -> %EDI.1 -> %100, reached from
select condition %1752. The trace has25 Boolean nodes/depth20 and its
historical-mode flag is false. Correlated/STORE-expression/hint-mapping stages
are not reached. No local limit event occurs for this seed; the aggregate
budget_exhausted=1 is not a target-level explanation.

Instrumented output/report/parsed-before are byte-identical to the child
baseline. Production source and budgets are unchanged, with no target-native
execution. Initial diagnostic preparation used a stale probe-source path:
0 compile/0 probe calls before a plan existed. That failure remains in
guarded-intrinsic-seed-125-diagnostic-20261114-v1; completed evidence is in v2.

A separate read-only NEXT-DESIGN assessment predicts that removing only this
first cycle blocker would next fail the external-operand domination check:
%1922 in memory.valid141 does not dominate seed125. Ignoring the intrinsic
observation boundary would also reach depth33, beyond unchanged limit32.
These are static predictions, not a new pass execution or proof of a fix.
The first observed refusal remains the PHI cycle. There is no promise that
one small follow-up removes the remaining outer dispatcher, and this
checkpoint does not expand the model or guess historical values.

## Actual real replay and independent saved-data audit

The adapter reuses original B ELF/raw/driver/input bytes and exact parser/hint
bindings. It does no producer, disassembly, lifting or product-pass run.
Actual result:128 complete native/root-RET matches,64 Unicorn/Python
specification checks,384 comparator-only controls,0 skips/exclusions/
normalizations and0 new native-C comparisons.

A new saved-data checker verifies all128 expected/native66136-byte blobs
are byte-identical: header, event counts,512 register bytes,65536 Guest bytes
and eight Exit words including budget0. It also checks the original76 fields
and binds128 input copies to64 original B reference-input members. Every
control is exactly one RAX, Guest0x8000 byte or Exit.reason xor1 difference,
and reproduces the complete stored difference JSON.

These frames validate transformation semantics on the finite domain.
They cannot prove that the entire loop was de-flattened. The48-member SCC
retains separate static provenance.

## Evidence bindings

Child root: ~/vmc-runs/guarded-intrinsic-seeds-20261114/.

| Independent payload | Report SHA256 | Manifest SHA256 |
|---|---|---|
| saved-native-independent-audit-v1 |f577b381fa1dd6e747bd957cdd8666352e890a1fc42f9cbbed640bc297bb1ade|f3e6e8a0b69dabc49b6549773c90d8e5bf2cf126adda55625e908135ca07d53b|
| real-replay-independent-audit-v1 |5926dd411684328ca0d515538d915f92335454d326440b85ab9deaa5c798f83a|241af4083b78b030051ac4b6e6a42a90404bcdacb0feee0d5afcbc204767db73|
| real-v1-independent-audit (SCC) |574bd6703f9bf53b3f2f3b84c35f358c9d165ed8128ec63e4a60f4ff54d1e3f5|735f9cf222988dbd4238cf9e9dd34e3c2c5cfc3c5b28da2140614bff54c21ea7|

Each payload's full regular-member set, size and SHA matches on Mac and VM.
Each contains its checker/report; the SCC package contains complete members
and the witness. New checker filenames leave old tools/run data untouched.
The NEXT-DESIGN package is separate:
guarded-intrinsic-seed-125-next-design-20261114-v1, report b903fb2...,
six payloads/96751 logical bytes, included with the125 history in the child aggregate.

Original B archive:
9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359.
Parent production archive:
dc1c28580ce2223f8a5eb663651e70c6bcce205c407c5a8e0275bf661d5d4756.

Final aggregate:
`guarded-intrinsic-seeds-production-evidence-20261114-v1.tar.xz`, SHA256
00e16572c3e770bb78d2621e90aac5acc224c13a67b998f661de370e035f8644.
It contains1101 payloads plus its manifest,93209212 compressed bytes and
640299400 logical bytes. Both hosts independently verified the exact regular
member set, sizes and every payload SHA; no hardlink member is present.
The writer also checked all source file identities/hashes before and after.
It includes the actual production libraries/configs, every child run/failure,
independent audits,125 diagnosis/design and legacy-backend retirement records.
Resource symlinks are recorded rather than followed; original source pins and
parent archives bind their contents.

The source archive inside it binds commit289bb23021dc1b4e081757baffe36aad544478ca:
source-git-289bb23.tar.gz, SHA256
915946dc9ea2df69e7c6c54a66dac75b4a228884fe35a5fb6c90c6f3f5a63a0d.
The included source-docs-pre-seal directory is the explicitly named checkpoint
before this final seal paragraph; final documentation is this Git checkpoint.

Durable Mac directory:
`/Users/c/Desktop/deob/validation-artifacts/guarded-intrinsic-seeds-20261114/`.
The same archives remain under `/home/cc/vmc-runs/` on the VM.

Only after the complete dual-host archive verification, five redundant focused
clients were retired: prototype-v1 and prototype-v2 tests-before/tests-after,
and production-validation-v1/noz3/before-focused. Two same-UID process scans,
file identity/hash/nlink checks and preserved-neighbor checks preceded removal.
Objects, source, logs, current production clients/probes and all native streams
remain. Exact archived members restore any historical client before replay.
This releases87646208 allocated bytes; recorded free space becomes1083998208,
above the unchanged1GiB fresh gate. The operation's separate four-payload
archive is intrinsic-seed-prototype-retirement-20261114-v1.tar.gz, SHA256
83d2e791121325643f538f52543d9c6fd18d257e959a1a383df681dca18f6821,
5534 bytes; both hosts independently verify its full set and every hash.
The child root carries RESTORE-retired-focused-20261114.json. No failed
fixture/input/output or semantic comparison was discarded. Full tests retain1GiB start/64MiB running/180 seconds;
bounded probes/replays retain512MiB start/64MiB running/180 seconds.
No gate or timeout is relaxed. No instruction semantics, new single-instruction
campaign or whole69-input corpus campaign is claimed.
