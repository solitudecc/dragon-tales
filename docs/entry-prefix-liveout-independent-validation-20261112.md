# Entry-prefix direct-exit live-outs: independent validation

Implementation `aac31b6` extends only ordinary uses in one direct exit whose sole original predecessor block is the defining block. It inserts an original/fresh value PHI per repaired definition and preserves every successor slot. It does not introduce memory facts, copy unknown calls, or support arbitrary non-PHI propagation through multiple exits or joins. The VM used actual checkout `756d64b75b93b64537b52567918bc43ee84c328f` plus the recorded patch, not a checkout of the implementation commit.

## Independent source and native domain

Frozen `scripts/validation/entry_prefix_liveout_oracle.py` SHA-256:
`e28032a57b2d3f8afd5a2a2bae4cf0c56b45a4426fd198525246771410e40aaa`.
The production API driver is the unchanged prior source
`entry_prefix_production_driver.cpp`, SHA-256
`c3de98f01ba23cd2bd68145df2ac4a86e289ebe68f4ac41a6900b5d9551cd941`;
its reused binary is `6e579299a7c769cf5ff3afa81f92f94843fe55c7048384e93f30d22b9ef35889`.
It invokes actual `LLVMLifter::runEntryPrefix`, verifies the result, checks module/argument/global/metadata identity and ordinary functions, and writes no output on rejection. No standalone prototype substitutes for production execution.

Six new named modes cover direct exits, a downstream diamond using exit-local snapshots, finite loop reentry, unknown calls, volatile effects, and three duplicate successor slots. All six must actually redirect the accepted seed to a cloned definition and have an original/fresh bridge in the un-cloned exit. The loop returns to the original definition; only an exit-local snapshot flows into its history PHI. The call remains in the original exit and modifies both registers and Guest memory; four volatile operations remain there. This is a host-compatible, i64 generic LLVM IR suite, not a new x86/ARM instruction-lifting campaign.

The frozen 384 inputs are six modes × four x values (0, 1, 2^63, 2^64−1) × four finite loop limits (0–3) × guard hit/miss × short/full descriptor. They include 96 full valid guard hits, 96 full guard misses, and 192 short descriptors. No input exclusion or normalization is used. Each observation compares return, call count, and the entire 73,728-byte arena, including 512-byte registers, 64 KiB Guest, and all seven Exit fields. The snapshot is 73,744 bytes. Calls execute once for full unknown-call inputs and zero times for short inputs.

Each runtime separately runs before/after at host `-O0` and `-O2` against a Python model. Results per runtime:

| Evidence | Count |
| --- | ---: |
| Normal full-state matches | 1,536 |
| Faulty-IR observations | 896 |
| Specified observable faulty-IR differences caught | 476 |
| Comparator mutations caught | 16,896 |
| Complete saved streams / frames independently audited | 38 / 2,432 |
| Mismatches / exclusions / skips | 0 / 0 / 0 |

The seven faulty IR controls replace the fresh snapshot, reuse a stale LOAD value, drop a loop STORE, alter carried history, drop an unknown call, drop a volatile STORE, and corrupt Exit sourcePC. Per optimization level their required detections are respectively 32, 32, 22, 24, 32, 32 and 64. The faulty model checks the actual erroneous execution too; unchanged observations are retained, not counted as detected faults. Full saved-stream replay adds zero native executions.

Three structural negatives retain explicit rejection, unchanged module and absent output: a remote ordinary use, an exit with two predecessor blocks, and direct historical PHI propagation. Two structural-only positives retain distinct old/fresh `freeze undef` executions and preserve fallback poison plus both `nsw` adds. No deterministic native result is claimed for undefined/poison values.

## Runtime pins and prior compatibility

| Runtime | Backend SHA-256 | Shim SHA-256 |
| --- | --- | --- |
| main | `34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f` | `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84` |
| no-Z3 | `e34bd492f93badcadfd441f51d089eeb06d069e1e5da9faac779b748194df3dd` | `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0` |

Both groups record source/runtime pins before and after, driver `ldd`, and explicit no-Z3 CMake setting. Same inputs, model, driver and controls are used in both. Each group separately reproduces all 13 previously successful output IRs byte-for-byte, retaining the exact old input path spelling/ModuleID. This is byte compatibility, not another execution of the prior 1,824-case native evidence. Previous historical/shared/budget controls remain rejected; the old `reject_liveout` fixture now succeeds because it lies precisely in the newly supported direct-exit domain. Its old hardcoded reject audit was not altered or rerun as if it passed.

Independent code review checked that stored ordinary `Use*` objects are outside the cloned region and unaffected by PHI operand growth, original historical PHIs are not rewritten, k duplicate original edges receive k original plus k fresh inputs, and bridge instructions are included before committing the shared instruction budget. Parent independently demonstrated old-code test failure: each old main/no-Z3 final binary group had 10 cases / 227 assertions with five cases / 20 assertions failing; each candidate passed 10 / 262. Parent full CTest passed 1,056 / 1,056 and 921 / 921. These are parent results, not additional independent executions.

## Retained infrastructure history

The first saved-audit script correctly verified all 2,432 frames but incorrectly expected the original latch to contain only a branch. The frozen fixture actually retains its STORE before that branch. Its source, stderr, stdout and failed audit JSON remain in `saved-audit-first-attempt/`; the final audit checks the exact STORE and original-target branch. This changed no model, input, native output, precision gate or semantic comparison. A compatibility script syntax error was corrected before it was frozen/uploaded and before any run. Neither is old-production regression evidence.

## Complete archives and reproduction

Both hosts use namespace `entry-prefix-continuation-20261112`:
VM `/home/cc/vmc-runs/entry-prefix-continuation-20261112/`;
Mac `/Users/c/Desktop/deob/validation-artifacts/entry-prefix-continuation-20261112/`.

- `oracle-main-v1-evidence.tar.gz`: 284 files; SHA-256 `f8b774c2c181d3d47c9ea85ba1bc9992052ca05da41b0557bd46e372a0e8f9e6`.
- `oracle-noz3-v1-evidence.tar.gz`: 281 files; SHA-256 `396aecd4ab0a9f4c8b43da885b08f4cb092bfed3a5795986d5c942911382e9e8`.

Every payload has path, size and SHA-256 in `FILE-MANIFEST.json`. Both tar member sets and all payloads were verified on both hosts; Mac complete expansion was independently checked too. Archives retain sources, generated IR, raw compiler/driver logs, failure evidence, complete compressed snapshots, compatibility references, binaries and audits. To audit a restored group while its recorded runtime is restored alongside it:

```sh
python3 -B oracle-main-v1/entry_prefix_liveout_saved_audit.py oracle-main-v1 --flavor main
python3 -B oracle-noz3-v1/entry_prefix_liveout_saved_audit.py oracle-noz3-v1 --flavor noz3
```

Audit writes a report; use a separate extracted copy to preserve the frozen manifest. No new broad corpus, instruction semantics, table immutability, cross-function summary, arbitrary live-out SSA, or full deobfuscation claim follows from these results. Real protected sample validation is independently documented by the real-sample agent.
