# Atomic dispatch corridor batches

`threadDispatchCorridorBatch` is an opt-in C++ custom-pass helper. A large
flattened loop can have many edges entering the same checked dispatcher.
The legacy helper copies and repairs each edge separately under its original
64-block ceiling. The new helper freezes and preflights an entire caller-supplied
batch against one unchanged function, then repairs each shared original SSA
definition once using all copied versions. It does not identify a table,
prove table contents, or automatically run in the default optimizer.

The new `DispatchCorridorBatchOptions` bounds candidates, rewrites, total copied
blocks/instructions, shared SSA repair and work separately. Limits are shared
across the whole invocation; no per-edge reset expands them. Existing public
options, statistics layout, ABI, legacy defaults and implementation path stay
unchanged. Original source/successor-slot identities must match; candidate
sources cannot occur inside any copied path. Any unsupported candidate,
exhaustion, unsafe repair, invalid trial or failed commit refuses the entire
batch and preserves the original Module/Function/Argument objects and contents.

A selector may have one fully proved defined state, or two states from one
bounded boolean observation. In batch mode, integer flags are checked over the
complete admitted domain. The actual data, memory addresses, flags, side effects
and errors are not replaced with predicted values. A two-state comparison stays
at the original switch's control-use point. When an earlier pure bounds head
has a self edge, every proved outcome must select continuation. For two states,
the copied head retains the actual original conditional branch at that point,
with both successor slots continuing. It does not introduce an early equality,
freeze, noundef or an assumption. See the separate undef notes for this precise
control-use argument; these finite outcomes are not exported as general data
range facts.

History bridges capture old observations on actual selected edges. Shared
SSAUpdater repairs receive the original and all copied available versions;
unversioned original instructions cannot be introduced by a copied available
value. PHIs use every actual successor slot, including duplicates. Batch cleanup
only removes graph-unreachable blocks and incoming slots; it does not fold
reachable terminators or invent undef/poison for an unrepaired definition.

The regression history retains the initial two implementation failures: LLVM's
legacy unreachable-block helper folded the required duplicate conditional, and
an i1 flag cast was admitted as an opaque observation. Final tests fail four
cases / 44 assertions on that exact intermediate SDK and pass 13 cases / 5,949
assertions on the fixed SDK. The old legacy cases pass 13 / 562 in both builds.
The original `07325bd` SDK has no new Batch symbol, so final API tests cannot
link against it; absence of this API is not an old runtime semantic defect.
Additional test-construction errors are retained separately and were corrected
without changing the source model, original implementation regressions, or
validation thresholds. Full CTest: main 1,254 / 1,254; no-Z3 1,108 / 1,108,
with the exact unchanged 146-case build-option difference.

Independent C/Python/raw-LLVM validation and the installed-API native results
are documented in `dispatch-corridor-batch-native-validation-20261115.md`.
Macro-enabled body-transaction fault hooks are a separate structural client,
not installed-SDK native execution: each backend passes batch 14 / 6,009 and
legacy 14 / 596, with both forced commit-failure phases and exact object/content
restoration independently audited. Independent validation adds 152 complete
state matches and 4,104 comparator controls; 54 deliberate-fault observations
detect all 38 specified errors. This includes three new late-error frames after
one or 71 dispatches. One supplemental audit-report construction failure and
its 12 duplicate observations are retained separately and not recounted.

On the fixed large Flatten input, an explicitly hypothetical table CFG allows
73 / 73 original incoming edges to be threaded in 600,320 work units, with
292 copied blocks and 1,749 repaired PHIs; the resulting IR verifies. This
is conditional diagnosis: table preservation is unproved, this IR is not a
publishable deobfuscated view, and this probe performs no native execution.
A separate actual public Image A/B replay on the new SDK produces exactly
the prior complete reports and output IR, preserving the real refusal. The
ordinary guarded Image context still needs a complete joint table/cell
memory proof before such a body can be committed. No complete real loop
removal, immutable image, noalias rule, VM support or instruction/corpus
campaign is claimed here.
