# Original large-sample LLVM native calibration on ROCK5B

This is a new host calibration of the original untransformed `flatten-switch-O2` raw module. O0 and O2 each passed all16 original inputs:32 new native full-state matches and96 comparator corruption controls. It does not claim a transformed/closed LoopImage native result. The previous ROCK32, old C16 and old Unicorn16 are frozen references, not new executions or recounted results.

The raw module is747549 bytes, SHA256 `4315ddcd08fd7e26861197be9a550ca7a1eee685be4206cc56f8c828d07abc05`, with literal `; ModuleID = 'lifted'`. The original ELF is9424 bytes, SHA256 `bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`. All50 selected source/input/reference/provenance members were checked against transfer manifest `719ca007dcbf1c790bfe14104a71fb8a7ab648d04a848d6db50bc5c45fc6fc07`; identical complete copies are included under the new evidence root/frozen-selected.

The unchanged portable runner is `scripts/validation/tigress_dispatch_native_before.py`, SHA256 `8c283ece2b49114d61d96a08b356b37846d6572326c50c4138664d366ee7f7a9`. It extracts unchanged original pure comparator AST bodies from `dispatch_corridor_real_replay.py` (b5516949...) and the existing complete Adapter. This supplies portable explicit paths/case bindings without altering old checker mains, expected state or normalization. Saved auditor `scripts/validation/tigress_dispatch_native_saved_audit.py`, SHA256 `e9f2bf72c9489e9357b1a6ca1877368d5d14fcd2c3fffcfe755c6a69639f5884`, reopened all32 persisted inputs/outputs and96 controls successfully, with0 further target runs.

The original driver is `frozen-selected/sources/frozen-driver.c` under the evidence root,1829 bytes, SHA256 `e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb`. Its argc3 interface, entry callbacks4096/4112, ordinary root invocation and budget0 are unchanged. Input is66048 bytes (512 registers +65536 Guest). Output is66136 bytes:24-byte header/call counts,512 registers,65536 Guest,56-byte Exit (7u64), and8-byte remaining budget. Every byte plus all76 named register/flag fields was compared; required Exit plus budget is(1,3,4111,61440,1,0,0,0). The96 original controls corrupt RAX,Guest memory and Exit, three per successful observation. They are comparator checks, not new native executions.

The fixed SDK main `9ee66d8343727cfbc0307b748d645cb57d71fcd70e067bdbe0e6ebbb7f3db29f`, shim9d95c640 and exact Z3 `.so.4.13`4da2bb31 were checked before/after against stable paths and actual worker mappings. They provide only existing comparison-ABI imports. The native executables link the original driver and raw-generated object, with no product transformation or analysis API. Preserved UCdev is imported for original adapter constants but never emulated. GCC14/LLVM21 tools,Execution.h,Python and reference environment are exact-pinned. CPU0-3,180s process-group guard,512MiB startup/64MiB running disk floor remained fixed; successful guard elapsed13.564524326s. No failures, skips, exclusions or normalization occurred. No observer, fresh C reference, Unicorn execution, lift, product pass or after API was run.

Complete evidence is frozen at `/home/ubuntu/vmc-runs/loop-image-real-native-before-20261115-v1` and `/Users/c/Desktop/deob/validation-artifacts/loop-image-real-native-before-20261115-v1/ROCK`. Both hosts passed exact member-set/size/SHA verification for270 payloads totaling10,039,531 bytes, plus the self-excluded FILESET manifest. Complete source,IR,objects,executables,logs and raw frames are included; the fixed SDK/reference/tool dependencies are explicit external pins.

| Receipt | SHA256 |
| --- | --- |
| READY-PLAN-before-v1.json | `6f02a9485adffc48abc131601e9f9aea9b6ad2c80e512422b58ee17788ec2f6f` |
| SUMMARY.json | `21e0bb5fd7d80581608e11276c77906a40699bf2e4fb04ff7c6d3c2a08b44e26` |
| FILESET.json | `f7af3afb930f78f37383508f57b39d5690ad488edd1a925b6e9ab07cc011476c` |
| run/RESULTS.json | `0c321ddd03feb0b554a8e90208fce4c31bc4d2271f473ade7aecfba3cbffe2eb` |
| SAVED-AUDIT.json | `5282ebdd5cd655e1b3b4635971123598d0261edb7aa6b1c2727ca443ee721249` |
| External dual-host certificate | `d824b94a352573187305145d6a0ce194510cb5584b32bf33bfcbdd19ccffa58d` |

This finite16-input calibration validates the original native/reference/harness chain. It is neither an all-input proof nor a de-flattening precision gate. Any later transformed after result must be separately measured and reported.
