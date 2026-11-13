# Partial finite-address subsets: independent validation (2026-11-13)

The L2 change is tested using frozen `254d024 + 4cad7319` runtimes, independently
of the subsequent state-value component. Main backend is
`d33340910d58a340c0278fba19a9eafb3e10967e3dac287154e69a33ac2efd66`;
no-Z3 is `75ce04792e413360bc6ce3dc1133e8e70f6fbd6e2320ef599c29065521d85fb0`.
This validates partial untrusted target predictions and their actual-read fallback;
it does not establish complete loop de-flattening.

## Frozen independent domain

`partial_address_subset_oracle.py` is
`85bc64f185b1456be3b07796e25183b5bb204ebf6f8d68128236dee90a9ed889`,
public driver is `33c4b18afe5053f35b96778198317eb153e81e8ad4857becb1e816f0dd996342`,
and API checker is `31e400f84ca19cb5f76e2eb108c69f90597e657f9e0187c32dc9c6e2bdea2126`.
The pre-execution declaration is
`a60dd93bb16d4b8d934f92dc5ddbe3d628b2c57cdb3495a02eccaaa22d3a621e`.

The scalar model calculates states 2/41 directly from the post-call condition.
The LLVM fixture independently expresses them with select/or-disjoint/add-nsw;
the unconstrained prediction enumeration has eight addresses. All actual
flagged operations are defined for this fixture. No hinted target replaces
an actual memory read. Four modes retain a CALL before the corridor, a STORE
before the target LOAD, a STORE after that LOAD, or an unknown CALL within the
proposed copied tail (refused). Duplicate switch/branch predecessor slots,
loop-carried history, complete Exit stores, and actual `regs = Guest + 32`
overlap are exercised.

There are 384 unique frozen inputs (96 per mode), two condition values, loop
limits 0/2, seven data values including sign bit and all ones, targets including
valid business, central and default destinations, and exact table-boundary
sizes 535/536/847/848 plus 65536. Eight hint profiles cover missing first/last,
a 7-byte first row, a non-case last row, all missing, all non-case, deliberately
wrong valid targets, and a complete eight-address map. Four zero budgets,
strict F/H modes and malformed metadata are additional API checks.

## Actual results

| Check | Main | No Z3 |
|---|---:|---:|
| Public API rows | 62 pass | 62 pass |
| New precision cases | 15/15 | 15/15 |
| Old J precision cases (same API) | 0/15 | 0/15 |
| Full-state native normal observations | 6912 | 6912 |
| Native fault observations | 1344 | 1344 |
| Specified faults detected | 824/824 | 824/824 |
| Comparator damaged-field controls | 76032 | 76032 |
| Saved streams / full frames audited | 86 / 8256 | 86 / 8256 |

Native observations use clang-21 at O0 and O2. Every normal comparison checks
return, CALL count, and the entire 73728-byte arena including Guest memory,
register bytes, memory descriptor, configuration and all seven Exit fields.
Each build has 4032 business-return and 2880 checked-failure observations.
The 6912 normal count includes 768 before, 2880 newly partial transformed,
576 full-map transformed, and 2688 unchanged conservative outputs. Historical
J native observations are not rerun or counted here.

Seven deliberate LLVM faults have the following specified detections per
optimization level: bypass CALL 96, stale condition 96, force first state 68,
reread table after a STORE 39, delete that STORE 45, substitute hinted content
44, and drop fallback case 24. Native bad variants match the independently
faulted model before being rejected against the correct model. Every saved
normal/fault frame was re-read and checked independently; this adds no native
executions. No exclusions, skips, field normalization, relaxed budgets or
comparison reductions were introduced.

All 62 main/no-Z3 API IR outputs are byte-identical. In each old/new comparison,
47 outputs remain byte-identical and the 15 intended partial improvements
change. The same byte comparator rejects all 62 one-byte damaged controls.
Old J's same-API runs have no generic correctness/identity failures; they fail
the new precision gate because they reject partially mapped address sets.

## Retained infrastructure failures and unused preparation

An unused alternate two-constant draft was prepared locally, then set aside
when the prior agent's intended independent scripts were discovered. It was
never compiled or executed and contributes zero observations. Its preparation
files are retained under `unused-preparation-v1`.

The first API launch used relative root `.` and failed to start `driver`.
The next driver was incorrectly linked against a second dynamic LLVM in
addition to Dragon's LLVM linkage; its 62 API processes aborted during
lifetime cleanup. Binary, ldd, logs and reports are retained. No native oracle
execution used either failed setup. Rebuilding the unchanged driver source
with the original validated J link recipe (`c++`, `-ldragon`, `-pthread`, no
second LLVM library) and using absolute roots makes both new and old API
matrices pass their respective fixed gates. Sources, inputs, model, comparator
and precision assertions were unchanged. The source-pinned final runs are
`main-v3` and `noz3-v3`.

## Real four-input tail9

The exact frozen I2 final modules were processed once with the original tail9:
K, J, five tiny LLVM passes, constant branches, and E. The original 47 earlier
runs were not rerun in this probe; composition totals 56. All four probes exit
0 and verify, with source/runtime/input/recipe pins checked before sealing.
The unchanged family audit retains every fast case and same-LOAD full fallback;
16 structural fault controls are rejected.

Original closure remains **true / true / false / false** for cff O0, cff O2,
combo O0 and combo O2. Combo O2 now commits 7 partial predictions (35 blocks,
672 instructions; no budget exhaustion, still incomplete), compared with 4 in
J. Its new three-case set retains target 68545 from extra predicted state45,
69064 from state2, and 67942 from state41. No edge is removed from the audit
because the prediction was spurious. The shortest remaining ordinary path is
a different CALL continuation through `bb_67608`, `call.continue188`,
`bb_66032`, and the original `memory.valid49` dispatcher. Combo O0 retains its
dynamic initialization-counter path and budget exhaustion. Neither combo is
reported as complete. No new real native replay or actual loop observation is
claimed by these IR-only probes.

Real archive: `~/vmc-runs/guarded-partial-addresses-real-20261112/guarded-partial-addresses-real-20261112.tar.gz`,
SHA256 `d669db7317d51820a7fccf7ce30032467c2e510dbe82d320d89f26559f00a95d`,
136 payloads, exact file-set and every payload verified on both hosts. Native
archive is `~/vmc-runs/partial-address-subset-native-20261113/partial-address-subset-native-20261113.tar.gz`;
its manifest binds all final streams, failed attempts, sources and reports.
Mac archives are under `/Users/c/Desktop/deob/validation-artifacts/` using the
same namespace names. Runtimes remain in their separately managed frozen
source namespace. No machine-instruction/Unicorn or whole-69-input campaign is
claimed by this component validation.
