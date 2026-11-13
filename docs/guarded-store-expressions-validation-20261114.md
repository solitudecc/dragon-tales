# Guarded STORE expressions: validation checkpoint (2026-11-14)

Implementation: `08ddbe4`, based on `ecdb4de`. Source
`GuardedStateCorridors.cpp` SHA256
`9bd053ed4530792be5ff47afffb7d6e0d73f55084191c31d46f882cf6af75440`.
The step is opt-in, uses independent untrusted prediction metadata, and leaves
actual operations, memory checks, reads and complete same-read fallbacks in IR.
It does not change the default recipe or produce a cleaned executable.

## Actual production builds and tests

Original production compile/link commands were replayed with RAM outputs,
recompiling only the two changed translation units. Every reused source/object
and static LLVM dependency was pinned before and after. Owned source, objects
and binaries were unchanged. Main/no-Z3 were linked sequentially.

| Item | Main | No Z3 |
|---|---:|---:|
| New cases against actual shared backend | 10 / 777 assertions | 10 / 777 assertions |
| Historical full CTest | 1163 / 1163 | 1028 / 1028 |
| Python registry/default/options checks | pass | pass |

New cases are run separately from the cached historical full test executables;
the historical CTest counts are not inflated to include them. Main backend SHA
`6debd1c17686fdf1188b0bab9c783f9f8e2437fbe1d121ba94065ea654d4a59f`;
no-Z3 `d816dca2c5da1ac210c451be98b2b4eba14f08233729794b1e7d704adaeec882`.

The first full main run caught a missing update to the exact Python builtin
set:1162 passed/1 failed. The candidate test adds precisely the new name and
its option checks; equality remains exact. RAM test-source resource bindings
initially missed headers/helper fixtures; those setup failures remain saved.
Final full runs retain every original test/name/timeout and assertion, with
only runtime paths and this updated test-source path rebound. No check was
weakened. Full campaigns retain the1GiB start/64MiB running/180-second guards.

## Negative regression on real origin/main

`origin/main c9cd78198fc002465f4c22f0d3335e2ffd7d114a` was reconstructed from147
Git-blob-exact archived production source files. Of59 development objects,
two origin-absent TUs were removed, five TUs recompiled (including unchanged
bodies affected by changed private headers), and52 reused only after their
complete valid Ninja source/header dependency closures matched origin bytes.
All five new dependency files contain only old-snapshot project headers.
All1223 input hashes/stamps remain unchanged.

Old backend SHA `fb0813eb0899526581058d993b275c9f929af82a511b416ae5fd39cee22db9e8`.
The same new test object compiles/links and runs once:10 cases fail at builtin
acceptance,21 assertions/11 passed/10 failed, exit10. This establishes feature
absence on old code, not wrong old execution. Earlier intermediate prototype
runs also retain the nested-poison and partial-target-mapping before failures.

## Independent bounded native domain

Independent Python arithmetic/state reference, original and transformed native
LLVM are compared using return plus every byte of the arena. There are20
fixtures (i32/i64 by ten modes),64 input keys, host O0/O2 and ordinary/wrong
prediction manifests. The same emitted40 transformed IR files are byte-equal
between main and no-Z3; this is not another native campaign.

Four actual runs each report2560 before/after full-state matches and3840
comparator controls. Each also executes1024 fault observations,868 of which
must differ and are detected. Total actual calls:10240 normal before/after
matches,4096 fault observations,3472 specified differences. Eight gzip streams
were independently audited for full domains, exact headers/CRC/EOF and precise
fault-byte sets. No exclusions or normalization.

These are repeated runs over the same domain:64 keys project to62 effective
i32 initial arenas and64 i64 arenas. Per-run fault effective-unique arenas are
1008 with853 specified-unique detections. Do not advertise repeated counts as
new business input coverage. i8/i16 have construction/structural regressions;
this native campaign's state widths are32/64. A widened-STORE fault is not
claimed covered by the wrong-width fixture. Original emit/runner/audit setup
failures remain retained; corrected tooling does not change inputs/reference.

## Four frozen real ELFs

Reuse the exact B ELF/raw/driver/input bytes under archive SHA
`9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359`.
The adapter performs no producer, lift or product-pass run. A read-only LLVM
binder proves archived raw plus the exact hint string/recorded parser identifier
prints byte-identically to each frozen parsed-before product.

The four real after modules are freshly compiled at host O0/O2 and compared
against each image's original Unicorn execution (1M Guest instruction limit):
128 complete matches/root returns,64 Unicorn/Python specification checks,
384 comparator-only corruption controls, zero skips/exclusions/normalizations.
There are no new native-C comparisons. Independent saved-data audit confirms
all128 expected/native66136-byte blobs are byte-identical (header, event counts,
512 register bytes,65536 Guest bytes and eight Exit words including budget0),
as well as all76 modelled fields. Inputs bind exactly to the original64 B
reference-input members. All384 controls have exactly one specified corruption.

| Frozen image | Conditional copies | Loop contexts | Full loop removal |
|---|---:|---:|---|
| manual C-O0 | 0 | 0 | not achieved |
| manual C-O2 | 4 | 0 | not achieved |
| flatten-switch C-O0 | 16 | 0 | not achieved |
| flatten-switch C-O2 | 25 | 0 | not achieved |

The45 changed source edges retain actual LOAD/same-observation fallback and
complete original case/default maps. Some C-O2 edges have identifiable business
state updates; some are already constant branch continuations. Copy counts are
not recovered business-operation counts. Excluding central dispatch/misses,
C-O2 conditional fast views still have no closed business SCC; C-O0 SCC scope
was not independently selected. The partial mapping regression improves its
synthetic case but does not increase these four frozen real counters.

A concrete next blocker is `manual C-O2 memory.valid125`: the original seed
contains `call @llvm.fshl.i32`, followed by an opaque-condition state select
enc0/enc6. Current call-free seed admission refuses even this mathematical
intrinsic, although the seed is not copied. Treat seed admission separately
from copied-corridor admission; preserve the actual operation and retain all
unknown-call/memory barriers. This next change is a separate work item.

## Evidence and scope

Real replay payloads469 are dual-host set/size/SHA verified in
`guarded-store-expression-real-replay-20261114-v2.tar.xz`, SHA
`6cb20e304eb1ba21e1127a3e26be390764d23d956563a8ffc623f681e190fc8e`.
After independent owners closed reads, only archived expanded non-JSON replay
products and obsolete interposing executables were retired. JSON results,
original input/raw/after modules outside that replay namespace and all failure
records remain; restoration uses exact archive members.

Production/runtime and aggregate results are sealed on both hosts in
`guarded-store-expression-production-evidence-20261114-v1.tar.xz`, SHA
`dc1c28580ce2223f8a5eb663651e70c6bcce205c407c5a8e0275bf661d5d4756`:
142189844 compressed bytes,1114 payloads plus manifest,797781032 logical bytes.
Every regular member name/size/SHA matches independently on both hosts. It
includes actual new/old shared backends, code and input/dependency closures,
all full/focused/native receipts, original setup failures and independent audits.
Read-only resource symlinks are recorded rather than followed; original Git
source and pinned resource paths provide their bindings.
No new machine instruction semantics, single-instruction campaign or full69-input
corpus campaign is claimed. The opt-in LLVM component's success is not complete
loop de-flattening, unconditional table knowledge or a whole-program memory model.
