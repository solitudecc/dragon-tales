# Predictions from selected-path STORE values

The opt-in `guarded-store-corridors` builtin first performs its existing seed
STORE search with historical evaluation. Only when that search finds no finite
value does it inspect simple i64 Guest STOREs in the proposed copied path, from
last to first. The existing SSA controller and the default pipeline are unchanged.

A path STORE value is evaluated in the fresh execution view. Each PHI chooses
the actual selected predecessor in that path; a root incoming definition still
uses the existing historical transition. Thus an i64 STORE whose PHI has a known
50 arm and an unknown Guest LOAD arm can provide a 50 hint only on the known
incoming path. The other incoming arm remains unknown. An earlier i32 STORE
need not be interpreted or treated as an i64 write.

These values only choose candidate table entries. The transform copies the
original STOREs and LOADs in order, keeps bounds/side exits, compares the actual
target LOAD against the predicted target, and retains a full switch using the
same loaded value. It neither proves that the STORE aliases the state LOAD nor
that a later write cannot change the state or table. An unrelated STORE or an
overwrite can therefore produce a wrong hint without changing execution. The
existing CALL/volatile/atomic path barriers remain in force.

Each new inspected instruction, address check, path-index construction, value
visit and predicted address consumes the same bounded global discovery work.
Existing module and per-path budgets are unchanged. The original seed search
returns immediately on success, retaining its candidate priority; the new path
search does not override it. Unknown/cyclic PHIs remain rejected by the finite
enumerator. The API, builtin name, stats layout and generated-block suppression
are unchanged.

The new independent regression uses an i32-only seed and a downstream i64 PHI
STORE with known/unknown incoming edges. Additional checks cover old seed
priority, incorrect hints, state/table overwrites, writes after the actual LOAD,
CALL/volatile barriers, duplicate successor slots with bounded continuation,
zero budgets, module validity and retained same-value fallback. Old H already
has this builtin, so its failure is a precision comparison, not API absence.
Native and real-module execution results are recorded separately by integration.

The diagnosed H v1 real run had two distinct limitations. Attempts from
`memory.valid399` and `memory.valid1944` needed 11 and 12 blocks while only nine
remained in the module budget. Separately, the original `memory.valid129` STORE
used a PHI with known 50 and unknown LOAD arms, so it never entered that run's
15-candidate list. This extension addresses the second limitation without
raising a budget. A bounded second invocation may address the first, subject to
actual revalidation and separate evidence; neither implies full loop closure.

## Implementation milestone: 2026-11-12

Frozen H2 source `911f9044d09f76a9776d2eceda0ee95f25a81208734ccb473cb5691343fc1f18`
actually passes both focused runs (46 cases / 1700 assertions), main CTest
1111/1111 and no-Z3 CTest 976/976. The three new cases compile against the
preceding H1 library and fail in both builds: 43 assertions, three failed
`changed` requirements. These are precision failures with the builtin already
present, not wrong-value execution claims.

The main independent native run completed 10752 full-state comparisons across
the original eight and additional two modes, with 6144 faulty-IR observations
and 2260 specified differences all detected. All saved streams were rechecked:
50 original streams / 12800 frames and 16 additional streams / 4096 frames.
Original H1 versus H2 outputs are byte-identical in 65 stored IR files; main
versus no-Z3 has 94 byte-identical output files. These checks are compatibility,
not extra native executions. No-Z3 native and formal real-module replay remain
ongoing at this implementation commit. No new complete-loop result is claimed.

Both runtimes and source evidence are frozen under
`~/vmc-runs/guarded-store-corridors-20261112/` (`source-v2`, `runtime-*-v2`).
The metadata-v2 archive SHA256 is
`54ecb71dbeb4578d5f1ce3d50192937bdd619093fde495454a06da35bfc60301`,
with 54 exact payloads independently verified on the development host. Earlier
H1 metadata-v1 retains the exact old-test executables; duplicate expanded
executables were retired only after archive verification, with the restore
receipt `~/vmc-runs/ARCHIVED-TEST-BINARIES-RETIRED-20261112.json`.

The subsequent no-Z3 independent matrix and saved-stream audit also passed with
exactly the same per-build counts. Both builds have 3304 business returns and
7448 explicit boundary outcomes; all 2260 specified faulty-IR differences and
118272 single-field comparator controls are detected per build. See
[the independent H/H2 report](guarded-store-corridor-native-validation-20261112.md).
Its full archive SHA256 is
`64ab18793a776c9f146859b8115f994b79e8b0758578502fc9ba92f7c912a7c6`;
the integration reviewer independently checked all 1213 payload files. Formal
real-module replay remains pending at this validation milestone.

## Final real replay

The independently preregistered 47-step replay completed C16/LLVM16 complete-state
matches and detected all 144 specified controls, with no new exclusions, skips or
normalization. See [the frozen real report](guarded-store-corridors-real-validation-20261112.md).
The complete formal archive SHA256 is
`f1e830cced2b004c192eb389b4a965d28735fae685cad9c19131fab902221be0`;
all 586 payloads were independently verified by integration. The reviewer also
checked the exact four final export/native-compiler input bindings and all four
cff O0 input/output bytes against the earlier observer. That cff O0 IR is exactly
unchanged, so its four actual loop observations are reused, not rerun or recounted.

The first H2 invocation recovers fourteen combo O0 slots; one explicitly budgeted
resume recovers five more. Closure remains one of four programs. The residual
Guest90592==0 path uses initialization counter i13 (Guest[RBP-132], initialized
to zero and incremented in `memory.valid109`) as dispatch state when the list
has not been established. It is not another constant-50 arm. This dynamic entry
and all unresolved/default paths remain; no heap-initialization or callee-effect
assumption was added. The remaining live-out/precision limits are recorded in
the raw reports rather than summarized as successful fixed-point resolution.
