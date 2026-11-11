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
validation are still ongoing at this implementation milestone.

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
