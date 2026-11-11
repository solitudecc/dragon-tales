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
