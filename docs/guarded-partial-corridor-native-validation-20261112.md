# Partial corridors after unknown calls: independent native validation

This report concerns the frozen J fixture and the public `guarded-partial-corridors` API in combined implementation milestone `55fe295`. Partial candidates are hints; successful copying does not prove a complete state set. Every committed partial candidate in this matrix reports `complete=0` and `Incomplete`, while preserving actual target LOAD, same-value fallback, unknown effects and size checks.

## Frozen experiment

The independent LLVM producer, sequential byte-array reference and C helper were frozen before candidate execution. The main model SHA-256 is `1fef932509527967a8ff460c211930384dab1b6ff85e8d78c32806f6b15cdb33`; driver `4f93bac3822cd46a628f9934d34b200cbeca487f262cf0b7b0cad32c46abb314`; API preparation `c8c01ab97644f15c988ea82d8bd05965ec915bb9fba0598cb01823ab577aca80`. The predeclared plan is `guarded-partial-corridor-native-plan-20261112.md` (SHA `546134dafa2e58a687e4fa5ce2a09620f413f654e9f6bd2c9481407ebf64c875`). No model, input, threshold or control was changed after execution began.

Four modes (`plain`, `table_before`, `table_after`, `call_inside`) each use 960 distinct inputs, 3,840 in total. Inputs cover unknown state 0/1/49/50/51/63/64/2^63/all-ones; existing and default target values; both conditions; loop limits 0/2/3; table-size boundaries 519/919/920/1023/1024/65536; and separate or overlapping register/Guest mappings. The plan records the exact base and additional boundary Cartesian products; not every dimension is crossed with every other one.

The unknown helper has only an external declaration in analyzed IR. Its independently linked native C body increments the call count, writes multiple registers, Guest storage and table cells, and may alias `Guest+80` with register offset48. The function really reloads post-call state/condition/side values. Cached entry values are explicit original SSA snapshots, not ABI-preserved-register assumptions. The actual 64KiB host allocation remains present for all inputs; the helper takes a raw pointer and may write beyond the logical descriptor size. Bounds failures after the call must still preserve all its real effects.

The normal expression `select(condition, 50, postcallUnknown)` has only a partial hint. Table writes before and after the actual target LOAD distinguish fresh values from forbidden reloads. `call_inside` adds a second call in the proposed copy path and must remain unchanged. Original loop/reentry PHIs, duplicate seed slots, seven original switch cases and default remain observable. A wrong-hint variant predicts777 instead of43 and must still match the ordinary program.

## API and compatibility

Each build executes 13 frozen public-API recipes. The previous `guarded-state-corridors` entry point leaves all four fixtures unchanged. J copies one edge in each of the three positive modes and zero in `call_inside`; wrong hints remain safe. Budget-one continuation is `[1,0,0]`, ending byte-identical to the normal output. Bad metadata and zero/insufficient budgets leave IR unchanged. The initial committed result reports one partial candidate/attempt/rewrite, zero unresolved edges, zero budget exhaustion, and explicit incomplete status. Fresh post-call LOADs and the original external call are retained.

Each new runtime reproduces **45** saved I2 IR outputs and **94** saved H/H2 outputs byte-for-byte, using the exact original input paths/ModuleIDs. All matching byte comparators reject a deliberately corrupted copy through their normal comparison path. J main/no-Z3 produce **41** byte-identical IR files with 41 such corruption controls. These comparisons add no native executions; earlier I2/H/H2 native observations are not counted again. Root separately binds K's original native evidence through byte-identical API output in the combined runtime; this report does not re-execute or recount K.

## Native execution and saved audit

Both builds independently complete the frozen O0/O2 matrix. Per-build counts:

| Observation | Count |
|---|---:|
| Normal complete-state comparisons | 17,280 |
| Before observations | 7,680 |
| Normal transformed observations | 5,760 |
| Wrong-hint transformed observations | 1,920 |
| Unchanged call-inside observations | 1,920 |
| Successful business-return observations | 7,056 |
| Checked failure-path observations | 10,224 |
| Faulty-IR observations | 9,600 |
| Specified wrong-result faults caught | 5,056 |
| Comparator corruption controls caught | 190,080 |
| Saved streams / frames | 28 / 26,880 |

