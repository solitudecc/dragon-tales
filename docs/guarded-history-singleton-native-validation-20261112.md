# Historical Boolean identity and singleton edges: independent native validation

Implementation `f16d5fe` closes the original I native precision failure without changing its model, input domain, recipes, comparison range, or fault controls. This is bounded corridor-copy precision and semantic validation, not a claim that arbitrary flattened programs are fully recovered.

## Original failure and new result

The retained I1 package (`205fe9ff8078778aedc6cd67f77d92293087a3480a30754e8a90425856456fd6`) has whole precision gate **false**: history produced two copies instead of three, budget-one continuation was `[1,0,0,0]`, and resumed IR differed from normal. Its successful semantic comparisons did not satisfy that gate.

Both I2 builds pass the unchanged 13 original recipes: history produces **three** copies; continuation is **`[1,1,1,0]`**; resumed and normal IR are byte-identical. The supplemental unknown-scenario rejection also passes. The original strict saved-state auditor, rather than the I1 semantic-only audit, now passes. No failure was excluded or downgraded.

The frozen producer/model is `guarded_correlated_boolean_oracle.py` SHA-256 `43846283d7b522d9ee5bae110507e365f52f3d14eaf5a89c6abff90f43defa4e`; the unknown-scenario supplement is `e03c41cbe6bfd55a29aa1c08a8fe5263ca3d3aafcb6c3319a34c0b6e8588ce38`; the strict saved auditor is `b72b196beb3e8eabff885c57b56febb474cbfb108d73651cb399b41ca8b50638`. The same driver binary and all source/input hashes are retained in `frozen-source-bindings.json`.

## Execution and complete saved-stream audit

Each build independently ran the original eight-mode, 2,048-input matrix and the supplemental 256-input unknown-scenario fixture at native O0 and O2. The supplement reuses the same input values for a different expression shape; repetitions across APIs, optimization levels and builds are not additional independent samples.

| Per build | Original eight modes | Supplement | Total |
|---|---:|---:|---:|
| Normal complete-state comparisons | 8,192 | 1,024 | 9,216 |
| Before observations | 4,096 | 512 | 4,608 |
| Transformed observations | 3,072 | 0 | 3,072 |
| Unchanged negative observations | 1,024 | 512 | 1,536 |
| Faulty-IR observations | 3,584 | 512 | 4,096 |
| Specified faults caught | 1,208 | 256 | 1,464 |
| Comparator corruption controls caught | 90,112 | 11,264 | 101,376 |
| Saved streams / frames | 46 / 11,776 | 6 / 1,536 | 52 / 13,312 |

All normal observations match, with zero exclusions, skips or mismatches. Each comparison includes return value, call count and all 73,728 arena bytes: registers, full 64 KiB Guest memory, descriptor/config storage and all seven Exit fields. There are 2,784 successful business-return observations and 6,432 checked failure-path observations per build; 9,216 is not a count of successful complete business returns. Saved gzip streams were independently replayed through the unchanged reference and comparator, checking exact stream membership, frame lengths and EOF. Replays add zero native executions.

The matrix distinguishes fresh versus historical Boolean LOADs across loop reentry, shared conditions, all eight three-Boolean assignments, four-key rejection, unknown non-i1 arms, freeze, actual table writes before/after the selector LOAD, bounds failures, duplicate seed edges and budget continuation. Fault controls alter correlation, reuse a stale Boolean, drop a condition, trust an unproved hint, reload after a write, delete a STORE, corrupt a PHI, or introduce a nonzero supposedly-zero expression. Poison/undef are structural rejection domains, not native concrete inputs.

## Compatibility and runtime binding

Each new runtime reproduces all **94** saved H/H2 IR files byte-for-byte, with 94 deliberately damaged comparison controls. Main/no-Z3 I2 outputs match across **45** original/supplement IR files, with 45 damaged comparisons. These are IR compatibility checks; earlier H/H2 native observations are not re-executed or counted again. Input paths were kept identical to retain exact ModuleIDs.

| Build | Backend SHA-256 | Shim SHA-256 |
|---|---|---|
| Main | `2a284c9593dceb1241e92730e4811f2aff9cd1121f5bee72f2d1f2abd2abfb96` | `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84` |
| No-Z3 | `cb0c78271047196d95f1d29b54573d629fcb59053c8b6560e84fa0e6ed5aa40a` | `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0` |

Runtime directories are `~/vmc-runs/guarded-history-singleton-20261112/runtime-{main,noz3}-v1`. Before/after hashes and loader paths bind actual execution. Root separately reports focus 58 cases / 2,035 assertions and CTest 1,123/1,123 and 988/988; these counts are not part of this native matrix. The VM checkout plus applied patch is recorded in root metadata, not assumed equal to the implementation commit.

## Reproduction and scope

Evidence namespace: `~/vmc-runs/guarded-history-singleton-native-20261112`; local mirror: `/Users/c/Desktop/deob/validation-artifacts/guarded-history-singleton-native-20261112`.

The package contains exact frozen sources, emitted before IR, actual driver, transformed IR, report/log/exit files, gzip snapshot streams and strict saved audits. `run-native.py ROOT main` (or `noz3`) reproduces four sequential native groups. `source/guarded_correlated_boolean_saved_audit.py ROOT main main-original-saved-audit.json` and the supplemental invocation with label `main-supplement` and `--supplement` independently recheck every retained frame. API recipes use the original input paths recorded in pins; restore the I1 package if those paths have been retired. `prepare-I2-evidence.py` documents exact source/input copying; `finalize-I2-evidence.py` verifies unchanged source bindings and aggregates successful strict reports.

This validates host-compatible LLVM IR under the explicitly enumerated input domain. It does not add instruction-lifter/IGNIL/Unicorn coverage, a new general binary corpus, call-summary inference, memory immutability, or full real-program loop closure. The original I1 failure package remains unmodified.

Final archive: `guarded-history-singleton-native-20261112.tar.gz`, SHA-256 **`22ed069037aaafed81ea4f28bb9f0ee547e0d8d864a62a45e22d2786fb639ef1`**, 978 payload files plus the manifest (979 total). Both hosts independently verified the exact archive and expanded file sets, lengths and SHA-256 values. Adjacent `-vm-archive-verification.json` and `-mac-archive-verification.json` retain receipts. The archived README predates this archive-hash footer; no archived payload was edited after sealing. No raw evidence was retired by this validation task.
