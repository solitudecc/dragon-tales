# Invariant self-loops: four frozen real modules (2026-11-12)

K was applied once to each of the four final I1 modules. This is a structural probe, with zero generated-native executions and no full replay. It does not establish complete de-flattening, terminate an infinite loop, assume progress, or remove unknown dispatch outcomes.

## Binding and recipe

Input bytes were independently frozen on Mac before retiring redundant expanded I1 files. Each matches the corresponding `probes-v1/<case>/module.after.ll` member in the complete I1 archive `87d51318ba31384667346e2814bb30db9e16261899bfdc4c87076c0d856cf72c`; `INPUTS.json` records every original path, length and SHA. The VM received those exact copies. The preceding I1 recipe is neither rerun nor modified.

The new public-API C++ wrapper parses the existing LLVM module, verifies it, and invokes only `invariant-self-loops`, explicitly `max_loops=64`, `max_work=1000000`, `max_blocks=65536`, `max_iterations=1`, `max_pass_runs=1`. No cleanup, F/H retry, memory forwarding or target-guard pass follows. Pre-execution contract SHA is `05066c09d7862913804aeb0469dd6968744123756784ba3c9b61a0174e68d32c`.

Runtime: `/home/cc/vmc-runs/invariant-self-loops-20261112/runtime-main-v1`, backend SHA `c05fb5ec022b35ce192b563ba48a06821ec56c33b0e72d5e076f94bd1faa99f0`, shim SHA `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`. Actual dynamic linkage and a smoke test are saved. The mutable shared build and its subsequent I2 changes are not execution inputs. All source, input, producer, runner and runtime pins are unchanged after the four calls. Every module verifies and preserves its guarded-entry manifest.

## Results

| Case | Candidates / isolated | Removed self PHI incomings | Work | Complete / budget | Strict conditional closure |
|---|---:|---:|---:|---|---|
| f-cff O0 | 0 / 0 | 0 | 131507 | 1 / 0 | true |
| f-cff O2 | 0 / 0 | 0 | 131519 | 1 / 0 | true |
| f-combo O0 | 0 / 0 | 0 | 209306 | 1 / 0 | false |
| f-combo O2 | 2 / 2 | 50 | 317461 | 1 / 0 | false |

The four calls exited zero, taking 2.616, 1.190, 17.352 and 8.412 seconds respectively. No resource limit was lowered. Each started with at least 512 MiB free; the earlier full-disk pause and cleanup preceded these calls.

Both successful candidates are `bb_66032`, one in `dragon_static_entry_x86_64_10090.guarded.real_sp`, the other in the ordinary root. The guarded view preserves `icmp ugt i64 %RCX.10, 50`; the ordinary root preserves the corresponding `%RCX.8` comparison. Each retains the original false exit `bb_66032.bb_66038_crit_edge`. Only the true self edge is redirected into a new `bb_66032.spin` containing one unconditional branch to itself. Divergence is preserved, not converted to a return or unreachable instruction. Each original block has 25 self-preserving PHIs; the audit confirms exactly its one self incoming is removed and every external incoming value/block remains in order and unchanged.

All actual target switches, their complete case/default lists, selector LOADs, equality checks and same-read target guards compare exactly between the before/after inventories. The unknown false domain still reaches the original central dispatcher. This component removes the irrelevant self edge from the finite continuation proof domain; it does not prove the incoming state finite or bypass a call. The other three modules are unchanged by K. Conditional closure remains 2/4, not 4/4.

## Independent saved-output audit

The C++ producer records every original conditional self-loop across all functions before the pass, then re-collects the same original function/block identities after the transactional rewrite; it never dereferences stale instruction or block pointers. The separate Python audit verifies the first comparison, false exit, pure divergent target and exact PHI incoming transformation. Four deliberately corrupted inventory variants (changed false exit, terminating spin, changed external PHI value, changed condition) are all rejected. These are structural controls, not native semantic comparisons.

The first family-audit invocation incorrectly used an already processed I1 final module as the D37 classification anchor. Because that module already contains several guarded small target sets, it was rejected as an ambiguous anchor. The failed traceback and v1 audit source are retained. The final audit uses the original frozen I1 D37 inventory to identify the source-PC family, first checking that the complete K before inventory exactly equals the frozen I1 after inventory. It retains all ordinary conditions and all actual natural-business cases and excludes only validated same-read failure edges. The classification/closure algorithm and its predicates are unchanged. This command/input correction is not a semantic pass or an additional execution.

Evidence is archived under `invariant-self-loops-real-20261112/probes-v1.tar.gz` on both hosts, with an exact-set, every-payload SHA/length receipt. The package contains wrapper and audit sources (including failed v1), immutable inputs and anchors, all before/after IR and inventories, reports, commands, compilation/smoke/linkage logs, failures and runtime pins. No known independent I1 historical precision failure is waived; these inputs remain explicitly I1. A later combined pipeline requires its own formal replay and actual-loop observation.
