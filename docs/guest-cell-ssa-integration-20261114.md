# Selected Guest cell SSA integration — 2026-11-14

Base: `17f4f7c` (the predicate scheduling fix and its sealed validation).
This milestone adds the selected-original-LOAD helper described in
[`llvm-guest-cell-ssa.md`](llvm-guest-cell-ssa.md) to the production library.
It does not register a module scan, a default pipeline step, or a C/Python API.
Image-table context proofs and dispatcher decoder prototypes are separate work.

## Implementation and build provenance

The helper implementation is unchanged from the native-validated prototype:
`LLVMGuestCellSSA.cpp` SHA256
`d622bb18ca09472e9e3c3c477176e08666baf5ca3d1acd47daccf3349f209947`.
The declaration moves to the installed header
`dragon/lifters/llvm/GuestCellSSA.hpp`; the old private header forwards to it.
Declaration tokens, type layouts, namespace and mangled signatures are retained.
CMake registers the implementation and its 14 tests once each. The test bodies
and assertions are unchanged; only the include and an obsolete private-test
comment change.

Production build plan:
`/home/cc/vmc-runs/guest-cell-ssa-production-20261114/PLAN.json`, SHA256
`8568f8881036616a9dca311f022dffa8488794a37e39c4083d7d4f9ec7a752ee`.
It pins 404 actual inputs. Six previously retired runtimes are historical
metadata with explicit restoration archives, never missing compiler/linker
inputs. Each backend uses the original GCC `-O2 -g` compile flags and original
static LLVM archive list, adding only the new helper object. The current
predicate, checked-corridor and pipeline objects are preserved. No Image or
Decoder prototype is linked into these libraries. The first preparation
attempt rejected a linker flag as a file path before any compile/link; its
script and failure are retained, and the corrected preparation changes only
that metadata classification.

Builds and validation use the owned Ubuntu worktree, with sequential links,
1 GiB fresh-build/full-suite start, 512 MiB small-command start, 64 MiB running
reserve and the unchanged 180-second target-command deadline. The original
dirty checkout is not reset or edited.

## Regression and execution checks

Both actual production backends built successfully. The main library SHA256 is
`806d53d84291d4f0a79103255d7fa37c330cd02048bf2868182ba8d4a34ccfb9`;
the no-Z3 library is
`689e22c414ad617e16ec33bb550189d3ef563797c75c11aa4da60f17945ace68`.
All 404 input identities and hashes were unchanged. Each compile/link command
finished within 2.61 seconds and observed the original resource guards.

The same newly compiled test object links only to the selected actual library
and Catch2, without a helper object in the executable. Both backends pass all
14 cases / 261 assertions. `readelf` leaves the helper symbol undefined in the
executables; loader diagnostics bind it to the actual new libraries. Linking
that same object against old main32c fails with the missing new helper symbol.
This is API absence, not evidence of an incorrect old native result.

The cached historical CTest passes 1163/1163 in 89.31 seconds with Z3 and
1028/1028 in 76.20 seconds without Z3. The 14 new cases are reported separately,
not added to those historical discovery counts. Recent predicate tests absent
from that discovery pass separately: main 12/962 and no-Z3 1/5. The unchanged
checked-corridor clients also pass 16/3493 in each backend; an explicit name
comparison confirms these cases are not in the cached suite. All use the new
libraries. Three preparation/classification mistakes are preserved with their
original scripts and receipts; corrections change metadata classification only,
not core code, test bodies, inputs or comparisons.

## Formal native execution

`/home/cc/vmc-runs/guest-cell-ssa-formal-native-20261114-v1` independently
relinks the original producer object to each actual new library, without the
helper object. The producer imports the API, each new library exports it, and
each of the 12 product calls has an explicit loader binding to the selected
library. All 14 before/after/mutant IR products in each backend are byte-equal
to the frozen prototype products; all scalar statistics agree. Raw runtime
maps and binding logs remain separate provenance, never normalized IR.

The original oracle and prototype are unchanged. An exact bridge checks the
original manifest, C sources, input/expected hashes, complete calibration
domains and saved-stream hashes before reusing the prior 384 before matches
(192 C and 192 LLVM). These are references, not new executions or a per-backend
increase in the old count.

Each actual backend adds 192 transformed LLVM/Python full-arena matches over
O0/O2 and 64 specified bad-PHI native detections. Across both backends this is
384 new normal matches, 128 actual fault observations/detections and 4992
snapshot-comparator controls. Fault observations have zero normal matches and
are not added to the success count. The normal matches comprise 256 positive
and 128 refusal executions. All eight saved streams are independently audited
over their complete 512-frame domain, full 66112-byte arenas and exact EOF.
All 20 guarded link/emit/replay/audit stages pass, with no skips, exclusions or
normalization. The domain remains six fixtures / 96 fixture-input pairs /
32 distinct initial arenas; backend and host-optimization repeats do not
increase it. There are no new C executions in this formal after-only campaign.

The final combined evidence seal is pending. The older prototype counts below
remain separate from these actual-library results.

## Separately sealed prototype reference

The separately sealed prototype campaign is
`guest-cell-ssa-native-20261114-v1.tar.xz`, SHA256
`d380f02809a1d93916be05dd7ff89f957caabd712a29c1bef0b3971a4feecf3b`.
It has 384 before calibrations (192 independent C and 192 original LLVM),
192 transformed LLVM matches, 64 detected actual bad-PHI native observations,
and 7488 snapshot-comparator controls. Comparisons cover all 66112 arena bytes.
The domain is six source fixtures, 96 fixture-input pairs and 32 distinct
initial arenas; O0/O2 repeats do not enlarge that domain.

Native widths are 32 and 128 bits; 8/16/64-bit support has structural tests.
Positive fixtures normally have equal fresh/historical values. Their
loop-carried arithmetic is exercised, but the genuinely clobbered
fresh/historical split is in the call-after-STORE refusal fixture. Alias,
partial-write and most atomic/volatile barriers have structural refusal tests.
These limits must remain visible in any integration report.

## Remaining dispatcher work

A real `manual-O2` selected state LOAD can become a PHI in a separately checked
entry view. This is not full loop de-flattening. The remaining selector has a
multi-block PHI graph, bounds/error checks and live-outs. Standard scoped CVP,
O2 and the existing DFA threading recipe still retain a cyclic dispatcher.
Turning an AND-1 arithmetic state into an earlier control-flow decision also
needs a definedness argument; a finite numeric domain alone does not provide
one. No new assumption, freeze, table immutability or noalias fact is added by
this helper.

This milestone does not change instruction semantics, IGNIL lifting, ordinary
Graph recovery or the default optimizer. It is not a new instruction/Unicorn
campaign, full 69-input corpus replay, arbitrary-memory fixed point, or proof
that all CFF dispatch loops have been removed.
