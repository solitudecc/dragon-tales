# Loop Image implementation milestone — 2026-11-15

The opt-in C++ `runLoopImageTableContext` now proves initialized loop-carried
Guest observations before using their ranges to check table and pointer-cell
preservation. It keeps the original memory effects and performs publication
only after complete closure. Ordinary/default APIs and C/Python ABI layouts
are unchanged. This milestone is **not complete large-sample de-flattening**:
the real B view still refuses, and new after-native executions remain zero.

Implemented components include OriginalIR low-bit PHI projection, dominating
path ranges, conditional constant-PHI graphs, bounded opaque PHI-region shape
checks, complete nearest-definition graphs and transactional exact-observation
range closure. No memory/table immutability or register noalias is assumed.
See the component documents and independent reviews for their contracts.

## Executed final checks

The compiled Image source is
`2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8`.
The main backend is `2c9ca7a777359f24b0ccf4a5bb88744297f1e0a86dc0376955c02310b20cb7ba`;
the no-Z3 backend is `cb1d1aec3e9f87df34091d7cf91088b13d4e54ad61142a1b5c7ffd75d9931645`.

| Check | Main | No-Z3 |
| --- | --- | --- |
| Loop context | 6 cases / 221 assertions | same |
| Depth accounting | 9 / 1210 | same |
| Observation memory graph | 3 / 844 | same |
| PHI-region shapes | 6 / 314 | same |
| Original opaque admission | 5 / 274 | same |
| Constant-PHI graph | 8 / 303 | same |
| OriginalIR low bits | 9 / 281 | same |
| Path ranges | 3 / 72 | same |
| Legacy Image compatibility | 14 / 2273 | same |
| Fixed-store spans | 2 / 4058 | same |
| Full CTest | **1305 / 1305** | **1159 / 1159** |

Both complete suites ran sequentially on the reference CPU4–7 / `-j4`
profile, with the unchanged outer900s and registered per-test limits. Main
CTest reported174.84s; no-Z3 reported166.05s. No skips, exclusions or disabled
tests were introduced. Source/runtime stat, ctime and SHA pins remained equal.

The first full run used CPU0–3 / `-j3` and failed7 of1305: four MBA precision
gates and three original180s test timeouts. The available MBA normal campaign
still matched32 native observations; each corruption campaign detected its32
differences. Unknown solver results do not identify a particular reason; MBA
defaults remain100ms, not the predicate default25ms. The first failure, all
generated partial outputs and the copying-checker ordering error are retained
in the independent311174660-byte failure archive described in the failure review.
The successful rerun changed the scheduling profile, not checks or budgets.

## Before controls and independent validation

These new APIs cannot link against `origin/main` at e952100; that is API
absence, not an old wrong execution. Executed same-public-API intermediate
controls additionally establish the actual gains:

- The unchanged24-layer depth client fails old b8 and passes b2438/1193.
- The same128-fixed-STORE client fails c535 at scalar nodes131 and passes
  0da2/4058, retaining every original fixed STORE and required LOAD.
- The final955ae memory fixture uses the same IR builder and negative inputs
  on both runtimes. Old c535 fails exactly six closure gates; the same client
  with2c9 passes3/844. Its independent upstream mem2reg/InstCombine control
  determines which index LOADs survive the existing frontend. Original v1
  failed assertions and their fixtures remain archived.
- Final Loop/Depth sources also execute against the unchanged public API:
  b348 fails exactly Gray closure and first-cell consumption (Loop6/210,
  Depth9/1199); the same physical client with2c9 passes6/221 and9/1210.
  All four runs preserve actual loader maps; no relink or Guest execution.

The actual-source inclusion cache experiment passes40 assertions; disabling
parent rollback produces six detected internal-state failures. Hook-off checks
the five common scenarios separately. A distinct orphan-subgraph control
passes8 assertions and detects two failures when only all-node coverage is
disabled. These inspect actual internal caches and spent debt; they are not
installed Image executions or Guest-native oracle results.

The independent low-bit component campaign previously executed128 complete
66112-byte matches and3584 comparator controls, tied to its exact component
source and historical runtimes. The real unchanged original LLVM was separately
calibrated32 times against frozen66136-byte reference frames with96 comparator
controls. Neither count is a new native validation of the loop rewrite. New
loop/table semantics still require an accepted-view after-native campaign
before merging this feature into main. No new instruction/Unicorn/full-corpus
campaign is claimed.

## Complete archive

ROCK root:
`/home/ubuntu/vmc-runs/loop-image-observation-range-milestone-archive-20261115-v1/`.
Mac root:
`/Users/c/Desktop/deob/validation-artifacts/loop-image-observation-range-milestone-archive-20261115-v1/`.

`loop-image-complete-source-six-products.tar.gz` contains1381 regular members,
910938498 raw bytes; archive335242431 bytes, SHA
`bd8d9b615ef7796fee22b47e2d78ae14242a45677a8dc44fdc932c604ea3311b`.
Both hosts independently verified the complete member set, every size/SHA and
single gzip CRC/EOF with no trailing stream. Certificate:
`bcf7549da2b65c2ec461fc2b85dfec52cfc609ac446b7016a32ac777328d1f1e`.

This includes1051 actual source files and six complete main/no-Z3 products.
The build host really has HEAD49f58f1 with owned source changes; it is not
claimed to be a clean Git build. Production `lib/include/tests/CMake` contents
match Mac e952100 plus the pinned final sources. Seventeen documentation,
validation-script and Git-note differences are listed separately. The archive
is not an OS/dependency image. No old evidence or expanded SDK was deleted.
