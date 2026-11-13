# Guarded state values: defined condition repair replay

The original concrete model, helper,1,078 input tuples, six faulty programs and
public driver are byte-identical to the first native preregistration. This
separate profile requires the new branch to consume exactly one freeze i1 of
its fresh EQ on the original state and requested literal. Original state
pointer/raw/state/ICmp definitions and fallback condition/slots remain checked.
An arbitrary freeze or a changed original comparison is rejected. The prior
fully initialized fixture results remain archived; they never covered undef
branch definedness.

The independent IR definedness witness and LLVM21 ValueTracking caveat are in
`state-value-definedness-independent-20261113.md`. That diagnosis has40 payloads,
SHA-256 `3ace00f715357ffd03beaa356b34cc81d1b1597da8aafbff4adeb1134386a55b`,
verified completely on both hosts. No undef program is executed natively.

The stricter public prepare actually runs all13 recipes on the old unsafe a2
runtime, then rejects its missing freeze. The API exists and no native execution
is attempted for that negative control. The fixed main/no-Z3 runtimes each pass
those13 recipes without changing counters' Incomplete contract, exact budget
rollback, seed effects, same-SSA full fallback, wrong hints or `[1,0,0]` resumption.

Each repaired build freshly executes clang21 O0/O2:

| Per build | Count |
|---|---:|
| Complete normal state matches | 4,928 |
| Business returns / checked failures | 1,664 / 3,264 |
| Fault observations / specified detections | 1,848 / 838 |
| Comparator corruptions detected | 54,208 |
| Fully audited streams / frames | 44 / 6,776 |

Return, callcount, the full73,728-byte arena, complete64KiB Guest and all seven
Exit fields remain compared without normalization, exclusions or skips. The
saved-stream checker replays every model frame and verifies exact file sets,
length and EOF. All group limits stay180 seconds. These are observations of
the original1,078 input tuples, not new distinct samples per build or compiler.

Main backend is
`ed2aa79502e05b0e92df966f55d99e8aa816af4857583d0ae070b049ff54eca2`;
no-Z3 is
`7e4c844857d288a9a2e413d6d6f9c21023d7f7acfcf9f631a4b405efc9f9c189`.
The identical public driver SHA is
`acb0e8c7d75cc97c7a935ff842c3ae68f6eb66f4ce05b2965e0094fc7942b163`.
Source, inputs, actual loader paths, runtimes and outputs are pinned before and
after. Main/no-Z3 retain41 byte-identical IR products and13 identical reports;
all41 damaged-byte comparison controls are caught.

The first launch copied driver contents without the execution bit and stopped
before any driver/native invocation. Its logs remain under label `main`; a
recorded mode-only repair restores the original executable mode without changing
its SHA. Successful fresh labels are `main-v2` and `noz3-v2`; the failed label
is not counted. The original model/inputs/thresholds never change.

All raw success, negative-control and infrastructure-failure records are sealed
in `guarded-state-values-defined-condition-native-20261113-v2.tar.gz`, with complete
dual-host file-set/length/hash receipts. This native fixture result plus the
separate IR definedness regression validates the component scope; real full-loop
closure remains a separate fresh-profile audit, replay and observer requirement.
