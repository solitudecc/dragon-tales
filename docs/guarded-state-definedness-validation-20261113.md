# Define only the new state guard condition

The pre-freeze state-values implementation could newly branch on undef. A LOAD
may read uninitialized memory; masking the result to0..3 makes an original
`icmp ugt state,50` false, while a newly inserted equality to2 may still be
undef. LLVM makes branching on undef/poison immediate UB. A pre-existing ICmp
branch therefore does not generally prove the whole state SSA is defined.
See the [LLVM UB manual](https://llvm.org/docs/UndefinedBehavior.html#the-freeze-instruction)
and `state-value-definedness-independent-20261113.md` for the reference rules.

The fix freezes only the new equality's i1 result before its new branch. The
original state, original ICmp, seed effects, other uses and both fallbacks stay
unchanged. Defined values behave identically; the extra freeze controls deferred
UB without inventing a range, alias or memory fact. Guard support now costs
four instructions instead of three, including all discover/apply/core/commit
budget checks. Failed copies still roll back atomically.

Frozen source: `9b3a00b9f0dcb7f91819b6945bebd132b17fbc0b` plus patch
`61892ff16ee7a6e56e6cba22d828436c740d66bdeb363bd76efeed78277cf176`.
Production CPP SHA-256:
`ede0c514bdc2775d9fb483324fcd18cf4a5af8074e12739bf19680dd777031e1`.
New independent test source:
`62f4c2048cb8beb0e75c784542634733cbde15ff252f6753f6370dc017ccd053`.
The original regression's scenarios and data remain; its condition-shape helper
now strictly requires Freeze→EQ. Its previousf3eee source stays in prior commits
and archives. The source/old/current validation is saved in
`~/vmc-runs/guarded-state-definedness-20261113/`.

The final new tests compile and run on both pre-freeze libraries, whose API is
already available: two cases /184 assertions, both cases fail with17 assertions.
They detect the missing explicit freeze and a seven-instruction budget that
previously committed instead of requiring eight. New related focus passes
76 cases /3582 assertions in both builds. CTest passes1141/1141 with Z3
(112.89s) and1006/1006 without Z3 (103.18s). The initial test-only Module printer
type error is retained separately as a compile failure, not regression evidence.

ValueTracking queried after an equality is already consumed by a branch can
assume the transformed program is defined and conclude that equality is defined.
That circular context cannot certify the transformation. The independent probe
records EQ-before-use=false, attached-EQ=true, freeze=true. The explicit freeze
contract is the regression gate; the attached API query is supplemental only.
Likewise opt21 happens to simplify the source, bad candidate and frozen variants
to the same return: this is not an old native wrong-result demonstration. No
undefined-input native execution is claimed.

New frozen backends:

- main: `ed2aa79502e05b0e92df966f55d99e8aa816af4857583d0ae070b049ff54eca2`
- no-Z3: `7e4c844857d288a9a2e413d6d6f9c21023d7f7acfcf9f631a4b405efc9f9c189`

Both new libraries retain J41/I245/H94 IR and reports, plus K's previously
executed native IR exactly. These compatibility checks do not recount native
observations. Metadata archive3eb8c8f19554770fc387ec0ddb5c2dc80e406894f8c7f980ab7f7160f9f5f78f
contains856 independently dual-host verified payloads. Independent semantic
probe/checker archive3ace00f715357ffd03beaa356b34cc81d1b1597da8aafbff4adeb1134386a55b
contains40. Local copies are under
`/Users/c/Desktop/deob/validation-artifacts/guarded-state-definedness-20261113/`
and `state-value-definedness-independent-20261113/` respectively.

The unchanged fully defined native model is being rerun against both new
libraries using a separately frozen strict condition checker; results will be
committed separately. Earlier fixture counts do not validate arbitrary undef
IR and are not substituted for new-runtime results. Real-loop audits/replay and
main merge remain pending. No instruction/Unicorn/full69 corpus run is claimed.
