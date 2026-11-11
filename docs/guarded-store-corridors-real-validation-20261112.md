# Guarded STORE corridors: frozen real validation (2026-11-12)

This is validation of H1 (`6981a44`) and H2 (`f54e434`), not a claim of complete four-program de-flattening. H2 was built as `6981a44` plus the frozen source-v2 patch; the subsequent commit changes the associated documentation without changing the compiled source. The final source binding is metadata archive `54ecb71dbeb4578d5f1ce3d50192937bdd619093fde495454a06da35bfc60301`, member `metadata-v2/frozen-sources/lib/GuardedStateCorridors.cpp`, SHA `911f9044d09f76a9776d2eceda0ee95f25a81208734ccb473cb5691343fc1f18`. The concurrently developed I implementation is not a source of H2 facts or execution.

## Frozen recipes

All probes start from the four previously frozen D37 modules. The default recipe is D37 → F1 → H1 → tiny5 → constant1 → E1, exactly 46 pass runs. The independently preregistered resume recipe adds exactly one H invocation immediately after the first H, giving 47 runs. Each F/H invocation uses the unchanged defaults of 64 candidates, 32 rewrites, 128 blocks and 16384 copied instructions. This is a finite explicit retry, not an implicit fixed point or an increase in the existing GuestSSA/dispatch budgets. The tiny tail is mem2reg, instcombine, GVN, instcombine and DCE; it has no general CFG simplification.

H1 default46 preregistration SHA: `fbf27b3476f6d3dead6f5ba63402408317b5c95836cc47d9bf59f6ee6afb8095`. H2 default46: `b680a338f50343ca28ebf6d91aeb6704b35474626387121aa9dd223c16ff05de`. H2 resume47: `db31a08df9f8df366e7e0423b300fabd7428f264ee0d0beedc409e77878f0e38`.

The formal B recipe keeps prefix 128/16384, the original three eight-step rounds, all four frozen ELF files, sixteen inputs, original C and LLVM reference comparisons, and all 144 specified register/Guest/seven-Exit corruption controls. Its only added stage versus the original 46-step adapter is the explicit H resume. The in-memory original D view remains separate; the verified final parsed module is exported and the unchanged native oracle compiles that exact final IR after binding the one wrapper call. Actual `commands.jsonl` llc invocations and byte comparisons bind the exported and compiled files.

## Structural probes

| Case | H1 default rewrites | H2 first H | H2 resume | Final conditional closure |
|---|---:|---:|---:|---|
| f-cff O0 | 0 | 0 | 0 | true |
| f-cff O2 | 0 | 0 | 0 | false |
| f-combo O0 | 13 | 14 | 5 | false |
| f-combo O2 | 0 | 0 | 0 | false |

Every probe verified IR and preserved entry metadata. Native executions in these probes are zero. Closure excludes only independently validated same-read target guard failure edges; all ordinary conditions, all natural-business small-switch cases, full fallback switches and every central switch identified by case set plus actual Exit source PC remain in the audit. Renamed entry-prefix default blocks are included. Counting guards or context copies is not counting recovered states or complete loops.

For combo O0, H2 first H copies 128 blocks/1913 instructions, succeeds on 14/14 attempted edges, and still reports incomplete with budget count 2. All thirteen H1 rewritten original slots remain, and `memory.valid127.guest.edge:0` is newly redirected. The explicit resume succeeds on 5/9 attempts, copying 51 blocks/778 instructions, with four refusals and budget count 5. All first-round fourteen slots remain. Newly redirected original slots are `memory.valid399:0`, `memory.valid1257.guest.edge:0`, `memory.valid1435.guest.edge:0`, `memory.valid1964.guest.edge:0` and `memory.valid2252.guest.edge:0`. The prior budget-rejected `memory.valid1944:0` still points to its original target. A report's last rejection message is not treated as the explanation for all refused candidates.

The nearest remaining combo O0 path reads the unknown branch at `memory.valid119`, goes through `bb_68250`, loads RBP−132 at `memory.valid123`, stores that value to RBP−252 at `memory.valid125`, reloads it at `memory.valid127`, then stores the sign-extended value as state at `memory.valid129` before returning to the central dispatcher. The repaired alternate path writes the finite state 50. Neither path permits globally assuming the unknown load is 50. Combo O2 still crosses the conservative call/reloaded-register path; cff O2 retains the correlated Boolean-state path. H2 alone does not solve these separate gaps. The default46 combo O2 H attempt and the additional47 resume each remain 0/1 with an explicit budget/refusal; both are preserved, not counted as recovered paths.

