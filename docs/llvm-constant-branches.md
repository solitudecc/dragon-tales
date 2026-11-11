# Constant branch cleanup

`constant-branches` is an explicit LLVM pipeline component. It folds conditional
branches and switches only when their condition is already a `ConstantInt` in
the supplied IR. It does not evaluate expressions, assume a bounds check,
interpret a hint or run `SimplifyCFG`, jump threading or a symbolic solver.

A suitable explicit tail after a guarded business-corridor transformation is:

    mem2reg, instcombine, gvn, instcombine, dce, constant-branches

The preceding LLVM passes can expose a constant state comparison. This component
then removes its impossible CFG edges without combining a dynamic target guard
and its fallback switch. Dynamic natural business switches remain switches.
Unknown, undef, poison and freeze conditions are not folded by this component.
It is not in the default pipeline.

The component snapshots the current literal-condition terminators. LLVM's
`ConstantFoldTerminator` updates their successor PHIs, including repeated slots.
Only changed functions then undergo pure entry-reachability deletion with
`EliminateUnreachableBlocks` and `KeepOneInputPHIs=true`. Every surviving dynamic
branch and switch remains outside the folding call. The generated-block
suppression marker is preserved when a terminator is replaced.

The Module, Function and Argument objects remain in place. The first version
rejects blockaddress, EH, debug and local-metadata identity before changing IR.
It verifies the input module after bounded scanning. Calls and stores on live
paths are retained; side effects in genuinely unreachable blocks may be deleted.

Options are nonnegative integers:

| Option | Default | Hard maximum | Scope |
|---|---:|---:|---|
| `max_folds` | 4096 | 65536 | Initially selected terminators |
| `max_instructions` | 1000000 | 1000000 | Module instruction scan |
| `max_blocks` | 65536 | 65536 | Module block scan |

All input checks and budget decisions precede the first fold. A zero fold budget
with an eligible terminator returns incomplete and leaves the IR unchanged.
The public pipeline reports candidates, folded branches/switches, removed
blocks, remaining constants, rejection and budget counts. Constants newly
exposed by PHI simplification are reported as `remaining_constants` with an
incomplete status; another explicit invocation can consume them. There is no
hidden unbounded iteration.

A successful cleanup is not a claim of complete de-flattening. Final acceptance
still needs full-state native comparison, fault controls and an inventory of
normal fast paths versus actual failed-guard/unknown-target fallback paths.

## Validation status (2026-11-12)

Baseline `3bcae99`. The final public-API six-case regression compiles against
the frozen baseline and fails all six cases (12 assertions, six failed) in both
main and no-Z3 libraries because the builtin is absent. This is an API/capability
regression, not an old incorrect execution result. Separate saved real before-IR
contains 25 literal cffO0 state branches; deleting those edges is a measurable
structural goal, independent of whether full loop closure succeeds.

Candidate main focused tests pass 22 cases / 741 assertions, including all
previous terminal and business-corridor cases. Full main CTest passes 1087/1087.
No-Z3 focus also passes 22/741 and CTest 952/952. Independent native/real
checks are now complete as recorded below; no full loop-closure claim is made.
Exact source and
old/new logs are frozen in `~/vmc-runs/constant-branches-20261112/`.

Frozen main backend `774c9fbb2b289fa95f42ce782424263ea67e9337c50bdfef2290d72bec993080`,
no-Z3 backend `ed9442eef0b05c8e7debe3814e4e84e582ffb9f7d44ee11fc4762b02428723bf`;
shims remain `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`
and `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`.
Python exact builtin inventory, opt-in default, unavailable empty module and
hard-limit checks pass as part of full CTest. No instruction/IGNIL semantics or
Graph targets change; no new whole-corpus or instruction campaign is claimed.

## Final independent evidence

Implementation commit `13f09d2`. Main and no-Z3 each have 3584 full-state
observations (1792 business returns, 1792 declared checked size exits), 1794
specified fault detections and 39424 comparator controls. Each build's 48 saved
streams / 6144 frames was independently rechecked; this does not add executions.
The 17 pre-existing literal terminators were consumed, and a separate duplicate
switch-PHI shape folds exactly one switch and one branch while deleting one dead
STORE block. Original 768 input/model semantics remain frozen. See
[the native report](constant-branches-native-validation-20261112.md).

The same four frozen real ELFs give C16/LLVM16 full-state matches and144/144
specified controls. Actual cffO0 IR folds25 literal branches and removes49 blocks;
comboO0 folds1/removes5; the O2 modules have no new literal conditions. Dynamic
actual-LOAD guards remain49/24/5/3 with their complete same-value fallbacks.
All four strict conditional loop-closure gates remain false; inherited incomplete
GuestSSA/dispatch reports remain explicit. See [real-program evidence](constant-branches-real-validation-20261112.md).

Mac evidence root is `/Users/c/Desktop/deob/validation-artifacts/`; VM root is
`~/vmc-runs/`. All packages have exact member sets and every payload SHA/size
verified on both hosts and independently by root:

| Relative archive | SHA256 | Payload files |
|---|---|---:|
| constant-branches-20261112/constant-branches-metadata-v1.tar.gz | ed3309ec59cc77763ac1433adf581d913263e3d6c9ca316694ad1859e3113bab | 52 |
| constant-branches-native-20261112.tar.gz | 468db785613171ba86e0fad8d0e751cf3eb66862966acf40b147a3d4ca648423 | 624 |
| constant-branches-full-real-20261112/recipe-B.tar.gz | 005a8827bf936a4f73eea3e7815956761bcffc05f230cb3ab908f1383a7414d7 | 481 |

Frozen runtime gzip payloads were also fully decompressed and hashed on both
hosts. Redundant raw runtime libraries and the real candidate expansion were
retired only after exact archive checks; adjacent retirement receipts describe
restoration. The archives, source, logs and final IR remain recoverable.
Next components must handle natural business target sets and SSA state edges;
real fast-loop executions must be observed in addition to conditional CFG closure.
