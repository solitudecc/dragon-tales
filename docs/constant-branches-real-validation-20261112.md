# Literal branch folding: frozen real-program validation (2026-11-12)

Baseline is C milestone `3bcae99`. This validates the explicit `constant-branches`
component on the same four f-cff/f-combo O0/O2 ELF fixtures, original sixteen
inputs, source C reference, LLVM complete-state oracle and all fault controls.
It does not establish complete loop deflattening.

## Frozen recipe and runtime

The inherited B recipe remains prefix 128 blocks / 16384 instructions once,
three original eight-step rounds, five standard cleanup passes, then one
`guarded-transitions` controller with business_corridors=1, max_candidates=64,
max_rewrites=64, max_blocks=512 and max_instructions=16384. Its 31 recorded
steps and all GuestSSA/dispatch budgets are unchanged.

D explicitly adds one round of `mem2reg, instcombine, gvn, instcombine, dce`,
then one `constant-branches(max_folds=4096,max_instructions=1000000,max_blocks=65536)`.
Each added pipeline runs once, with max_pass_runs 5 and 1 respectively: total
**37**, not 31. No general CFG cleanup or SimplifyCFG follows the controller.
Inherited reports retain their original scope; the six D runs have separate
activation records and before-tiny/before-fold/final IR snapshots.

- Adapter: `scripts/validation/constant_branches_real.py`, SHA-256
  `f4c35425046fd1e5882ef4042ef9e2853130de15af92f2bfcdfe2fdc772b5449`.
- Pre-registration SHA-256:
  `9480a0e984d115628605592456ab414af7bd5bbf0d23ce70704de9fbd5d99850`.
- VM runtime: `~/vmc-runs/constant-branches-20261112/runtime-main-v1`.
- Backend SHA-256:
  `774c9fbb2b289fa95f42ce782424263ea67e9337c50bdfef2290d72bec993080`.
- Shim SHA-256:
  `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`.

The wrapper only appends stages at the frozen final controller invocation; it
never changes inputs, model, comparison fields or the existing pipeline's
reports. Runtime/source/fixture/contract/harness hashes were checked before
and after execution. No runtime libraries were duplicated into the real archive.

## Actual execution and transformed CFG

The new run took 103.50 seconds. C **16/16** and LLVM **16/16** complete-state
comparisons match. All **144/144** specified native fault controls were detected:
16 register, 16 Guest-memory and 112 seven-field Exit controls. All sixteen
input files are byte-identical to the frozen inputs. Source modules and Graphs
remain unchanged for all four cases. There are no new exclusions, skips or
normalizations. These are new executions; the saved-IR review adds none.

| Module | Folded literal branches | Removed unreachable blocks | Actual target guards before/after | D complete / budget events |
| --- | ---: | ---: | --- | --- |
| f-cff O0 | 25 | 49 | 49 / 49 | 1 / 0 |
| f-cff O2 | 0 | 0 | 24 / 24 | 1 / 0 |
| f-combo O0 | 1 | 5 | 5 / 5 | 1 / 0 |
| f-combo O2 | 0 | 0 | 3 / 3 | 1 / 0 |

There are zero folded switches and zero remaining literal terminators in the D
reports. Each surviving formerly literal branch has exactly its actual selected
successor; the impossible edge is absent in the transformed IR. Dynamic target
LOAD/equality guards retain their same-read complete switch/default fallback.
Every surviving ordinary business switch retains all original cases/default.
The earlier 25 STORE-edge rewrites and 290 controller block cost are audited at
`before-tiny.ll`, not inferred from final block counts after D deletes blocks.

The unchanged earlier continuation audit passes, including actual entry table
70032 guard, bounds-to-`bb_67010`, retained error exit and direct-exit bridge
checks. The independent inherited controller review detects eight malformed
structure controls; the D review detects six malformed guard/branch controls;
the graph checker has five synthetic controls. These are separate from the
144 executed native controls, not additional native samples.

The old three-round analyses still report GuestSSA incomplete 12/12 with 12
budget events, and dispatch-specialize incomplete 12/12 with 16 budget events.
D's complete status applies only to its literal-fold sweep, not the whole module
analysis.

## Remaining loop boundary

Strict guard-success closure still fails in all four modules. The remaining
original central selectors are `memory.valid13`, `memory.valid15`,
`memory.valid137` and `memory.valid49`, respectively. The audit excludes only
actual verified target-guard failure edges. It retains ordinary conditions,
every natural business switch case/default and call paths. File bytes are not
used to remove these edges.

A separate read-only projection of the earlier tiny-tail IR hypothetically
restricts natural business switches to six file-derived target candidates.
That projection removes the cff O0 central reentry, but not the other three.
It is neither implemented IR nor native evidence. The other shortest residuals
identify further distinct work: cff O2 has constant state 37 on a register-PHI
incoming edge, while the first combo O0 and O2 selectors have {0,50} PHI/select
states. Those facts motivate later guarded candidates; they are not assumptions
about table immutability, heap initialization or callee effects.

Independent `dragon.indirect-target-hints` extraction records pin the original
ELF bytes and six natural-business targets at cff O0 70040, cff O2 70104,
combo O0 90528 and combo O2 74200. These are untrusted candidate hints only.
They are not inserted into `guarded-entry.memory`, not used in this D recipe,
and require actual runtime target checks in a later component.

## Evidence

VM namespace: `~/vmc-runs/constant-branches-full-real-20261112/recipe-B`.
Complete archive on VM: adjacent `recipe-B.tar.gz`; Mac copy:
`/Users/c/Desktop/deob/validation-artifacts/constant-branches-full-real-20261112/recipe-B.tar.gz`.
The archive includes pre-registration, exact sources, commands, all execution
streams and fault streams, failure logs, 20 readonly inventory parses,
phase IRs, independent reviews and source/runtime bindings. `ARCHIVE-MANIFEST.json`
records every payload's byte length and SHA-256. The regular-file set must equal
that manifest's keys plus the manifest itself. Adjacent dual-host verification
receipts bind the final archive SHA and prove every payload, not only the tar.
No expanded evidence is retired by this report.

This campaign covers host-native full-root LLVM behavior on these four frozen
fixtures, not a new instruction/IGNIL campaign, the full 69-program corpus,
arbitrary black-box protection, or universal guard success.
