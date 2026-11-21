# Loop arithmetic and projected-observation validation (2026-11-16)

This directory is a replay source set, not a product pass. Runners execute only
from a fresh owned evidence directory with an explicit frozen `RUN-PLAN.json`
(or temporal `PLAN.json`), source/input hashes and absolute compiler/runtime
paths. Each child retains the 180s / 512MiB start / 64MiB floor contract.
No file is deleted, normalized or silently skipped.

The normal fixture has two new sources (`sparse32`, `wide64`) and sixteen fixed
initial arenas: 32 distinct source/input pairs. Its C reference uses ordinary
unsigned multiplication; the independent Python specification uses repeated
addition and modular shift-plus-add. The full 66,112-byte arena contains Guest,
possibly aliased register bytes, all Exit fields and the return snapshot. LLVM
is an input implementation, never the expected-state source. Unsupported-PC and
guard-miss inputs remain in the compared domain. Source generation is emit-only:

```
python3 -B source.py NEW_OUTPUT /path/to/LoopRangeArithmeticFixture.hpp
```

The runner first compiles a `BEFORE_ONLY` parser whose dynamic imports contain
no Image API. All C/Python/LLVM before calibration, comparator corruption controls
and bad-IR controls must pass before compiling the public-API after producer.
The latter imports the actual SDK, never a product object or private Image body.
Saved streams use the existing complete-arena comparator and single-XZ EOF/CRC
checker in `dispatch_corridor_native.py`; their exact source is pinned per run.

Observed intermediate main SDK `123947a4…` results, under
`~/vmc-runs/loop-range-arithmetic-native-main-20261116-v1/`:

- Before O0/O2: 128 complete C/LLVM matches against Python.
- After O0/O2: 64 complete matches; two actual public API calls closed, verified
  and preserved non-view identities. There are 5,760 normal comparator controls.
- Ten bad-IR variants across O0/O2: 320 observations, 192 predeclared triggering
  input domains detected by full-frame inequality, 128 nontrigger full matches.
  These cover address+1, skipped STORE, doubled seed effect, old/fresh confusion
  and missing fallback. Trigger counts do not claim predeclared byte offsets.
- Six saved streams / 512 frames passed complete-domain and EOF/hash audits.
  No Unicorn or new machine-instruction campaign was run.

The same physical plain-API test client fails old SDK `c320bf04…` with 8 of 218
assertions (four precision positives), then passes all 218 on `123947a4…` by
changing only the library search path. All ten semantic negatives pass on both.
An earlier fixture version incorrectly expected rejection after legal LLVM
refinement of `undef` / `freeze undef` to zero; its 16-failure logs and raw IR are
retained separately. The corrected negatives use an actual unknown address and
two independent real LOAD observations. No old wrong-execution claim follows
from these precision tests.

## Temporal observation counterexample

`temporal/` supplies a separate independent C/Python model. Before/candidate LLVM
is extracted byte-for-byte from the saved public-API test output, with offsets
and SHA in `EXTRACTED-IR.json`; no extra API call or manual IR repair is made.
An external counter is guarded at entry, then an internal two-iteration loop
reads `fresh`, writes 29 then 0, reads `current`, and guards only `current`.
Its first false guard retries; its next true guard consumes the older `fresh=29`.
The i32 STORE at 4008+3*29=4095 changes the seeded pointer at 4096 from 32768 to
32. The raw program therefore writes Guest[32]. A proof that transports the
current iteration's guarded observation backward across the loop incorrectly
publishes a Guest[32768] write.

The unreleased intermediate SDK `123947a4…` actually published that incorrect
candidate. Independent before C/Python/LLVM O0/O2 calibrated all 64 full frames
with 1,920 comparator controls. Its candidate after had 32 observations: sixteen
actual wrong executions at both specified fields (Guest[32], Guest[32768]),
and sixteen nontrigger full matches. Four saved streams / 96 frames passed all
EOF/domain/hash checks. Evidence is under
`~/vmc-runs/loop-projected-temporal-counterexample-20261116-v1/`.
This is an unreleased candidate defect, not a defect attributed to the previously
merged main. The final fresh-observation-point fix must separately refuse this
same public fixture without changing its IR. Final SDK binding is pending.

The first projection fixture also exposed a separate unsupported precision
boundary: InstCombine canonicalizes its guard to a wide AND mask and pushes
Trunc through Shl. That original failed expectation and private snapshots are
retained; the canonical-mask form is now an explicit conservative-refusal
boundary. The supported witness fixture uses the real sample's OR/SUB signed
guard and i32 multiplication, with observable full-width Guest trace stores.
No `xfail`, exclusion or claim that the original precision gate was fixed is
introduced.

Current result counts above belong to the explicitly named intermediate SDK.
Any final main/no-Z3 transfer must bind actual public-API reports and byte-exact
IR; it must not silently recount these native executions as fresh runs.

Both native evidence roots are frozen, including complete source, streams and
small compiled products. Their exact sets/sizes/SHA were independently matched
on ROCK and Mac by the retained `verify_files.py`:

- Math `FILESET`: `1d85ed9f1715a02a62b9152fa8867441bda0f2a65d9fc6fe8b5017a8c8519ee0`;
  404 payload files / 73,121,112 bytes (plus FILESET and closure).
- Temporal `FILESET`: `64b38c50b96481eca94bcc5abb146260dcff8efbe875f1c71a3362b0c35fbd78`;
  65 payload files / 13,031,432 bytes (plus FILESET and closure).
- External dual-copy certificate:
  `~/vmc-runs/loop-range-arithmetic-independent-dual-20261116-v1/DUAL-CERTIFICATE.json`,
  SHA `b301061ab9fe78fb5d9ca6b71fdcdf9dc49984470f95516c66985514de7df743`;
  Mac mirror under `validation-artifacts/` with the same basename.

This is complete expanded evidence with exact file-set certificates, not a claim
that a new compressed archive or complete SDK/OS backup was created here.
