# Correlated Boolean state candidates: real probes (2026-11-12)

These are four structural probes of I v1 (`90f24b2`), not formal native validation or a waiver of the independent historical-candidate precision failure. The latter still expected three copies and observed two; its failed gate remains recorded by the independent oracle owner. This probe executes no generated native program and does not establish runtime loop traversal.

## Frozen inputs and execution

The input is the same four D37 modules used by H2, extracted individually from the complete H2-47 archive (`659840e81d3c8ec2361403e0e3c73cdd2f0c5c75d240f1c7f825089d76f9ef9e`) and checked against its per-file manifest. The same public-API producer binary (`c484c8f54082129b66dee71270c5576afd4e36c1eada8e861b90d794400ae4f3`) runs the unchanged exact47 recipe: D37 → F1 → H1 → H-resume1 → tiny5 → constant1 → E1. F and each H retain the default 64 candidates, 32 rewrites, 128 blocks and 16384 copied instructions. All prior GuestSSA, dispatch and input contracts remain unchanged; no third H retry occurs.

Only the dynamically loaded runtime changes. Its backend SHA is `e09aebc9ec241a5e22bb162d04958e5bda66aab15b73b9c47fbcd956e286a934`; shim SHA is `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`. Actual `ldd` resolution and a smoke run are saved. Source provenance is independently checked against metadata archive `8a3802c6cd568372157a6410f9b20117c3d58687c881db245faa28b0df040a36`, member `metadata-v1/frozen-sources/lib/GuardedStateCorridors.cpp`, SHA `83ad02c4481e02a97608dabb03fd90aa4cc88bd3230734f13dfeec6bebc9d7c1`. It does not read the mutable production build tree for evidence.

Pre-execution contract SHA: `f2d8e697334abd47d00600dab1a86bc5327614d391a0c2368ccd3b7311aca13b`. Every case requires at least 512 MiB free at start, with a 180-second timeout. All four exited zero; all input, producer, runtime, runner and source-provenance pins are unchanged after execution. Each stage's exact name/count and verify/entry-manifest preservation checks passed.

## Actual results

| Case | F rewrites/attempts | H first | H resume | Strict conditional closure | Final IR byte-equal to H2 |
|---|---:|---:|---:|---|---|
| f-cff O0 | 0/0 | 0/0 | 0/0 | true | yes |
| f-cff O2 | 20/20 | 0/0 | 0/0 | true | no |
| f-combo O0 | 1/2 | 14/14 | 5/9 | false | yes |
| f-combo O2 | 26/45 | 7/8 | 0/1 | false | no |

The unchanged family audit identifies central dispatchers by the full case set and actual Exit source PC, including renamed entry-prefix default blocks. It excludes only validated same-read target-guard failure edges. All ordinary branch alternatives and all cases in the actual natural-business small switch remain. Complete original fallback switches and guards remain in the IR. Neither the count of guards nor the number of copied contexts is treated as a count of recovered states.

For cff O2, F grows from 18 to 20 successful original-slot rewrites, using 75 blocks/2638 instructions with complete=1 and budget=0. The two additional original slots are `bb_66175:0` and `memory.valid71:0`, formerly targeting `bb_65712.bb_65718_crit_edge`; they now target its `transition1` and `transition12` copies. The second is the previous nearest central-reentry witness. The strict final graph contains 69 central-family switches, but none is reachable on the audited fast-success domain. This is a real structural improvement, not elimination of the fallback program. Formal full-state replay and observed execution of this final cff O2 loop remain outstanding.

For combo O0, the exact final IR is unchanged from H2-47. Its nearest residual still follows the dynamic initialization counter: `memory.valid119` selects the Guest90592-zero path, `memory.valid123` reads frame−132 (`i13`), `memory.valid125` writes frame−252, then `memory.valid127/129` reload and write the state before returning to the central dispatcher. Earlier source diagnosis identifies prefix initialization to zero and an incrementing initialization loop; this is not the known alternate state-50 path or a missed partial PHI. No new execution establishes a narrower runtime domain.

For combo O2, F succeeds on 26/45 attempts, copies 126 blocks/3003 instructions, and reports 19 refusals plus eight budget events. H then adds seven copies on eight attempts (38 blocks/963 instructions, one budget event); the explicit resume remains 0/1 with one budget event. The nearest residual still traverses `call.exit.valid`/`call.continue`, then `memory.valid45` → `bb_66032` → its critical edge → original `bb_66038`/`memory.valid49`. The last report message is not used to explain all nineteen rejected candidates. Calls and unknown loaded state are retained; no alias/callee preservation assumption is introduced.

## Evidence

Complete evidence lives under `guarded-correlated-booleans-real-20261112/probes-v1/` and its adjacent archive on VM and Mac. It includes all four inputs, all stage IR/inventories/reports, source/producer/runtime pins, actual commands and elapsed times, the unchanged closure audit and its controls, and the original-slot delta. The adjacent exact-set verification receipt records the archive SHA and every payload hash. There are zero native executions, zero formal C/LLVM observations, and no new exclusions or comparator changes in this probe stage. A later fixed I runtime must retain this v1 evidence and run its own frozen validation.
