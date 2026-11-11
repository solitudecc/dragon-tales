# Checked Guest index dependency reuse: independent evidence

Baseline `40f733e`, implementation `02a71ef`. Reuse is limited to one fixed
corridor/predecessor proof, with no intervening IR mutation. The LOAD-index
and STORE-index dependency traversals share the successful historical set;
fresh and historical sets remain separate. Any traversal failure rejects the
whole candidate, and the sets do not survive that candidate or invocation.
This changes repeated proof work, not expression admission, poison handling,
address equality, memory aliasing, or the configured proof budgets.

Independent code review checked the early visited insertion: recursion may use
it for graph convergence, but the first traversal must return success before
the second traversal can consume the cache. A failed child aborts the entire
candidate. Reaching the same SSA value in a fresh view still performs its own
check. Fresh corridor LOAD/freeze rejection is not bypassed by a successful
historical check.

## New frozen native fixtures

`guest_index_cache_oracle.py` final SHA-256:
`b8b34b5119207e67cb10f7a331f0ff6b5ad29f8a96cf22fd45e23ed3d1f4d411`.
The unchanged production adapter is `guest_join_coverage_driver.cpp`, SHA-256
`1805db7cd7276985a42358ccf0a1d5cbd2b1fda4afa88a69428c237bc49c0868`.
It directly calls production GuestSSA with `runMem2Reg=false`, verifies the
module before/after, and links only the selected frozen backend.

Seven named modes use a balanced index tree with 64 distinct entry register
LOAD leaves and 63 additions. One mode freezes each already-executed leaf.
The inputs arrange cancellation with wrapping arithmetic so the resulting
Guest address is always inside the allocated 64 KiB, while leaves exercise
zero, one, the high bit and all ones. Known and unknown paths remain distinct.
Descriptor bounds independently permit or refuse the checked LOAD.

The positive modes are plain historical leaves, already-executed frozen
leaves, and an unknown write call before the last known STORE. Each must
actually redirect the known edge, remove the copied state LOAD, and feed its
copied result with the proved constant43. The original unknown edge, actual
LOAD and bounds check remain. Checking the copied value expression prevents
merely counting a clone or trusting missing metadata as proof of forwarding.

Negative modes are a call after the STORE, a different STORE offset, a fresh
corridor LOAD, and a fresh corridor freeze. They must not redirect the edge or
forward the state value. The call helper modifies Guest memory, a side cell,
registers and call count, so its surviving effects are observable.

The 336 frozen inputs are seven modes × four x values
`{0,1,2^63,2^64−1}` × two paths × three descriptor bounds × two frame addresses.
Every observation compares return, call count and the full 73,728-byte arena,
including 512-byte registers, all 64 KiB Guest, seven Exit fields and actual
memory descriptor bytes. The descriptor pointer is compared exactly using
the recorded allocation address; no bytes or fields are normalized/excluded.
These are host-compatible DL64 LLVM-transform fixtures, not new machine
instruction, Unicorn or full-corpus coverage.

Both old backends verify all seven inputs/outputs. The three positives have
zero forwarding and report proof-budget exhaustion; this is an old precision
limit, not old native wrong semantics. The offset-mismatch mode also previously
hit that budget before reaching its negative proof. Both new backends perform
one real forwarding in each positive with no proof-budget event; all four
negatives remain unmodified. The offset mismatch is now rejected without
exhausting the repeated traversal budget.

## Native observations and controls

Each new runtime independently executes before and after at host O0 and O2:

| Per runtime | Count |
| --- | ---: |
| Normal complete-state matches | 1,344 |
| Faulty-IR observations | 576 |
| Specified observable faulty-IR differences detected | 320 |
| Comparator mutations detected | 14,784 |
| Saved gzip streams / complete frames audited | 40 / 1,920 |
| Mismatches / exclusions / skips | 0 / 0 / 0 |

The six faulty IR controls drop the STORE, change its state value, drop the
unknown call, bypass bounds, force a constant on the unknown path, or corrupt
Exit sourcePC. Per optimization level their required detections are
24/24/24/32/8/48, all caught. Fault executions also match the independently
specified faulty model. Observations without a specified difference are kept
in the complete streams and not counted as detected faults. Comparator controls
mutate result, call count, registers, Guest and every Exit field using the same
equality helper as normal comparisons.

Complete saved-stream replay rechecks every byte against the frozen model and
adds no native execution count. The final audit also verifies source/input
identity, before/after runtime/binary pins, actual dynamic linkage, required
precision, retained unknown paths, and exact main/no-Z3 output IR equality for
all seven modes. Original input IR is unchanged by analysis.

