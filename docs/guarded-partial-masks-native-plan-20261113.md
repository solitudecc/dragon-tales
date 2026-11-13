# Independent partial-mask prediction domain

This new domain does not modify the sealed J or state-value oracle matrices.
Its pass is `guarded-partial-masks`; metadata remains the J untrusted table
schema. Candidate indices0/40 predict targets43/44, never contents that must
remain immutable. The function computes `(unknown & sext(fresh)) |
select(choice,0,40)`, then executes its original index/size checks, actual target
LOAD and seven-case/default switch. A same-SSA full fallback must remain.
Every changed result must report Incomplete; no range or definedness fact is
exported by the fixture or checker.

The six modes are plain, historical, table-before, table-after, frozen-bool and
tail-CALL. An unknown helper is in a distinct predecessor block followed by
`call.continue` and a CALL-free seed. Its analysis declaration has no effects
or alias promises. The actual independently linked C body changes register
state/bool, both predicted table cells, loop-limit storage and Guest bytes that
really alias registers when `regs=Guest+32`. Before-CALL snapshots and post-CALL
reloads have distinct SSA values. The historical mode combines a loop-carried
previous fresh bit with the pre-call snapshot, while the mask consumes the
current post-call bit. Helper writes force those values to differ. Latch has
two actual successor slots to the header, retaining corresponding PHI inputs.
A defined `add nuw nsw side,0` remains in the corridor; no poison is executed.

Each mode has336 distinct tuples. The base288 vary unknown0/1/39/40/41/63/64/
2^63/all-ones, current bool0/1, target43/44/777/default99999, limit0/3, size1024
and separate/actually overlapping callers. The additional48 retain unknown0/40,
both bits/layouts, target43/limit3 with logical sizes519/520/839/840/1023/65536.
No base tuple is removed. Total2,016 distinct inputs; all backing Guest storage
is allocated64KiB even when descriptor size is smaller. The raw helper may
legitimately write beyond the descriptor size but within that allocation.

Planned public API gates: six normal modes (five changed, tail-CALL unchanged),
wrong untrusted targets777, budget-one `[1,0,0]` resumption, malformed metadata,
low clone budget, zero rewrites, and unchanged old J/F/H controls. Three fatal
modules (direct undef, direct poison, constant oversized shift) are verified
structural refusal inputs and are never executed natively. A legal frozen
current boolean remains a normal concrete mode. Every output verifies; Module,
Function, arguments, globals, metadata, attributes and old fallback targets/PHI
slots remain checked. Historical/current values may not be conflated.

Per new build, clang21 O0/O2 give8,736 normal full-state observations,5,376
fault observations and96,096 comparator corruptions,42 streams/14,112 frames.
Normal includes4,032 before,3,360 changed ordinary,672 unchanged tail-CALL and
672 wrong-hint observations. Native stages/builds are not distinct input samples.
The eight actual faulty programs are duplicate/missing helper CALL, stale
unknown state, collapsed history, forced40, post-STORE target reload, wrong
join PHI and omitted777 fallback case. Every faulty program must match its
own sequential reference; every independently differing normal/fault pair is
required to be detected, with at least one specified detection per control.

Frames compare return8, callcount8 and the complete73,728-byte arena, including
all register/Guest/descriptor/config bytes and seven Exit fields. Every normal
frame receives eleven comparator byte corruptions. Exact stream sets, lengths,
EOF and complete model replay are rechecked from saved files. No normalization,
exclusion, skipped case or timeout increase is permitted. Native groups retain
the original180-second limit. Source/model/inputs/driver freeze before candidate
execution, and before-only model verification is separately reported. This
preregistration claims neither native candidate success nor real58 loop closure.
