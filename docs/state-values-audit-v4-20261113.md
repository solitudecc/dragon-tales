# State-value and small-switch audit v4 (2026-11-13)

The v3 small-switch auditor could mistake a previous-iteration value for a
current LOAD. Its recursive PHI resolver reused the fast predecessor at every
level. A valid SSA example is:

```
header:
  %history = phi i64 [ 43, %entry ], [ %fresh, %fast ], [ %selector, %fallback ]
  br label %fast
fast:
  %fresh = load i64, ptr %table, align 1
  switch i64 %fresh, label %fallback [ i64 43, label %done
                                      i64 44, label %header ]
fallback:
  %selector = phi i64 [ %history, %fast ]
  switch i64 %selector, label %done [ i64 43, label %done
                                     i64 44, label %header ]
```

Resolving `%selector -> %history -> %fresh` with the same `fast` label silently
changes which execution produced the value. The old frozen v3 `sets()` actually
accepted this verifier-valid inventory as one same-read set. V4 rejects it.
This is an auditor defect; no production wrong-result claim is inferred from
this synthetic example.

The new versioned files leave the old v2/v3 sources and results intact:

- `guarded_state_values_manifest_audit_v4.py` checks every real terminator slot
  against its instruction operands and checks PHI incoming multiplicity against
  the full unprojected CFG. Scalar same-value guard records are recomputed from
  actual EQ/LOAD/switch operands, without trusting generated markers.
- Every state guard must have the actual old state SSA, the original ICmp, one
  new i1 EQ and one i1 freeze consumed by the branch. Original seed effects stay
  once. The chosen original direction is matched to the actual copied path,
  including instruction flags/operands, PHI edge selection, each unselected
  side edge and each new exit PHI value. Historical snapshots and unsupported
  clone shapes are conservatively refused.
- All guard witnesses must succeed before a multiseed predecessor-to-fallback
  map is created. Original PHI pairs are then checked with the complete map.
  Thus two original seeds feeding one PHI may both be relabeled without
  inventing or dropping an original incoming edge.
- A fallback selector may unwrap one PHI only when that PHI is physically in
  this fallback block. All inputs from the actual fast predecessor must be the
  same current LOAD directly. Cross-block or nested historical PHIs are refused.
  This rule applies at every original full-switch/default fallback-chain layer.
- `guarded_state_values_set_cycles_v4_audit.py` applies this independent proof
  before projecting state/target failure edges. All validated small-switch
  cases remain in the graph. It writes the distinct
  `STATE-VALUES-SET-CYCLES-V4-REVIEW.json` and refuses to overwrite an older run.
  The old scalar-EQ-only metric and its actual failures remain reported.

## Focused validation

Five small LLVM modules were parsed and verified using an independent
inventory-only tool linked to the frozen LLVM 21/Dragon environment. The tool
runs no deobfuscation passes and executes no target/native code. The fixtures
and their exact inventory snapshots are committed under
`tests/fixtures/state-values-audit-v4/`, with byte/hash pins.

`state_values_audit_v4_selftest.py` passes on both macOS and the Ubuntu VM:
5 positives and 36 negative controls. Positives include two simultaneously
validated seeds sharing an original PHI, the same case with real duplicate CFG
slots and PHI inputs, reversed hint ordering, and duplicate identical current
fallback inputs. Negatives include the actual historical-PHI misacceptance,
foreign/nested PHIs, disagreeing duplicates, state rereads, missing/double
freeze, changed old predicate/direction, skipped selected roots, changed LOAD
address/alignment, wrong original/new PHI values, omitted CFG edges, corrupted
slot IDs and incomplete/duplicate/foreign-function hint sets.

The two preserved old one-hint combo inventories were also checked read-only:
the exact selected clone path and new exit-PHI mappings pass (4 blocks at O0,
2 at O2); 19/41 old small-switch sets satisfy the single-edge resolver. Those
old state branches lack the new freeze and are not accepted as new guard
correctness, fixed-runtime, two-hint or final loop evidence. The proposed old
v3 two-hint combo case was never executed and is not counted as a positive.

An initial selftest mutated a copied instruction dictionary instead of its
underlying inventory record; the changed-predicate control exposed this test
construction error. Switching to the actual record then exposed a stale
`pop('block')` in another control. Both preparation failures are retained; the
final tests change the intended inventory fields and retain the same rejection
requirements. No comparison, field, control or accepted-domain condition was
relaxed.

This stage changes validation scripts only. It does not run the new fixed V
candidate, a full 57/64-pass recipe, a new native state oracle, Unicorn, or the
69-input corpus. Conditional structural closure remains distinct from path
feasibility, proof every guard succeeds, and observed repeated loop execution.
The original 64-pass timeouts and earlier false metrics remain failures. The
new fixed-runtime four-module V-only diagnosis and final full-state replay must
be recorded separately.

Evidence namespace: `~/vmc-runs/state-values-audit-v4-20261113/`; its archive
retains the old v3 misacceptance, verified LLVM/JSON fixtures, original and final
audit sources, read-only old-structure checks and test-construction failures.
