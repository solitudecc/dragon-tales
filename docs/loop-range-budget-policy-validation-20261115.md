# LoopRanges known-leaf and same-root shape accounting validation

Three exact original Image translation units were compiled and executed with
one unchanged private unit driver. The only source overlays add a test friend
and rename three public Image symbols. Reversing those edits reproduces each
original source byte for byte. There is no publication hook, visitor mirror,
normalizer or input mutation. The included Image implementation is defined in
the executable; imported Phi/Opaque/Path/Batch/LLVM helpers bind to the fixed
777 SDK. This is source-inclusion unit validation, **not installed Image API
execution or Guest-native semantic validation**. New C/Unicorn/native counts
are all zero.

| Original source | SHA-256 | Actual unit assertions | Raw exit |
|---|---|---:|---:|
| b6, occurrence charging | `b6b18cc70504e690c1eb3671858309237a06b48451ff9ace8561f6ada91dc01e` | 17 pass / 4 fail | 1 |
| df, known leaves paid once | `df7746ffbf9b5fc9b90ce6c5667a938658dea6c3532228c87e5116cf0ec5a8a7` | 31 pass / 1 fail | 1 |
| 17, complete same-root Opaque credit | `17badf6503b87724228fad78ee3b424f94683e24afff1532017f3789b76cc544` | 36 pass / 0 fail | 0 |

All three compiles returned0 (64.53s,63.88s,65.88s). Actual units took less
than one second each. Every command used CPU0–3, one compiler process,180s,
512MiB startup and64MiB running floor. Sources, tools, exact public/private
headers, SDK777 and wheel Z3 were frozen before compilation and rehashed
after every command. Actual `/proc` maps and `LD_DEBUG` init paths confirm
only the pinned SDK and wheel solver. The nine complete generated IR fixtures
are byte-identical across all three executables; every fixture verifies and
is unchanged after queries.

The b6 failures are exactly the literal positive, shared H_C SSA positive,
independent H_C candidate positive and repeated Opaque-root positive. Df
passes the three known-leaf positives and fails only the Opaque positive.
Failed old positives do not execute their subsequent dependent assertions;
those gates are not counted as old passes or hidden as skips.

Forty distinct `select(c,1,2)` nodes plus a balanced add tree return40..80.
Df/17 pay121 nodes and199 actual Work per get. Another outer get pays121
again; getForWrite, including its cast, pays122. H_C tests use actual distinct
LOAD SSA values as conditional visitor inputs: a shared pair maps to1/2;
another independent candidate maps the same pair to7/9 and returns280..360.
These artificial immutable hypotheses are not a new memory-content proof.

The repeated Opaque fixture has20 uses of one complete root whose footprint
is8, under separate `&7` expressions and a balanced add tree. Df actually
refuses the whole query at the128-node policy;17 returns0..140 with67 nodes,
repays67 on another root, and68 for an outer write-root cast. A prior direct
Opaque cache warm is charged and does not substitute for LoopRanges paying
its first complete expansion. Twenty distinct roots sharing the same scalar
dependency graph still refuse; overlapping graphs do not merge credit.

All versions preserve the six rejection controls: Work198, an already-paid
literal used at depth33, different equal-valued H_C SSA keys, repeated
non-known SSA, a nested original-STORE query entered with100 spent nodes, and
different Opaque roots exceeding the complete footprint cap. Df/17's Work198
case consumes exactly198 Work and refuses despite121 nodes. The depth33
case still pays its visit. No root/candidate or nested query resets shared
Work; no whole IR, effect, branch, address or flag is rewritten by the unit.

One controller preparation error is retained separately. The first request
for `opaque-probe` was issued while its original compile was still running;
the guard rejected the absent executable. That empty directory and
`PREPARATION-FAILURE-v1.json` record0 unit/API calls. After the original compile
actually returned0 and its executable hash was frozen, one real unit ran in
`opaque-probe-v2` using the unchanged plan, source, inputs and assertions.
The preflight failure is neither a product failure nor a skip.

The phase1 cache/rollback suite is not re-counted here. This unit does not
revalidate D parent-child transactions or Strong/Anchor mode restoration.
Its result applies only to b6/df/17 with the common pinned helpers. It makes
no execution claim about430 or later consume-plan/effect-inventory sources,
and does not establish closure of the real loop sample.

## Frozen evidence

- ROCK: `/home/ubuntu/vmc-runs/loop-known-leaf-accounting-private-20261115-v1`
- Mac: `/Users/c/Desktop/deob/validation-artifacts/loop-known-leaf-accounting-private-20261115-v1`
- 96 regular payloads,2,617,442 logical bytes, plus `FILESET.json` and
  `CLOSURE.json`; no symlinks/hardlinks. The empty failed-preflight directory
  is included in the exact directory set. Both roots are frozen with no more
  writes.
- `SUMMARY.json`: `1a2d70991790a031a57472af64a0219c1534a32cf56a678aff1e0789fdc80de9`
- `SAVED-AUDIT.json`: `b9ea9843bfc69683af5a58630eef9ce84c24cfc3a789fb28e0b52c3f6a103fb8`
- `FILESET.json`: `1b3045892e31acb5fd3cb8ac3e53fe86e8a690d51b4d77e476c1b580f021b8bd`
- `CLOSURE.json`: `9501be8cecedf237f83729917a7e429cd6189342010bd2b8167894eade48cb04`
- External dual-host certificate, in both artifact-parent directories:
  `loop-known-leaf-accounting-private-20261115-v1-DUAL-VERIFIED.json`,
  SHA `afba314fc6ac6972177f0bfc9e769d7b39808a1599a71c4811fd22fb1bedff58`.
  It independently checks the complete set, every size/SHA, control hashes and
  before/after identities on both hosts. No whole-library archive is copied.

## Replay source

The following repository files are exact copies of the executed artifact
sources. The shared driver SHA is
`69be08e30033721571fa08fbdb6f48a2104d1f140534306afdc510ea224734f5`.

| Repository file | Name in a new owned replay directory |
|---|---|
| `scripts/validation/loop_range_budget_probe.cpp` | `budget_probe.cpp` |
| `scripts/validation/loop_range_budget_overlay.py` | `prepare_overlay.py` |
| `scripts/validation/loop_range_budget_plans.py` | `prepare_plans.py` |
| `scripts/validation/loop_range_budget_guard.py` | `guard.py` |
| `scripts/validation/loop_range_budget_saved_audit.py` | `audit_saved.py` |
| `scripts/validation/loop_range_budget_freeze.py` | `freeze.py` |

Restore only the three original `LLVMImageTableContext.{b6,df,opaque}.cpp`
members from the exact evidence set. The overlay generator checks all three
full source hashes and rejects any substitution. Run it with the new owned
directory argument, then the metadata-only plan generator, which checks and
copies the fixed SDK headers. This creates fresh compile/probe plans and
READY pins. Each compile must finish successfully before that plan's probe
is run. The guard is called with the actual plan SHA, stage and a new output
directory; use `opaque-probe-v2` for the final saved audit's expected layout.
Old raw exits1 are expected precision controls and must remain recorded.

The executed preparation-error receipt is historical evidence, not a failure
that should be intentionally recreated. A clean independent replay should
write its own truthful preparation status and audit namespace rather than
claim the historical accident occurred again. The saved-only checker is
specific to the frozen measured namespace and includes that incident check.
`loop_range_budget_verify_dual.py` is the separate complete dual-host verifier.
