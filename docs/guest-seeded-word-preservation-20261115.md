# Seeded Guest word preservation (2026-11-15)

This opt-in `ImageTableContext` increment closes a joint table/one-word
preservation proof. It does not complete the large Flatten sample's loop
removal, change the default pipeline, or treat an image section as immutable.

The initial register snapshot is already checked by executable entry guards.
A simple, independently constant, full-width Guest STORE must dominate the
lookup and every selected same-width Guest reload. A proof-only constant
map propagates those exact reload observations through unflagged integer
expressions. Every continuing write must preserve both the checked table
and the seeded word; an overlapping fixed write, an unclassified/dynamic
possible alias, a partial write, an unknown call or a fresh unproved
observation refuses closure. No CFG edge is pruned under the word hypothesis.
The actual LOAD instructions and memory/error checks remain executable.
Terminal old values keep their observation time; terminal fresh reloads gain
no word fact. All changes are confined to owned module trials and the
existing checked commit transaction.

Regression tests append two cases without changing existing fixtures or
expectations. Both old SDK configurations compile the tests and fail the
same eight positive precision assertions (2 cases / 826 assertions, 818
passed); the 28 negative configurations still refuse. New focused results
are 2 cases / 914 assertions and related results 14 cases / 2,273 assertions.
Full CTest results: main 1,241 / 1,241, no-Z3 1,095 / 1,095; the additional
146 main cases are the unchanged solver-dependent test set. The no-Z3 focus
actually mapped its own SDK and no solver. Independent native results are
recorded below.
These old failures establish a missing optimization, not a wrong native
result in the old code.

Independent native validation uses five new C/Python/LLVM source fixtures and
eight fixed initial arenas, including actual Guest/register overlap. Before
calibration adds 160 full 66,112-byte state matches and 4,960 comparator
controls; five deliberately faulty IR variants add 80 observations with all
36 specified detections. Final installed-SDK after execution adds 80 matches
and 2,480 controls per backend, 160 / 4,960 combined. Ten actual API calls
accept the two positive fixtures and retain byte-exact refusal of the three
negative fixtures in each backend. Four after saved-stream audits pass;
no-Z3 actually maps its own SDK without a solver. Before/fault counts are
referenced once, not rerun or recounted for each backend. No exclusions,
skips or comparison normalization were added.

The fixed large `flatten-switch-O2.elf` remains incomplete. SHA-256:
`bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`.
The unchanged A/B recipe checks SP/table alone, or additionally the initial
RSI word. B discovers the actual dominating pointer seed at Guest 51072,
64 bits, value 32768, and one selected reload. It then refuses the STORE in
`memory.valid125`: its conservative address range is
`[50992, 4295018287]`, 32 bits, which can overlap that word. A still lacks
an independently constant pointer seed. Both results preserve the original
module, with no budget exhaustion or native replay claimed. A further
state-sensitive array-index proof and whole-loop control-flow consumer are
required. Merely counting copied contexts or known indices is not the
complete de-flattening gate.

Evidence roots on ROCK5B (`/home/ubuntu/vmc-runs/`):

- `seeded-cell-tests-before-20261115-v1`: old main/no-Z3 regression failures.
- `seeded-cell-main-after-20261115-v3`: final core focused/related results.
- `seeded-cell-dual-tests-20261115-v1`: final main/no-Z3 suites.
- `seeded-cell-real-20261115-v1`, `v2`, `v3`: all real refusals retained.
- `seeded-cell-v2-exact-runtime-20261115`: exact intermediate runtime/source.
- `tigress-seeded-cell-native-20261115-v1`: frozen independent before/model.

The final source and SDK digests, native results, complete file manifests
and dual-host verification accompany these evidence roots. No instruction
lifter/Unicorn opcode campaign or whole corpus campaign is claimed here.
