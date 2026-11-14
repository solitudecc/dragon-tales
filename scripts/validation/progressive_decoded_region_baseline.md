# Portable decoded-region baseline: review notes

Prepared and executed once on ROCK main, 2026-11-14; actual results below.
An earlier zero-analysis configuration failure is preserved separately. No
native/C/Unicorn or opt-in checked component has run in this batch. This
harness owner did not build or install dependencies. The ordinary collector
`3c399e7e...` and its saved results remain unchanged.

`progressive_decoded_region_baseline.py` accepts a newly sealed `--plan` and
`--plan-sha256`, plus a fresh absolute `--out`. The adjacent example is
deliberately `ready=false` and has unresolved runtime/tool paths. Do not run it
as an actual plan. Freeze its explicit values after the first ROCK build and
root review; the new runtime is never inferred from a legacy RPATH.

The selected `TRANSFER-MANIFEST.json` and its 50 input files are verified
without editing them or invoking a legacy full-namespace verifier. Original
`/home/cc` strings remain historical provenance. Every selected source, input,
expected snapshot and canonical LLVM file is checked by size and SHA before
and after execution. Neither old snapshots nor canonical LLVM are used to
set the new Guest-memory values, target facts or region selection.

The entry does the following once per backend, sequentially:

1. Pin the exact venv Python, new backend/shim, build manifest, LLVM `opt`, SDK wrapper/C headers,
   the existing static-region helper and frozen CFG/pipeline helper. Check
   actual mapped backend and shim, and capture the complete LD_DEBUG trace.
   Tools/venv/runtime may have normal symlinks: record their logical/resolved
   paths, hash the actual bytes, and require the same resolutions at the end.
   Selected inputs and repository source files still reject symlinks.
   `runtime.library_path` is an explicit frozen list, including the owned Z3
   wheel directory when needed; never infer directories from old paths.
   Pin the exact wheel Z3 shared library from the build/dependency audit and
   require it is the worker's sole Z3 mapping both before and after the run.
   A no-Z3 plan instead requires no worker Z3 mapping. A system4.8/wheel4.13
   mixture is rejected. LLVM `opt` is a separate process, may have its own
   system Z3 dependency, and retains its own LD_DEBUG log without a shared
   solver-context claim. Unicorn is not called or added to library_path.
2. Load the one original ELF, disassemble with the original strategy, and save
   a full source Graph snapshot including machine bytes, IGNIL text/operands,
   unlifted markers, branch flags and Function membership.
3. For every valid Function, require a unique positive executable ELF STT_FUNC
   extent. Use the existing `static_region_replay.select_region` to choose
   decoded leaders. Request `lift_static_region` with only the selected
   additional blocks, `rebuild_decoded_blocks=true`, max_blocks4096 and
   max_dispatch_cases65536. It does not mutate source Graph or request a
   finite/known-memory Graph pass. Failed/partial IR and reports remain.
4. Verify actual region attributes and raw LLVM. Check that independently
   expected target addresses are decoded and appear in actual private-region
   coverage, and that exactly one actual LOAD-driven switch admits all of
   those case values. The static table manifest is consumed only here, after
   selection/lifting, as a coverage oracle. It cannot initialize Guest memory.
5. Only if that raw coverage gate passes, run the unchanged ten-step B pipeline
   once (max_iterations1/max_pass_runs16). Verify and save actual final LLVM,
   CFG/SCC/operation counts and every raw pass counter/status, including
   Incomplete, unavailable, budgets and unknowns. Save source Graph again even
   on exceptions and require exact equality. Never strip ModuleID or rename
   blocks to force equality with historical LLVM.

There are zero calls to the three opt-in C++ components, zero C/UC/native runs,
and no stock native code generation. A covered decoded region retains actual
target LOADs and unknown-target Transfer exits; its source Graph may still
have no resolved central edges. These are deliberately separate measurements.
`region_coverage_complete=true` would mean only that the private raw region
admits the expected cases. `precision_complete=false` and
`semantic_validation=false` remain explicit: this baseline does not prove the
business loop has lost its dispatcher or that LLVM execution matches the ELF.

The guard retains 512MiB startup free space, 64MiB running floor, and a complete
180s CLI deadline. The parent starts one worker; the worker only invokes LLVM
verification subprocesses sequentially. Plan/inputs/tool/source hashes are
rechecked on completion. Estimate: under16MiB output/backend, roughly0.5–1GiB
RSS, seconds to tens of seconds; these are estimates, not ROCK measurements.
No library or entire old archive is copied by the producer.

Python3.10 compatibility is explicit: this entry hashes files in chunks. It
does not call the helper's `file_sha`/`mapped_libraries`, whose implementation
uses newer `hashlib.file_digest`. Actual module imports are path-pinned.

