# Frozen Flatten O2: actual Loop view native validation

This validates the frozen Tigress `flatten-switch-O2.elf` (`bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`) through the actual closed B view. Main and NoZ3 production output are byte-identical (`714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`); this common artifact is replayed once. B reports closed/changed/verified, work985771, no budget exhaustion. A's refusal and global precision=false remain. No new lift, product pass, C or Unicorn reference is run here.

## Binding and compilation

The production input remains unchanged. Validation adapter `1b7bfbbe06a045ded628b922a5c7d07f5f859468c507d495d6fa8ce73394d34b` changes precisely the unique ordinary1000 direct call to ordinary1010/guest target4112 into a call to the closed view. The ordinary1010 body and view fallback remain unchanged. The frozen driver (`e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb`) gets only the explicit view alias and its two4112 routing bindings; ordinary1000's initial invocation stays unchanged. The observer adds two volatile load/add/store sites at accepted/fallback and writes a separate24-byte sidecar. These are validation artifacts, not changes to the production IR.

Mechanical emission uses the previously synthetic-tested adapter, with fixed ef35 SDK supplying LLVM symbols. It parses/verifies actual B, checks the exact callee delta, unchanged ordinary/view/fallback bodies, globals and whole modules. Emission runs no Guest. The adapted IR is `1bf75468a52d776ecb8ff0dcf8723a5d7d3a1412a15da9f443cd02f7492196af`; observer IR is `b776d5bd4e4e6c4bd495bf5a89baf70cd7cdce829d789593af63e649c35d1181`. Adapted driver is `81254f59abd0864d3fd1afd86b9e72dc8f0616ebef22d725d7580926e30732bf`.

Compilation exactly retains the before protocol. O0 consumes emitted IR. O2 first runs `opt-21 -S -passes=default<O2>`; both levels verify and then use `llc-21 -O0 -filetype=obj -mtriple=aarch64-linux-gnu`, followed by GCC14 `-O0` with the frozen driver/header. Each generated ModuleID is checked against its actual input path; nothing is stripped or normalized. Native executables must not link an SDK or LLVM DSO. Worker maps separately bind main d446, shim9d95, wheel Z3 4da2 and the preserved ucdev library; no new UC execution occurs.

## Nominal execution

The16 original inputs run at O0/O2: **32 uninstrumented complete-state matches**, plus a separately counted **32 observer complete-state matches**. All32 observer sidecars contain actual accepted/fallback counts **1/0**. Each group also detects **96 comparator-only corruptions**, one exact RAX, Guest-memory or Exit field difference per input/optimization. No skip, undefined exclusion or normalization is introduced.

Every comparison includes all66,136 raw bytes:24 header bytes (magic and two call counters),512 raw register bytes,65,536 Guest bytes,56 Exit bytes (seven u64 fields), and8 budget bytes. The unchanged comparator checks76 named fields, calls and all Guest bytes; raw equality also covers every register byte and budget. Nominal root Exit+budget is `[1,3,4111,61440,1,0,0,0]`. Observer counters are separate from this frame.

The original Flatten raw before calibration remains32 native matches/96 controls, and the original C16/UC16 references are reused by exact input/ELF/driver/raw lineage. Those executions are not repeated or added to these counts. O0/O2 and main/NoZ3 compatibility are not new input domains: there are16 distinct nominal inputs.

Nominal guard: exit0,32.294054s, CPU0–3,180s deadline,512MiB startup and64MiB running floor. Inputs, sources, tools and runtimes match pre/post pins. Results:

- `/home/ubuntu/vmc-runs/loop-real-native-nominal-plan-20261115-v1/run/RESULTS.json`, SHA `b45c24f5c0be21c30d9e4e92e07e82093ba997ac42427ff198c9f36815c82ccb`.
- Saved-only audit v2: `loop-real-native-nominal-saved-audit-20261115-v2/REPORT.json`, SHA `17ab56274fa43e821abc46162e12bac8eaae9b457d009c2786604983003c96b9`; all64 blobs/inputs,32 sidecars and192 comparator controls independently rechecked.

## Four control classes

Exactly12 additional machine executions, all O0/O2, are separate from nominal64:

| Class | Executions | Required actual result |
|---|---:|---|
| Used table slot48 changed |2 ordinary reference +2 observer| Complete same-input peer equality; observer0/1 |
| SP+8 with sentinel migration |2 ordinary reference +2 observer| Complete same-input peer equality; observer0/1 |
| Omit unique wrapper callee retarget |2 faulty observer| Nominal full state still equal; actual0/0 rejects the1/0 gate |
| Accepted increment1→2 |2 faulty observer| Nominal full state still equal; actual2/0 rejects the1/0 gate |

The table input changes the full i64 at Guest8192+48*8=8576 from0x1741 to0xdead. The original LLVM reference actually exits with `[2,1,4390,57005,1,0,0,0]` and PC0xdead at both optimization levels. This is a concrete TRANSFER/BRANCH outcome; it is not forced into the nominal root-RET expectation. The observer fallback exactly matches each full reference output.

The SP input changes register offset32 from0xc808 to0xc810, copies the original0xf000 sentinel at Guest0xc808 to Guest0xc810..0xc817, and retains the old sentinel. All individual changed bytes are recorded in PLAN. Its original LLVM execution actually returns the nominal root-RET Exit; full Guest/register/RSP state is taken from the changed-input peer, not from nominal expected output. These four ordinary reference runs are **LLVM references, not UC truth**. No new C/UC claim is made for either changed input.

The four fallback matches also detect12 exact-field comparator-only corruptions. The four runtime observer faults are deliberately wrong valid-IR machine executions and are counted as four specified fault detections, not normal matches. Guard: exit0,21.382076s, same180/512/64 limits and CPU0–3. No retry or input change occurred.

Control RESULTS: `/home/ubuntu/vmc-runs/loop-real-native-four-controls-20261115-v1/run/RESULTS.json`, SHA `8daaa66b2ac7c511f138818987ae93bed8fac415ed0124cbddf98ce25334eb21`. Saved audit: `loop-real-native-four-controls-saved-audit-20261115-v1/REPORT.json`, SHA `b8c3ca805bdf7eb2795d9971e6bed7cf98b0060a56fb248cd33cdd1d2d730ebb`. All12 raw outputs/inputs, eight sidecars and12 comparator records are rechecked without loading an SDK or running Guest.

## Preserved preparation/audit failures and replay sources

Emission plan v1 was never executed: pre-execution review found its guard source was not pinned. A new v2 plan adds that source pin; v1 is retained. Saved-audit v1 checked all64 blobs and sidecars, then rejected Python tuple-versus-stored-JSON-array types in complete comparator records. V2 applies exactly the original JSON storage operation, explicitly permits only128 Exit tuple→array conversions and retains every field/value/order. Guest data normalization remains zero. Neither event is a product/Guest semantic failure and neither reruns Guest.

Actual adapter, driver, common helper, emit/nominal/control planners, runners, fixed guard and saved checkers are under `scripts/validation/loop_real_native/`. Historical failed/pre-execution sources are retained in its `historical/` directory, with artifact pins in README. No clean-IR native run, additional observer locator, instruction corpus, heap/VM protection or arbitrary input semantic proof is claimed. Production SDKs and host tools remain explicitly pinned external dependencies; the small native evidence package contains the source/input/execution evidence and full failure records.
