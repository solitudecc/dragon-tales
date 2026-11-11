# Historical singleton and selected-PHI repair: real probes (2026-11-12)

I2 was run on the original four D37 inputs with the exact same 47-step recipe used by H2 and I1. Only the runtime changes; K is absent. These are structural probes, not generated-native executions, formal full-state replay or observed loop traversal. The separate independent I1 historical precision failure remains preserved; this probe does not waive that gate or substitute for the I2 native oracle.

## Frozen execution

Pre-execution contract SHA: `6b4cc884e158c8fe90d4f713e6c186af526ca58ffc2ed541daa02bb5c1618b62`. The four inputs were extracted individually from the exact I1 archive `87d51318ba31384667346e2814bb30db9e16261899bfdc4c87076c0d856cf72c` and checked against its manifest. The producer is the unchanged public-API binary `c484c8f54082129b66dee71270c5576afd4e36c1eada8e861b90d794400ae4f3`.

The recipe remains D37 → F1 → H1 → one explicit H-resume → tiny5 → constant1 → E1. F and each H keep their existing defaults: candidates64, rewrites32, blocks128, copied instructions16384. Every stage has one iteration. There is no new resume, no implicit fixed point, no increased GuestSSA/dispatch budget, and no new input assumption.

The runtime backend is `2a284c9593dceb1241e92730e4811f2aff9cd1121f5bee72f2d1f2abd2abfb96`; shim is `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`, from `guarded-history-singleton-20261112/runtime-main-v1`. Production provenance is `4aa5916` plus `c58e7b` patch. Complete frozen controller/core source copies and hashes (prefixes `8f877444` / `16d1949`) are included in `production-source/` and the predeclared file set. The producer never reads current production source or a mutable build library. Actual `ldd`, smoke result and before/after pins are retained.

All four calls exit zero and preserve all pins, LLVM verification and guarded-entry metadata. Elapsed times are 13.763, 7.393, 72.162 and 33.105 seconds. Every case starts above the unchanged 512 MiB threshold; the timeout remains 180 seconds. No failed case was excluded or retried under a looser budget.

## Results relative to I1

| Case | F | H first | H resume | Strict conditional closure | Final IR equal to I1 |
|---|---:|---:|---:|---|---|
| f-cff O0 | 0/0 | 0/0 | 0/0 | true | byte-identical |
| f-cff O2 | 20/20 | 0/0 | 0/0 | true | byte-identical |
| f-combo O0 | 2/2 | 13/22 | 5/9 | false | changed |
| f-combo O2 | 26/45 | 7/8 | 0/1 | false | byte-identical |

The closure algorithm and controls are unchanged. It identifies all central-family switches by their complete cases and actual Exit source PC, including renamed entry-prefix defaults; it retains ordinary conditions and all actual natural-business cases. Only verified same-read guard failure edges are excluded from the fast-success projection. Full fallback paths remain in the module. Conditional closure stays 2/4, and no actual runtime traversal is asserted here.

Combo O0's F pass now succeeds on both original candidates rather than 1/2: it copies 14 blocks/110 instructions and retains a budget event from its search, so its report remains incomplete. The newly rewritten original slot is `memory.valid125.entry.prefix:0`, formerly to `bb_68262.entry.prefix`, now to its second transition copy. H first subsequently performs 13/22 attempts (120 blocks/1864 instructions, nine refusals and ten budget events), versus I1's 14/14. Exact original-slot comparison retains thirteen H rewrites; `memory.valid119.entry.prefix:0` is no longer rewritten by H, while its downstream `memory.valid125.entry.prefix:0` has already been rewritten by F. This ordering change is reported explicitly; a per-pass count alone proves neither a loss nor a gain in state coverage. The H resume remains 5/9 (51 blocks/778 instructions, four refusals and five budget events).

The nearest combo O0 central-reentry witness is unchanged: the dynamic initialization-counter path through `memory.valid119`, `memory.valid123/125/127/129` and the original dispatcher. It is not the repaired finite-state-50 alternate path. Combo O2 remains byte-identical to I1, including its call/reloaded-state path via `memory.valid45` → `bb_66032` → original `bb_66038`/`memory.valid49`; all its earlier refusals and budgets remain. Last-message text is not used to explain every rejected candidate. No alias/callee guarantee or arbitrary finite bound is inferred.

## Evidence and limits

The full four-case raw/stage evidence, inventories, input/source/runtime pins, commands, reports, original-slot delta, unchanged audit sources and this report are archived as `guarded-history-singleton-real-20261112/probes-v1.tar.gz` on both hosts. Adjacent verification receipts provide the compressed SHA, exact file set, sizes and every payload hash. This stage has zero C/LLVM native comparisons and zero new semantic-control executions. A combined I2+K+J pipeline requires a separately frozen recipe and its own formal execution and loop observation; these results cannot be added together as if that composition had already run.