Local preparation controls use only stdlib and saved/local data: six bad
coverage/attribute cases and three wrong/mixed/no-Z3 mapping cases are detected;
the old region helper's pure selection
controls pass with its newer hash function replaced by a raising sentinel;
all 50 selected inputs hash correctly; the saved historical canonical LLVM has
one actual LOAD-driven switch covering the 75 expected targets. These are
reader/selection controls, not new product gains or new native comparisons.
Prepared source snapshots 8379402d (initial) and 74e490ca (tool/Python path
iteration) and the initial control receipt remain in the Mac preparation
directory; the solver-pinned version has a separate final SHA/control receipt.
The final f050 version alone was deployed; earlier preparation snapshots were
never deployed. The original template e8f7991e remains in prepared-source-v3 and
the deployed source-v3 evidence. The current example still says ready=false,
with its illustrative Z3 filename corrected to actual SONAME libz3.so.4.13.

## Actual main v3/v4: IR coverage passed, CFF still present

The owned evidence root is
`/home/ubuntu/vmc-runs/progressive-flatten-rock5b-preflight-20261114-v1`.
The unchanged runner SHA is
`f050a0daf96fb1d90fa3cf6daf758cd516a5a1442d29a13eabf66f55f80aa930`.
Actual backend `28ddd0d8...` and shim `9d95c640...` came from root's a7e2
ROCK build and are external pins; no compiled library was copied. This is
main only: ROCK no-Z3 has not been built or run in this batch. Old VM counts
are not credited to ROCK. The initial complete SDK CTest had 1,231/1,233
passing, with two Python Guest RET-imm16 `root_ret_max` reference differences
under separate investigation. This baseline does not claim a fully passing
SDK or completed semantic validation.

`decoded-main-v3` exited 1 after 1.203s, with zero disassembly/lift/pass calls.
Its plan incorrectly pinned the link-input filename `wheel/libz3.so`. The
backend has DT_NEEDED `libz3.so.4.13` and RUNPATH to that same wheel directory.
These are two distinct regular files (inodes 2225953 / 2225952), each
19,027,008 bytes, SHA
`4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`.
The saved loader contains only the wheel `libz3.so.4.13` Z3 init path. This
was a preparation error confusing link input with runtime SONAME. The strict
guard correctly rejected it; no product defect or system/wheel mixture was
established. The original source, failed plan/report/logs and zero-analysis
counts remain untouched. The 10-file / 87,654-byte failed-attempt set is
dual-host verified by `V3-FAILURE-FILESET.json`, SHA
`df118d539a34b202d428be84c65b075b1ae3286e86859d356180a914f009c53b`.

Before separately authorized v4, `soname-binding-v4` froze llvm-readelf's
full dynamic output, both files' identities/hashes, and the saved loader init
path. `SONAME-BINDING.json` SHA is
`e5e7fac25c13677aa319490d279a1a8dd3e231bd1d411f3cdb6bb386fbf54f03`.
New `plan-v4/PLAN-main-v4.json` changes exactly one value:
`solver.library.path` now names the actual wheel `libz3.so.4.13`. All other
plan data, source, guards, budgets and DSO/input hashes are identical. Plan SHA
is `502da1d99f3ec439bd271706c7356539d27408a577666f24d6f6a9ec920a1b18`;
invocation SHA is
`472b59d2dff988465fec9359bdd84c41cefd3de324c4d0b38cf2f3e191107a37`.
It ran once on taskset CPUs 4–7 with the original 512MiB / 64MiB / 180s
limits. No alias alternative was added to the guard.

The v4 CLI exited 0 after 3.807006s: one disassembly, two region lifts, one
pipeline / ten passes; zero native, C, Unicorn or three opt-in C++ component
calls. Source Graph bytes match before/after, including IGNIL and membership.
Source Graph unlifted markers are empty; this API does not separately expose
a private rebuilt-region unlifted list, so this is not semantic completeness.
Worker initial/final Z3 maps contain only the pinned wheel SONAME. Actual
private-region attributes and one LOAD-driven switch cover all 80 slots / 75
different expected targets. Oracle addresses only check coverage after the
lift; they never become known memory, replacement state, assume or selection
leaders.

| Kernel actual IR measurement | Raw before | Ten-step after |
|---|---:|---:|
| Basic blocks | 359 | 360 |
| Successor slots (duplicates retained) | 436 | 577 |
| Central cyclic-switch count | 1 | 1 |
| SCC containing central LOAD/switch | 208 blocks | 208 blocks |
| LOAD operations | 4054 | 126 |

