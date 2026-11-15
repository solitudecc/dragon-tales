# Loop proof branch milestone — 2026-11-15

This is the historical implementation checkpoint. Subsequent independent
native results are in [the large-sample validation report](loop-image-large-native-20261115.md);
the pending counts below are retained as the status at this checkpoint.

The large Flatten-O2 B view now completes the opt-in Loop proof under the
unchanged1M work limit. Both main and no-Z3 produce the exact same complete IR.
This is an implementation checkpoint on the feature branch: after-native
validation and the later readability cleanup are still pending, so it is not
yet eligible for merging into main.

## Result and scope

Using the same physical real-input producer, original ELF/raw IR/frame,
request and target set, old ef35 refuses B at work1000000 without changing its
module. New main d446 and no-Z3 d98 complete B at work985771, with68 protected
STORE checks and all73 surviving lookup cutpoints retained. The selected view
has733 blocks, one entry switch and zero cyclic switches. Ordinary1000/1010
function bodies, existing objects/arguments, globals and named metadata pass
their preservation checks. A, which lacks the output-register guard, remains
a conservative refusal. These are conditional GuardedEntry views, not an
unconditional specialization for arbitrary caller states.

The common after IR SHA is
`714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`.
The source Image is
`e39195a44337b3f61231e654d4468e470f0ff6149d814e19378279884c234c81`;
Path implementation/header are `c12b6702…` / `02e9783c…`.
Main library:
`d446226f9ff402bf1e096e197993aa8f2301d2ab81af3c573b50ae13ed2e0ed4`.
No-Z3 library:
`d98b8e934cbbd861c3353e092b891e7a5f19534ad42aa0d8788581bb56762bec`.

## Changes

Known low-discarding masks can request one stronger memory-range proof only
after a valid coarse write interval ambiguously overlaps a protected span.
Nested queries retain their own RAII modes; independent anchors prohibit this
demand and all provisional observations. Both operands and every original
hazard remain checked. Bounded, legal nonsingleton logical shift counts now
produce conservative intervals; wrapping left shifts retain Full.

Repeated same-SSA literal/H_C leaves pay one scalar node per outer query.
Repeated complete same-root opaque shapes reuse their already paid footprint,
while still paying each root use and actual helper work. These are explicit
Loop accounting policies, not changes to the old opaque API's full-footprint
contract. Nested attempts never reset shared counters or refund work.

After joint closure, the consumer applies the already proved ConstantInt
overlay in frozen original-instruction order, instead of starting another
whole-function folding fixed point. It changes uses and retains every original
instruction, effect and CFG node. A completely fresh empty-H memory recheck
still follows. The recovery scan's exact instruction/marker inventory remains
valid across these RAUW-only operations. A physical effect index retains all
calls, reads, stores, fences and other writes, with identity/type checks and
the same original effect classifier. It supplies no numerical or alias facts.

The explicit `PathRangeTraversal::BranchConditions` overload avoids the
instruction-list walk performed solely by `BasicBlock::size()` for legacy
accounting. It still rebuilds the DominatorTree, witnesses and all branch
facts from scratch. The old three-argument constructor symbol and historical
WholeFunction accounting remain. No numerical cache crosses SSA mutation.
Default pipelines, old Image APIs and C/Python ABI layouts are unchanged.

## Executed validation

Fourteen focused groups pass in both builds. New/extended groups include
mask2/692, variable shift2/763, constant consumption2/24011 and branch
traversal6/349. The unchanged legacy Image group remains14/2273.

The complete registered suites ran sequentially on reference CPU4–7 / `-j4`,
with the unchanged900s/512MiB/64MiB outer guard and original per-test limits:

| Build | Passed | Failed/skipped | Time |
| --- | ---: | ---: | ---: |
| Main | 1317/1317 | 0/0 | 211.970s |
| No-Z3 | 1171/1171 | 0/0 | 168.116s |

Main result SHA:
`a4c172c22dd6eb3ca2a3433c45f25dab509e54ebe8892ada67891b4101bd274d`.
No-Z3 result SHA:
`3c9ff1d0d3e08be8e3f01a2a88d1528306e5dff545583610a6bdcd42426639d8`.
Source and runtime stat/ctime/SHA pins remain equal. No check, registered test
limit, exclusion or normalization was loosened for these results.

Same-client controls establish mask old2/542 with five positive failures
versus new2/692, shift old2/553 with seven failures versus new2/763, and the
final consumption fixture old2/13991 with one1M precision refusal versus
new2/24011. The original consumption-fixture naming/sinking calibration
failures remain preserved; the final768-trace fixture is a separately recorded
new scenario. Three exact-source internal accounting runs preserve the actual
b6 four-failure, df one-failure and17 zero-failure outcomes. They are not Guest
executions. The new Path overload cannot compile/link on the old API; that is
API absence, not old wrong execution. Its real same-public-API gain is the
ef35/d446 Loop result above.

## Publication checker calibration

The first successful real rewrite encountered the old producer's exact
module-function-set gate. The sole added function is the canonical declaration
`llvm.fshl.i32`, with five direct users in the selected view. Importing missing
fshl/fshr declarations was already explicitly supported by the original
commit code in e952100. No other existing function changed.

The original producer failure remains a failure. A new checker additionally
classifies only this exact typed, canonical declaration after a successful
closed result, with unchanged existing-function/argument/global/metadata
checks. Fourteen valid-IR controls pass, including the old exact-set checker
rejecting the same legitimate import and the new checker rejecting unrelated
declarations/definitions or ordinary-function mutations. Its first LLVM21
API-signature compile error is retained separately with zero analysis calls.
API-return stats are now journaled before postcondition checking; an observed
return alone is not the final eligibility gate.

## Remaining validation

The earlier original-IR calibration has32 complete native matches and96
comparator controls against16 saved Unicorn reference frames. Those counts
are not after-native validation and are not recounted here. The adapter's
synthetic binding/observer checks are also not Guest executions. The actual
closed B artifact must still pass complete state comparisons and observed
accepted/fallback checks. No new instruction/Unicorn/whole69-input corpus
campaign is claimed. No independent deobfuscated executable is delivered.

The seven main validation roots, including original failures, are independently
dual-host verified by `loop-image-final-small-verification-20261115-v1/`,
certificate SHA
`5ef6e9c60cb5045c999842c2275f4411af297529425ce2078dc5851190105d65`.
The later same-client ef35 control is separate, result SHA
`86e300354e0a9973e5cf8dc0daf48de294d211f67fedaed22d1e46186ca974e5`.
The main/no-Z3 six complete products and25 source inputs are independently
frozen under `loop-image-final-branch-sdks-20261115-v1/`, manifest
`fdd8ddddbe64684ccaedb5f09eb1179b0183273dfc5b0efd49618c58c76ae932`.
Large compression/archive transfer and final native evidence are subsequent
steps; no old evidence has been deleted.
