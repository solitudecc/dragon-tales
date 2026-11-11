# Guarded target sets implementation milestone (2026-11-12)

Baseline `5cac879`. The opt-in `guarded-target-sets` component consumes separate
untrusted hints and builds an actual-LOAD small switch with a full same-value
fallback. It never adds entry memory facts. The old single-candidate request,
layout and compare/branch path are retained. See [the contract](llvm-guarded-target-sets.md).

The final six-case public regression compiles against frozen old D runtimes:
each main/no-Z3 has 6 cases / 20 assertions, with five failures because the new
builtin is absent and the legacy single-guard case passing. This is capability
absence, not an old execution mismatch. Final new related tests pass each
28 cases / 927 assertions. Full CTest passes main1093/1093 (123.97s),
no-Z3 958/958 (110.02s), including the Python exact builtin inventory, explicit
default exclusion, unavailable-without-hints and hard-limit checks.

The independent oracle agent also executed the original single-core/controller
driver on exactly the old input paths: all 24 main output IR files match D
byte for byte; D's frozen baseline matches the earlier archived originals.
This adds no native observations. New target-set model and real complete-root
validation have now completed; their final evidence is recorded below. Actual
fast-loop observation remains part of the overall completion gate.

| Frozen build | Backend SHA256 | Shim SHA256 |
|---|---|---|
| main-v1 | c8bb7646ab64504576de536009d577b346e38beaffe16cc8e8067e29ff4ed4a1 | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |
| noZ3-v1 | be9589ab36770bb856bcf27254d2338aee41ead3549363560104edd7a8669726 | 061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0 |

Exact baseline+patch, source pins, original old failures, focused/full results
and runtimes are under `~/vmc-runs/guarded-target-sets-20261112/`.

Four independent probes use the actual final D IR, not a projected CFG.
Each produces one six-case business switch backed by the same actual LOAD and
full fallback. The cff O0 conditional fast graph has a cyclic SCC and no central
selector reentry; the other three strict gates remain false. This is structural
evidence only. Full native comparison and actual fast-loop observations are
required before claiming even the first sample complete.

Two verifier issues are retained separately from production results: the first
malformed-hint negative compared function text before metadata insertion and
failed solely on LLVM metadata slot renumbering; v2 instead checks complete IR
after insertion and proves no mutation. The original classifier required a
same-block LOAD, while D removed now-dead original constant switches; the new
anchor accepts only independently verified same-read guarded fallbacks. It does
not infer edges from names or file hints. Missing/invalid hints remain
Unavailable/Incomplete and preserve the full post-metadata input byte for byte.

No instruction/IGNIL semantics, Graph target publication, default pipeline,
heap/callee assumptions or standalone binary output are changed. No new full
69-input corpus or instruction campaign is claimed. Subsequent work must recover
SSA PHI/select state paths for the three remaining modules.

## Final component validation

Implementation `a654e15`. Main/no-Z3 each complete7680 full-state observations,
including3840 business returns and3840 declared size failures. Before2560,
actually transformed after4096, unchanged CALL after1024; the same1280 inputs
repeat across optimizations/builds and do not count as new independent cases.
Each build has2560 faulty-IR observations with944 specified faults detected and
84480 comparator controls. All40 saved streams/10240 frames are reaudited per
build without adding executions. Normal six-target and stale{777} predictions
both preserve semantics. Legacy24 single-target outputs match byte for byte in
both builds. See [native validation](guarded-target-set-native-validation-20261112.md).

The completed real retry gives C16/LLVM16 full-state matches and144/144 specified
controls, unchanged original inputs/Graph/source and exact runtime/harness pins.
The actual E-exported raw/bound modules are checked, not merely the earlier
in-memory D view. Final conditional closure is true only for cffO0; actual
fast-loop execution observation is still outstanding. Other three modules retain
central reentry. See [real validation](guarded-target-sets-real-validation-20261112.md).

The first real run stopped at the fixed64MiB disk reserve (return-15), without a
final summary. Its inherited summary is not a complete E result. Preserve that
failed package. The independent retry changes only directory and startup reserve
to750MiB, preserving the64MiB running stop, inputs, budgets and comparisons.

All following archives have exact member sets and every payload SHA/size checked
on both hosts and again independently by root. Mac root is
`/Users/c/Desktop/deob/validation-artifacts/`, VM root `~/vmc-runs/`:

| Relative archive | SHA256 | Payload files |
|---|---|---:|
| guarded-target-sets-20261112/guarded-target-sets-metadata-v1.tar.gz | 079a2365151dafc23884de61c4652bb19ed87a471b543e0b15b220c7e5b25b8f | 54 |
| guarded-target-set-native-20261112.tar.gz | 0bb9f7cfc6b435085b26a6cbb6525f509cbe663dfb320261237759c49ce86a40 | 830 |
| guarded-target-sets-real-20261112/probes.tar.gz | 8fc79363ce3d53065fa774d7ea5ef582d097178f7b314d05c2b738077e965dde | 150 |
| guarded-target-sets-full-real-20261112/recipe-B-failed.tar.gz | ff9a1a55ce430068bc742badc400bc4a0aaaca35a8df95b6c82f13ebc750fef7 | 460 |
| guarded-target-sets-full-real-20261112/recipe-B-v2.tar.gz | 83cc318d5df9904dc71bcccb8e43b7dbff53c429fb963318edd4f6b28dde14c4 | 484 |

After verification, redundant expanded C/D/failed-E candidates were retired with
per-file receipts and exact restore commands. Separately,27 already verified old
compressed archive/runtime copies were relocated from VM to Mac, freeing945577984B;
no live input or expanded source dependency was removed by that relocation.
`ARCHIVES-ON-MAC-20261112.json` and adjacent per-file notes identify each exact
copy and restoration hash. Newly frozen E runtimes remain unchanged.

This completes validation of this component, not the overall four-module loop
goal. Next: explicit SSA-state corridor recovery, then final real closure and
fast-loop observations with all unknown/failed-check fallbacks retained.