## Historical LOAD/freeze and loop compatibility

The unchanged `guest_memory_history_native.cpp` and its original models are
reused only to generate nine known historical/loop fixtures under all four
old/new runtimes. Every before and after module is byte-identical to the
previously native-verified `new-expanded001` output from backend
`1b84e712011ef8bd19583924cdb34d8b06eadbdfd23e60e152a569c1d5d4fa5b`.
That prior model covers historical versus fresh LOAD/freeze, unknown entries,
finite loop iterations, side reentry, live-outs and fault controls.

The original six-mode whole precision gate remains **false**: its three old
single-predecessor deep shapes remain unsupported. Single3 and the separately
added legal multi-hub branch3 gates remain true. No old fixture or threshold
was changed. The prior 6,480 complete-state observations and 2,196 specified
fault detections are **not rerun or counted as new native evidence**. This is
explicit byte compatibility with those already-verified products.

The prior complete dual-host archive SHA-256 is
`31045d1616873f1d04f48f7b2a1cfb4c69c6404524adb3ee1eb882be2edd793e`.
Exact copies of its 30 IR files, original report, three frozen sources and a
reference manifest are included in this evidence. Current history source SHAs
match the prior report, so the comparison does not substitute a new model.

## Runtime pins

| Runtime | Backend SHA-256 |
| --- | --- |
| old main | `d6cfa1bfc4898abcd2ab771a368010c247b78eb98b27f93f9908b42e9ddd3c2f` |
| old no-Z3 | `c566a1e43fe067ff43d4b1fb13f8308c3db0b68d56d086d19cb75747429c7ab6` |
| new main | `b6a910b8ca34c2ce1d997cb936dcf68282aec0762cc29ac9a2fff6defc3c7f72` |
| new no-Z3 | `cf6078427bdd735580b9d5d1fce95cb68f163cedf5ea331464c6cb1b8c69023c` |

Main shim `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`;
no-Z3 shim `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`.
Only the backend is linked by the adapter, but both runtime files are pinned.
Root's build, unit and whole-suite evidence is separately archived and not
added to these observation counts.

## Preserved v1 fixture failure

V1 source `54bcb7f8056808dea14baf64a0a6bcd8423a1ce361776665b7d77e947a7f734e`
contained a handwritten decimal failure sentinel
`-4972005338616971008`, which is `0xbaffe37cc263c100`, whereas the frozen
independent model specified `0xbad0000000000000`. The first original before-IR
bounds-failure input therefore disagreed, before any optimized IR execution.
Its source, old/new adapter results, failure log/report and partial stream are
fully retained. It is a fixture failure, not a framework regression.

V2 changes that IR literal alone to `-4985484787499139072`. A textual/AST
comparison in `FIXTURE-V2-CHANGE.json` confirms that `parameters`, `initial`,
`model`, `same` and `run` remain identical; input generation and fault controls
are unchanged. V2 then repeats both old adapters and both new adapters/native
matrices from scratch in a separate namespace. V1 partial observations are
not added to the successful final counts.

## Complete archives

Archives are in `/home/cc/vmc-runs/` and
`/Users/c/Desktop/deob/validation-artifacts/`, with same-named expanded evidence:

| Archive | Files / payloads | SHA-256 |
| --- | ---: | --- |
| `guest-index-cache-native-20261112.tar.gz` (preserved v1) | 318 / 317 | `bd37719d0f2080a37ee46f493bda36435daad99cab48393e0596cc37255bc46a` |
| `guest-index-cache-native-v2-20261112.tar.gz` (final) | 659 / 658 | `e144136fdffd9a02a1da17db0289a90dcf1028a60d06923d9678304e133a38b4` |

Both hosts verify the exact tar file sets and every payload size/SHA; the Mac
complete expansions are independently checked too. Sources, all valid IR,
compiler/adapter logs, the v1 failure, original historical references, complete
compressed snapshots and fixture binaries remain in the packages. Production
runtimes are bound by pins and are separately archived by root.

The v2 README gives exact compile/adapter/native commands. To recheck saved
results without executing target code, use a separately restored copy and run
`python3 -B source/guest_index_cache_audit.py .`. Its absolute source/runtime
pins require the original paths to be restored; it writes reports, so avoid
changing the frozen expansion used by the manifest.

This proves safe reuse of historical dependency work and a known STORE-to-state
LOAD copy. It does **not** prove target-table contents, resolve an indirect
jump target, or remove an entire flattened loop. Those remain separate tasks.
