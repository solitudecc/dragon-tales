# Checked-corridor implementation and validation

This milestone implements the opt-in `guarded-checked-corridors` component.
Its focused/full and independent native checks pass in both builds. Real59
diagnostics, fresh full replay and actual combination-loop observations remain
separate pending gates. The previous M/V diagnostic remains T/T/T/F; no complete
real loop-de-flattening result is claimed here.

## Exact implementation

See [the component contract](llvm-guarded-checked-corridors.md). A separate core
entry retains selected initial root-PHI values and fresh inputs for every
actual fast backedge. Historical inputs use the existing snapshots. Every
operation, flag, memory access and side exit remains, and the complete fallback
uses the same actual LOAD and its original destinations. The original chooser
does not repeat. Original helper/result layouts, J/F/H/M and defaults remain.

The local reproduction base is `db72df7e6db2d2b0ba8dc402efec3c606d85ac0b` plus
candidate patch SHA-256
`9f5bc594f4aa462dd279a1ce3288bdf0f2414ed1b5989c2389d9ba04ac0711df`.
The actual VM build HEAD remains `cdb4f6b`, with the earlier M patch and these
candidate sources applied. The metadata archive records that distinction,
every source hash and the reconstructable actual worktree patch. A later
documentation commit is not represented as the build HEAD.

| Build | Backend SHA-256 |
|---|---|
| Main | `48b9b76481c93fd6f18f0a7aca6a913e44e72d8becdc1e6a948a25369d4acc85` |
| No Z3 | `ab748c4165795b0e2cb36b4e5c9fa55b90b60b0efa6101b0dcf6a7dc0d5032eb` |

## Regression and full checks

The final public test source is
`15219e5cbd996a1ae47325aa3c57f8804fbb2425a9925098230d6e857acdb958`.
It compiles against origin/main `c9cd781`'s exact41 headers and the frozen926a3abf
H2 runtime. All six cases fail because the new builtin is absent:13 assertions,
seven failures, including one extra nested Catch REQUIRE exception report.
Both adjacent pre-component M runtimes also produce those failures. This is
API absence, not old wrong native execution. The temporary old library was
restored from its exact archived gzip and retired after hash/maps checks.

Final new tests pass6 cases /690 assertions in each build. They include a real
historical input defined inside the original region, fresh loop backedges,
duplicate seed/terminal slots, liveouts, mutable-table stores and complete
fallbacks. Related tests pass87/4661 each. Full CTest passes1152/1152 main and
1017/1017 without Z3, on the owned Ubuntu worktree with two jobs.

Compatibility runs retain J41, I245, H94 and state-values41 IR products and
their report fields; K's actual native-fixture IR remains exact. M additionally
retains53 IR products and17 reports byte-for-byte, with53 damaged-byte controls
per build. Historical native observations are not rerun or counted here.

Implementation metadata archive SHA-256:
`6f8b939d25b586786b24e0f41b8deb72709d97a5a5b47323c643f6b04d48ad20`.
It has1324 payloads /45165755 bytes. Both hosts independently verify the exact
member set, every length and every SHA. Main/NZ libraries remain raw in their
separate frozen runtime directories. The initial runtime-freeze script's wrong
C-shim pathname failed before its manifest; the unique actual path correction,
unchanged backend bytes and preparation failure are recorded separately.

## Independent complete-state validation

The separately owned native model uses1536 distinct inputs across eight modes.
Each main/no-Z3 build passes6528 complete-state comparisons:4814 business exits
and1714 checked failures. Each detects2292 specified differences among4224
fault observations and71808 comparator controls. Each56 streams /10752 frames
are reaudited. No exclusions, skips or normalizations are added. Before-only
calibrations remain separate from these counts.

Eighteen API recipes pass in each build, including resumed duplicate slots,
wrong predictions, unchanged CALL barriers and budget refusals. Fifty-seven
structural corruptions are rejected;35 verify as LLVM IR, and none is executed.
Main/NZ produce58 identical IR files and18 byte-identical reports, with58
damaged-byte controls. Initial helper-lexing and invalid-mutant failures remain
in their own archives. See [the independent report](checked-corridor-native-validation-20261113.md).

Native archive SHA-256:
`28ab9f5c55a3a3f5392d4c588c32fc4b467e963b87ecba098c0d5787d1f09a99`,
1028 payloads. Root independently checks every payload and delivery source.
This does not add a machine-instruction/Unicorn/full69-input campaign or prove
the remaining real dispatcher closed.

Both packages are under their named `guarded-checked-corridors-20261113` and
`guarded-checked-corridors-native-20261113` directories in local
`/Users/c/Desktop/deob/validation-artifacts/`, with the VM paths and manifests
recorded inside. Required real validation must finish before merging to main.
