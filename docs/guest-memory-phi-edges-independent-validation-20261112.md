# Guest-memory PHI edge coverage: independent validation

Baseline `e5d49be`, implementation `5e9a75f`. This is an IR correctness fix,
not a new loop fixed-point analysis or additional deobfuscation coverage claim.
The ordinary memory-fact worklist excludes backedges and unreachable blocks.
It previously created a conflicting-value PHI using only the known predecessor
blocks, even when that set omitted real CFG edges. It also omitted duplicate
successor slots. The separate canonical loop-PHI path had the same duplicate
backedge omission. The fix refuses incomplete ordinary PHIs and emits one
incoming for every actual edge when all facts are proved.

## Independent adapter and frozen model

The new adapter directly invokes production
`dragon::llvmlifter::detail::runGuestMemorySSA` with existing options and
`runMem2Reg=false`. It parses into an existing StaticAnalysis LLVMLifter module,
verifies the input, runs the pass, verifies the output, and saves all statistics.
It returns 4 and writes only an explicitly suffixed `.invalid.ll` artifact if
output verification fails. That artifact is never used for native compilation.
Only the frozen `libdragon` is linked; no second shared LLVM is loaded.

Frozen source SHA-256:

- `guest_join_coverage_driver.cpp`:
  `1805db7cd7276985a42358ccf0a1d5cbd2b1fda4afa88a69428c237bc49c0868`.
- `guest_join_coverage_oracle.py`:
  `4f9faffaf3e58ae95bf827adc87b839a0f2077f64313df5c109ac15b646bec30`.

There are nine separate generic LLVM IR modes: two different entry STORE values
with stable loop memory, changing memory, a self-loop, a nested loop, an unknown
write call, an ordinary acyclic join, duplicate switch edges into that join,
an unreachable predecessor, and duplicate canonical loop backedges. The last
mode uses one external entry, while the two-entry loop modes retain different
entry values. All 15 original/control input modules verify before analysis.
The oracle requires actual forwarding in the ordinary, duplicate-join and
canonical duplicate-backedge controls; it does not require new forwarding in
unsupported multiple-entry loop shapes.

The 288 frozen inputs are nine modes × x in `{0,1,2^63,2^64−1}` × loop limit
`0..3` × two entry branches. No input is excluded. Each run compares return,
call count and a full 73,728-byte arena containing 512-byte registers, 64 KiB
Guest, all seven Exit fields, and the memory descriptor. The descriptor's real
pointer bytes are compared exactly using the allocation address recorded for
the run; no bytes are normalized or omitted. Loops use ordinary wrapping
integer arithmetic without poison-generating flags. This is host-compatible
DL64 LLVM-transform coverage, not new instruction-lifter/Unicorn coverage.

## Actual old/new results

The identical compiled adapter and frozen input files run against both old
backends. Each old backend has **eight invalid output modules and one valid
ordinary-join control**. All eight errors specifically report:
`PHINode should have one entry for each predecessor of its parent basic block!`
The invalid IR and verifier diagnostics are retained. There are **zero old
native executions**; these are verifier failures, not claimed native wrong
results. The old libraries were retired only after their complete dual-host
gzip copies were checked; the retirement receipt is included.

Both new backends verify all nine outputs. Each separately executes before
and after at host `-O0` and `-O2` against the Python reference:

| Per runtime | Count |
| --- | ---: |
| Normal complete-state matches | 1,152 |
| Faulty-IR observations | 384 |
| Specified observable faulty-IR differences caught | 288 |
| Comparator mutations caught | 12,672 |
| Complete gzip streams / frames audited | 48 / 1,536 |
| Mismatches / exclusions / skips | 0 / 0 / 0 |

The six real faulty IR controls reuse a stale load, drop a self-loop STORE,
use the wrong entry value, drop the unknown write call, select the wrong PHI
value, and corrupt Exit sourcePC. Per optimization level they produce
24/32/16/24/16/32 observable differences, all detected. Unchanged faulty
observations remain in the streams and are not counted as caught differences.
The faulty model is also compared against the actual erroneous execution.
Comparator controls mutate result, call count, registers, Guest and every Exit
field through the same equality helper used by normal comparisons.

A complete saved-stream replay reproduces every full frame and fault count,
adding zero native executions. Additional pin/structure audit checks input
source identity, runtime/source/binary before/after hashes, runtime linkage,
all verifier errors, and exact original PHI multiplicities. The ordinary PHI
has one left and one right incoming; the duplicate join has two left and one
right; the canonical loop PHI has one entry and two body incoming values.
Calls and loop STOREs remain. New main/no-Z3 output IR is byte-identical for
all nine modules, with no ModuleID normalization.

Stable/changed/self/nested/unknown-call multiple-entry loops conservatively
retain the LOAD and record a join barrier. The unreachable-predecessor case
also rejects the incomplete ordinary merge, but the existing later checked-edge
phase independently proves a copy (one forwarding, three PHIs). It is therefore
incorrect to require the whole pass to make no change in that case. Native
comparison covers its final result, not just the rejected ordinary merge.

## Runtime binding

| Runtime | Backend SHA-256 |
| --- | --- |
| old main | `34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f` |
| old no-Z3 | `e34bd492f93badcadfd441f51d089eeb06d069e1e5da9faac779b748194df3dd` |
| new main | `d6cfa1bfc4898abcd2ab771a368010c247b78eb98b27f93f9908b42e9ddd3c2f` |
| new no-Z3 | `c566a1e43fe067ff43d4b1fb13f8308c3db0b68d56d086d19cb75747429c7ab6` |

The main shim is
`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`;
no-Z3 shim is
`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`.
The adapter only needs the backend, but both runtime files are recorded.
Production source/build/CTest metadata is independently archived by the root
agent; this report does not add those test counts to native observations.

## Retained infrastructure failure and reproduction

The first adapter compile failed because `detail` was ambiguous between two
project namespaces. The original source, compiler log and nonzero exit are
preserved in `compile-first-attempt/`. Fully qualifying the existing function
fixed the harness, after which the same binary served all four runtimes.
The frozen model, inputs, comparisons and controls never changed between runs.
No invalid-IR execution was used to force a native failure.

Full evidence namespace:
`/home/cc/vmc-runs/guest-join-coverage-native-20261112/`, mirrored under
`/Users/c/Desktop/deob/validation-artifacts/guest-join-coverage-native-20261112/`.
The complete archive is `guest-join-coverage-native-20261112.tar.gz` beside those
directories: **574 files / 573 payloads**, SHA-256
`bcd5a469bbc0be4b159373bc9517e8a30179f704bfb383b113f69afa3a600a69`.
Its README gives the exact compile and execution commands.
`FILE-MANIFEST.json` lists every payload size and SHA-256; both hosts verify the
complete file set and all payloads. Raw logs, invalid outputs, all valid IR,
sources, reference inputs, fixture binaries and every compressed snapshot are
included. Production runtimes are in their separately verified root archives.

To replay saved observations without native execution, use a separate restored
copy of the evidence and run:

```sh
python3 -B source/guest_join_coverage_oracle.py audit main-v1
python3 -B source/guest_join_coverage_oracle.py audit noz3-v1
python3 -B source/guest_join_coverage_audit.py .
```

The final audit binds recorded absolute source/runtime paths and requires their
restoration at the original locations. Its read-only checks write a new report;
use a copy to preserve the original manifest. No whole-corpus or complete
loop-deobfuscation claim is made by this synthetic matrix.
