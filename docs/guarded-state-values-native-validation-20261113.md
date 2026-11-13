# Guarded state values: independent native validation

This validates the opt-in `guarded-state-values` transformation on a separate,
sequential byte-array reference model. The original preregistration is
`guarded-state-values-native-plan-20261112.md`; the model, producer, driver and
prepare checks retain their original v1 bytes. Real Tigress loop closure and
whole-binary replay are separate requirements and are not established here.

## Frozen independent domain

Seven modes (`plain`, `table_before`, `table_after`, `freeze`, `flags`,
`duplicate`, `tail_call`) each retain154 input tuples, total1,078 distinct inputs.
The state boundary values include0/2/50/51/63/64/2^63/all-ones; both initial loop
limits0/3, checked memory boundaries519/919/920/1023/1024/65536, three table
contents and both separate/actually overlapping register/Guest layouts remain.
The unknown helper is only a declaration during analysis. Its separately linked
C body changes real registers, the state slot and table bytes. The seed STORE
and CALL execute once before either the new guard or original comparison.
Both directions, guard hits/misses and repeated loop iterations are exercised.
The state guard must consume the actual old SSA value, not reload changed memory.

The metadata predicts state2 and target43. Separate state51 and target777
recipes are normal equivalence tests of untrusted predictions. The copied path
retains the actual target LOAD and complete same-value switch fallback, including
all seven cases and the default. Duplicate seed successor slots retain both old
PHI incoming edges. Freeze and defined `add nuw nsw raw,0` remain present.

## Public API and precision gates

The original thirteen recipes pass separately in main and no-Z3: six normal
positive shapes, unchanged tail-CALL refusal, state51, target777, budget-one
resumption, malformed metadata, inner transaction budget and zero rewrites.
Resumption is exactly `[1,0,0]` and equals the normal final IR. The low inner
budget and refusal recipes preserve parsed-before bytes. Successful copies are
explicitly Incomplete, never full coverage. Module/Function/Argument/global and
unrelated metadata identities remain checked by the actual public driver.

The same driver compiles and runs against the frozen old J runtime, exits3 with
`unknown pass: guarded-state-values`, and retains the complete input and
identities. This is absent API evidence, not an old native wrong-result claim.

## Full state observations

Each build runs clang21 O0 and O2 with the unchanged180-second group limit:

| Per build | Count |
|---|---:|
| Normal full-state observations | 4,928 |
| Business returns / checked failures | 1,664 / 3,264 |
| Actual faulty-program observations | 1,848 |
| Independently specified fault detections | 838 |
| Comparator corruption controls | 54,208 |
| Saved gzip streams / complete frames | 44 / 6,776 |

Normal observations consist of2,156 original-before,1,848 changed ordinary,
308 unchanged tail-CALL and616 wrong-hint observations. Repeated stages,
optimization levels and builds are not extra distinct inputs.

The six faulty programs each match their own independently computed faulty
reference on every input. Whenever that reference differs from the ordinary
reference the comparator must reject it: duplicate seed CALL308, omitted seed
STORE154, forced wrong original direction180, stale header state56, post-write
target reload48 and wrong join PHI92. No template or failed input is removed.

Every frame compares return8, callcount8 and the complete73,728-byte arena,
including all actual registers, full64KiB Guest, descriptor/config, all seven
Exit fields and untouched bytes. Each normal frame has eleven specified byte
corruptions; the overlapping caller layout intentionally aliases some locations.
Saved-stream audit checks the exact stream set, every complete frame, EOF,
reference model and all specified fault detections. No normalization, skipped
cases or exclusions are used.

An additional before-only preregistration check runs O0/O2 without any candidate:
each1,078 normal,924 fault observations/419 specified detections and11,858
comparator controls,13 streams/2,002 frames. These duplicate baseline executions
are kept separate and are not added to the per-build candidate table.

## Identity and provenance

The original v1 source freeze JSON SHA-256 is
`28ef3a15906bf378ebb196609fbd1fac1b700d99487b7b56b7b9de35447fad2c`.
The public driver binary is
`acb0e8c7d75cc97c7a935ff842c3ae68f6eb66f4ce05b2965e0094fc7942b163`.

Main backend is
`a2f82bdd378c72e42b57b1e2fcb9e58317b106531d385492f93ab47230df5ef5`;
no-Z3 backend is
`b4fae0a37b04ab2e9a8bab3ec4841751076f3dbaa9d93a2cb1654b19da203a3d`.
Each launch records source, inputs, actual runtime and prepared output hashes
before and after, exact commands, ldd loader paths, all finite timeout limits,
compile/native/verification logs and exit codes. No production source or runtime
is altered by the oracle.

The final archive, dual-host verification and exact cross-build IR comparison
are recorded by the completion receipt delivered with this report. Native
success here establishes the fixture domain; actual full real-loop completion
requires its independent structural audit, full-state replay and repeated-loop
observer.