Guest-memory is INCOMPLETE: `complete=0`, `budget_exhausted=1`, 72 Guest
LOADs, 69 Guest STOREs, 0 forwarded, 4362 alias barriers and 209 loop barriers.
MBA visited 687, queried 0, simplified 0, and skipped 111 for budget.
Predicates visited 157, queried 64, folded 1, unknown 0, unsupported 1, and
budget-skipped 123. Dispatch-specialize rewrote one branch and zero switches,
leaving one unresolved switch. Dispatch-deflatten visited one, rewrote zero
and left one unresolved dispatcher. Other steps returned SUCCESS. All raw
counters remain in `pass-report.json`. Skipped/budget queries are not complete
proof coverage and were not retried or given more budget.

The after IR retains this actual business cycle:

```
memory.valid57 -> bb_4625 -> bb_4384
 -> bb_4384.bb_4390_crit_edge -> bb_4390 -> memory.valid57
```

The table LOAD `%807 = load i64` and switch are at `after.ll:2552`. Its case
4625 enters `bb_4625` at `after.ll:547`, retaining mul/add/xor for
`acc += t + (x ^ ((c1 + 17U*r) + j))` (`transformed.c:537`, source case 21),
then returns toward the dispatcher. Saved-only independent forward/reverse
reachability confirms the complete 208-member SCC and every witness edge.
No business/error block was deleted to create the view. This is a syntactic
CFG witness, not a new feasibility or native proof. **Region coverage
recovered; dispatcher/business-loop removal remains incomplete.**

Cross-host IR compatibility is exact, without normalization:

- Actual `before.ll`, SHA
  `4315ddcd08fd7e26861197be9a550ca7a1eee685be4206cc56f8c828d07abc05`,
  equals every byte of frozen B's canonical raw (747,549 bytes).
- Actual `after.ll`, SHA
  `15b75e16cbcf7cda4fcea10711b6749e9595efceaa2ae30ef77808824c6ce252`,
  equals B's archived `product-O2/raw.ll`. The Mac saved audit opens the exact
  member from archive `9c623...` and compares all bytes, not just counters.

Old IR/reference provenance is reused; no old C/UC/native match is counted
again. New-host native ABI/compiler calibration remains pending. The actual
17-file / 2,072,823-byte output and plan/source/SONAME evidence are mirrored at
`/Users/c/Desktop/deob/validation-artifacts/progressive-flatten-rock5b-preflight-20261114-v1/ROCK-ATTEMPT-v4`.
Actual `REPORT.json` SHA is
`9b78761bd3e9afd9a9d07857818833320c7ecf97454d009f32e0798bb4431568`;
external `saved-audit-v4/REPORT.json` SHA is
`9167e885d384ce1c4100a7539092c60bae54fe89d1c2a2895005933f6dde70db`.

Five evidence roots (`source-v3`, `plan-v4`, `soname-binding-v4`,
`decoded-main-v4`, `saved-audit-v4`) total 30 regular files / 2,143,298 bytes.
All member sets, sizes and SHA256 match on ROCK/Mac; source/input/tool/runtime
pins and logical/resolved paths were rechecked after execution. Certificate
`V4-FILESET.json` SHA is
`62d34733e86945dbfded54ca44520f9a841ccc2785959a14f0a6fc7d9ba3af93`;
`V4-DUAL-VERIFIED.json` SHA is
`697462829a4cbaba2ebe499f433c9c02bf2f7fa70070715ce66a58b0f87c0937`.
They are at the owned ROCK root; Mac equivalents are `FILESET.json` and
`DUAL-VERIFIED.json` in the mirror above. Compiled libraries are external
hash references. These five roots are frozen; no further target or mutation
is implied.

## Later portable before calibration (not implemented or executed here)

A separate fresh plan should bind this run's actual raw decoded-region LLVM,
actual ModuleID literal, original ELF, unchanged `sources/frozen-driver.c`,
all 16 `inputs/input-N.bin`, all 16 `reference/expected-N.bin`, new Execution.h,
new host LLVM21 `opt`/`llc` and C compiler. Use O0 and O2 with the frozen
16-value domain. Recheck the complete 66,136-byte protocol (24 header + 512 raw
registers + 65536 Guest + 56 Exit + 8 budget), all 76 named register fields, root RET,
and exact EOF; preserve zero skips/exclusions/normalization.

First execute the new raw LLVM 32 times and compare to the old 16 UC snapshots,
then only after all 32 match execute the pipeline after 32 times. Each 32 group
needs the unchanged 96 exact-field comparator controls. Saved C16/UC16 are
historical references and are never counted as new ROCK executions. A tool or
ABI mismatch stops at the new before calibration with raw evidence; do not
modify inputs or the comparator to obtain an after result. Logical-size and
guard-miss domains, if later requested, need their own original-LLVM peer
calibration and cannot be labelled new Unicorn coverage.