## Execution and archives

Formal 47-step preregistration SHA: `5e8f0ab81317e4251bfc7da5185dcbd4d0145a4e16327c6cfab2d6089d7fdd2a`. Runtime main-v2 backend `926a3abf391b9e1c5bc2d57237d3aa0c78d37d7d25b9f4124d5cd36e25973b03`, shim `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`. The formal run completed with exit 0 in 207.329 seconds: C 16/16 and LLVM complete-state 16/16 matched, all 144 specified controls were detected (16 registers, 16 Guest memory, 112 Exit fields). All sixteen original input files match their frozen hashes; runtime, fixtures, source snapshot, harness, contracts and extra-source bindings are unchanged. There are no new exclusions, normalizations or skips. The unchanged inherited precision audit passes. The additional saved-output audit verifies all four actual llc command bindings, unchanged guarded-entry manifests, all final selector/full-fallback relationships and sixteen deliberately corrupted structure controls. It performs zero new native executions. All four formal F/H/H-resume counts match the corresponding structural probe results. Legacy GuestSSA remains incomplete in 12/12 invocations with 12 budget events; dispatch-specialize remains incomplete in 12/12 with 16 budget events. These reports are retained, not converted into complete.

Both hosts hold complete probe archives under `guarded-store-corridors-real-20261112/`:

| Archive | SHA-256 | Payload files / bytes |
|---|---|---|
| probes-v1.tar.gz | 8db4f82323a1776ee18305990a3d6f65e8c12c077f3b733d471a1e1e3688cc59 | 130 / 188234770 |
| probes-v2-46.tar.gz | bc98d22a1cabc10385865662936615aa7cc4a4065171200ea7b22ac64141e41a | 128 / 189166197 |
| probes-v2-47.tar.gz | 659840e81d3c8ec2361403e0e3c73cdd2f0c5c75d240f1c7f825089d76f9ef9e | 141 / 223712316 |

Each archive has an exact regular-file manifest with every payload's length and SHA; both hosts independently verified the complete set and every hash. Only matching redundant expanded `.ll` files were subsequently retired (32/111240717 bytes for H1, 32/111608701 for H2-46, 36/129270208 for H2-47). All source, JSON, logs and reports stay expanded; adjacent retirement receipts and restore commands preserve provenance. H2's independent D37 inputs were verified before H1 retirement, and the formal producer and hints were copied and pinned before H2-47 retirement. No raw library or unrelated namespace was removed by this validation agent.

The initial formal-harness upload before its destination directory existed failed at scp, before any execution; the directory was then created and exact frozen inputs were verified. This setup error is not a semantic test or old-code evidence. H1 and default46 structural probes are not counted as new formal C/LLVM executions. The inherited saved-IR audit was first invoked without the repository validation path (ModuleNotFoundError), then with the outer run directory instead of the required candidate directory (FileNotFoundError). Both failures are retained; the third invocation changes only those command-line/environment arguments and passes the unchanged audit. They are not native failures or additional native executions.

The actual final compiled cff O0 IR hash is `34859b006ec19bde9945493ccfd5307985d7861880845a98c44d27886f02d84d`, byte-identical to the already archived G observer input. Consequently the existing four-input, single-view-entry observations remain bound to this exact program: entry admission once; all target/business failures and all fifty central counters zero; one closed-fast-SCC guard repeats 51/63/30/53 times. These are reused observations, not four new executions. They establish the local cff O0 fast loop only, not the other three programs or recursive ordinary callee dispatchers. The observer archive remains `056f926cce14dc6b7e8214329395c35892cdbb136a07a4ab708ba0861a55db79` (see the separate observer report).

The complete formal run is archived under `guarded-store-corridors-full-real-20261112/recipe-B.tar.gz` on both hosts. The adjacent verification receipt supplies its compressed SHA, exact member count and every payload hash; the package includes all inputs, generated C/native/IR files, reports, failures, actual command logs, frozen harness and this report. The referenced H2 production/runtime source binding remains the separately verified metadata archive above; no later I production source is used as H2 evidence.