All normal comparisons match, with zero exclusions, skips or mismatches. The 17,280 observations include explicit failure exits and are not 17,280 successful complete business returns. Repeated before/after/hint/optimization/build executions are not new independent samples. Per optimization level, required fault detections are: call removal 960, stale register 642, forced known arm 500, post-write reload 282, and deleted fallback 144; each is caught in full. The original 180-second timeout remains unchanged for each native group.

Every normal/fault observation compares return value, call count and all 73,728 arena bytes. This includes the complete register area at the input-selected mapping, 64KiB Guest, all seven Exit fields, descriptor/config and remaining bytes, without normalization. Each normal observation has 11 deliberate field-corruption controls using the same comparator. Both builds pass the complete saved-stream audit. Saved streams are checked for exact membership, full frame length and EOF and independently replayed through the frozen reference; replay counts are not new native executions.

The five faulty variants remove the call, substitute stale pre-call state, force the known select arm, reload the table after a write, or delete the original777 fallback case. The required wrong-result inputs are selected by the independent fault model; every fault observation must also match that fault model. A changed hint alone is a normal equivalence variant, not a fault.

## Runtimes and evidence

| Build | Backend SHA-256 | Shim SHA-256 |
|---|---|---|
| Main | `62ff74c752915064d3cfe87d024ead24169d5449386f9b91ea1f9fa01801f39e` | `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84` |
| No-Z3 | `d8fd563b9b0158bca90567c677a310e4f0088d3a81f18d05544e53ae65ae80a6` | `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0` |

Runtimes are `~/vmc-runs/guarded-partial-corridors-20261112/runtime-{main,noz3}-v1`; native evidence is the separate **singular** namespace `~/vmc-runs/guarded-partial-corridor-native-20261112`. Loader paths and before/after hashes bind execution. Root separately reports focus 66/2480 and CTest 1131/1131 and 996/996; those are not native comparison counts. The combined source patch and K compatibility are in root package `guarded-partial-corridors-metadata-v1.tar.gz`, SHA-256 `06774b4778547d2eba1559a496272b316d9b1246c348c130f45604be9c8b1757` (118 payloads); no assumption is made that the VM working-tree HEAD alone identifies its applied source.

The package includes the actual driver and compile command, frozen source and before IR, API reports, logs/exit codes, native shared libraries and gzip streams, and saved audits. `run-native.py ROOT main` or `noz3` reproduces sequential O0/O2 execution with the original 180-second per-group timeout. `saved-audit.py ROOT main OUTPUT` rechecks every saved frame. `i2-compat.py`, `h2-compat.py` and `byte-compat.py` preserve their exact CLI arguments in logs/source. Restore older evidence namespaces named in their pins if necessary. One byte-comparison CLI invocation initially omitted its required ROOT argument and exited2 before comparisons; the usage log is retained as `byte-compat.log`, followed by the successful unchanged-script invocation `byte-compat-v2.log/.exit`. It is not a product failure or semantic observation.

This is host-compatible LLVM IR validation with a declared helper prototype. It does not infer arbitrary external function prototypes, syscalls, callee summaries or a complete state/target set; it does not add machine-instruction, IGNIL/Unicorn, VM-protection or full-corpus coverage. Real ELF closure and observer results are separately validated by the other agent. Partial status remains meaningful after this successful semantic experiment.

Final archive: `guarded-partial-corridor-native-20261112.tar.gz`, SHA-256 **`7b6d8806d854d3642fdac7576b336009d6b3cc7b8924cdb32e4d177789978389`**, 997 payload files plus one manifest (998 total). Both VM and Mac independently verify exact archive and expanded file sets, lengths and SHA-256 values. Local mirror: `/Users/c/Desktop/deob/validation-artifacts/guarded-partial-corridor-native-20261112`; adjacent `-vm-archive-verification.json` and `-mac-archive-verification.json` are retained on both hosts. Archived README precedes this hash footer; no sealed payload was changed. No native evidence was retired by this task.
