# Real Loop-view native replay sources

This directory contains the **executed** validation sources for the frozen Flatten O2 artifact, plus the previously executed synthetic adapter unit. The original guest program, pass inputs, expected frames, ordinary functions and production IR are not edited. No product pass is called by these native replay workers.

Execution is driven by exact, separately frozen JSON plans. The nominal `PLAN.json` SHA is `8d31e3abcafb78909d312d432c8c04e4bfee0892588a2cd8513ed32facfaa206`; control plan SHA is `99decb96ae1032cf24a2ad37e8563a77ca620ff352a632662559ef2fe4e070e4`. Every command/environment is recorded in each namespace's EXECUTION/GUARD-PLAN and subprocess receipts. All target CLIs keep180s/512MiB startup/64MiB running floor and CPU0–3. Restoring the exact archived paths and pinned external runtime/tools is required; these scripts do not rewrite legacy path strings or manufacture a closed result.

| Source | SHA256 | Actual role |
|---|---|---|
|`loop_real_native_adapter.cpp`|`1b7bfbbe06a045ded628b922a5c7d07f5f859468c507d495d6fa8ce73394d34b`|Actual LLVM parse/unique callee binding and observer emission; no Guest|
|`loop_real_native_driver.py`|`fcdc72442d77499a8ce7870df063f462370d41bc4a874dc8045215d0a1943f50`|Exact frozen driver delta and sidecar helper|
|`loop_real_native_adapter_unit.cpp`|`a669b78672f3726a61834e45dcc1a9ccbdbda7ba1b50a37845b11666b0327ba9`|Earlier synthetic-only LLVM unit, not real Guest|
|`loop_real_after_common.py`|`b9961219223700fef9da918633268bb344a37c1ef8339128cc37761f9384df7e`|Complete transfer/eligibility pins; full unchanged comparator import|
|`loop_real_after_emit_plan_v2.py`|`ad844cc5c03c4f1ab5803aefd3d8d71bf8ba9c551ecab36e66d06ab4553bc01e`|Actual mechanical-emission plan generator|
|`loop_real_after_nominal_plan.py`|`94a177106336aa1f8e506ec901490150f747b5ad5fcb270b73b5e5aa352478c2`|Nominal source/input/tool/runtime plan|
|`loop_real_after_execution_plan.py`|`786d259f4c9419ef9568d0632c1959448bbb4814017cc31eab05e2d66a068719`|Exact outer guard and all planned argv|
|`loop_real_after_replay.py`|`2d4a8f0eb1d01c4d594149128aa91fd16a1bfe8c170f247d2897fdca6ad9c6ce`|Actual32+32 nominal machine runs and192 comparator controls|
|`loop_real_after_saved_audit.py`|`dde21eab6362743035be6fc13d6bc41f74cd82255b97c4843b113b90661b6def`|Saved-only v2 audit, no SDK or Guest|
|`loop_real_after_controls_plan.py`|`42c7cbd3e3c54fc8df7b81dfc80a4b689885e4bf44f11f95963e4bc9a547e12c`|Exactly two observation faults and two changed-input plans|
|`loop_real_after_controls.py`|`4cf9ad23ec7dac0feb3d92a7fb46cbe2e9461d1d2a8fe1f6125ba25548e4d746`|Actual12 control executions and12 comparator controls|
|`loop_real_after_controls_saved_audit.py`|`fd35292772a2c8a728a5709269006b772ce35684229217fe7604aea9ce24caeb`|All12 saved raw outputs/inputs, eight sidecars and12 controls|
|`guard.py`|`d0452de8980c041803db571d1555fbdb63c8d2234ebaa81875f5f2125dbc7cad`|Fixed timeout/disk/affinity/source pins and actual worker maps|

The exact original comparator modules, reference loader, driver/header and16 initial/expected frames are provided by the frozen public input packet `loop-image-native-public-inputs-20261115-v1/TRANSFER-LEDGER.json`, SHA `8674465843ef192335054e7220914e980874f1d4fa659baaecc60631fd032480`. Its files[]/roles are verified rather than guessed. The main/NoZ3 artifact is identical and replayed once. Original C16/UC16 and before32/96 counts are references, never recounted. The full frame is66,136 bytes, including all512 register bytes,76 named fields, full Guest65,536,56-byte Exit and8-byte budget; no fields or raw bytes are excluded.

Historical sources are preserved, not passing variants:

- `historical/loop_real_after_emit_plan_v1.py`, SHA `490c491c9ced0b181e095a5ee070fdec345f8b737bbd5706a3c515d0dee87de5`: unexecuted preparation namespace; its guard source pin was added in a new v2 plan before any target.
- `historical/loop_real_after_saved_audit_v1.py`, SHA `3914aabb226e10b747b97da661e782027a000a399b3235c6d98a59d8be558886`: pure saved auditor whose final direct Python tuple-vs-JSON-list comparison failed after all64 raw blobs/sidecars passed. V2 checks the same complete records through exactly the original JSON storage operation and asserts the only type changes are128 Exit tuple→array conversions. No Guest rerun or normalization results from this correction.

Artifact roots on ROCK are under `/home/ubuntu/vmc-runs/`: `loop-real-native-emission-20261115-v{1,2}`, `loop-real-native-nominal-plan-20261115-v1`, `loop-real-native-nominal-saved-audit-20261115-v2`, and `loop-real-native-four-controls-20261115-v1` plus its saved audit. All raw failures/stdout/stderr remain. The evidence seal records the complete member set and exact external SDK/tool dependencies. These files do not claim a clean-IR replay, a new instruction/corpus run, unconditional deobfuscation, or a runtime protection framework.
