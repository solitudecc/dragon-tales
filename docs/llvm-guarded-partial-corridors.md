# Guarded partial state predictions

`guarded-partial-corridors` is a separate opt-in LLVM builtin. It can predict a
nonempty finite subset from a select whose other arm is unknown. These values
are deliberately incomplete target hints, not value facts, proven bounds,
callee effects or immutable table contents. The default pipeline and the finite,
correlated and STORE controllers do not enable this mode.

The builtin first runs the existing strict finite and correlated prediction,
including I2's dominating historical-Boolean-PHI identity retry. Only a failed
query invokes the new bounded mode. That mode evaluates both arms
of each select; if exactly one yields finite values, it may retain that arm.
Unknown operands outside a select still make the containing expression unknown.
Historical PHIs still require all incoming values; fresh PHIs still select the
actual path predecessor. There is no partial PHI merge or guessing of arbitrary
integer values. A visited cycle, direct undef/poison, expression limit, global
work exhaustion or known out-of-range shift rejects the partial query instead
of discarding that failure as an unknown arm. The old mask fallback is not
applied inside the partial retry. A nested partial select can contribute a
finite subset, with the incomplete classification retained for the whole query.

For `select(condition, post_call_value, 50)`, the candidate may be the table
entry associated with 50. The condition is not assumed false, and the unknown
arm is not replaced. A suffix begins after the actual CALL, retaining the CALL
and register reload. Copied stores, loads, flags, size checks and side exits
execute in their original order. The core checks the actual target LOAD against
the predicted set; its default switches on that SAME loaded value using every
original case and default. An incorrect hint or an unknown arm therefore keeps
its original behavior through fallback. The unknown arm may also reach a
predicted target by chance, in which case taking that exact existing case is
valid. No register-preservation ABI, alias or heap assumption is introduced.

The API is a new internal result wrapper, `GuardedPartialCorridorsStats`, holding
the old `GuardedTransitionsStats` and four counters. Existing overloads and the
old stats layout are unchanged. New counters are:

- `partial_candidate_edges`: distinct frozen original slots selected using a
  partial query, including candidates that may not fit a later rewrite budget.
- `partial_attempted_edges`: those partial edges actually attempted.
- `partial_rewritten_edges`: incremented only after the core transaction commits.
- `partial_unresolved_edges`: partial candidates minus committed partial edges.

If any partial candidate exists, the builtin reports Incomplete even when every
selected candidate committed and no budget was exhausted. Unknown outcomes
remain in the complete fallback; this status is not relabelled as a complete
value set. A finite-only run can still report Success and zero partial counters.
A repeated run with no new partial candidates cannot claim additional commits.

The same public options and hard caps apply: defaults are 64 candidates,
32 rewrites, 128 cumulative copied blocks and 16384 copied instructions, with
per-transaction limits of 32 blocks/2048 instructions. Discovery, path-index
creation, every visited expression and resulting candidate are charged against
the existing global work budget. Unknown CALL, volatile, atomic and unsupported
path barriers remain unchanged. Source manifest bytes supply predictions only.
The full original switch is not replaced by a claim that its other cases are
unreachable.

The four new structural regression groups verify post-CALL reload preservation,
committed-only counters, same-value fallback under wrong hints and writes before
or after the actual LOAD, rejection of missing finite arms/unknown outer values/
cycles/explicit poison/effectful paths, finite-mode output and counter
compatibility, duplicate-slot continuation and zero budgets. The old runtime
lacks this new builtin; old public-test failures are API evidence only, not an
old semantic failure. The integration stage records actual results separately.

## Independent native validation plan

Freeze a separate model and matrix before executing the candidate. An observable
external helper must really write registers and Guest memory and increment a
host call counter. Both select conditions, the known arm, and unknown values
including 0, 1, 49, 50, 51 and all-ones must execute. Include the unknown arm
reaching the predicted target, a different original case and the original
default. Test mutable table contents, a wrong source hint, a STORE before the
target LOAD and a STORE after it, short descriptor sizes, and overlapping valid
register/Guest mappings in a single model arena. Include loop reentry and live
outputs, preserving the original helper call on every relevant iteration.

Compare the return, complete register/Guest/Exit state and call counter against
an independent sequential model. Specified controls must catch bypassing the
CALL, reusing the pre-CALL register value, forcing the known select arm,
re-reading the table after its STORE, and dropping an original fallback case.
The source, normal inputs, expected control differences and precision gates
must be pinned independently. Native correctness, conditional fast-graph
closure, and actual repeated fast-loop hits are separate requirements; none is
claimed from creation of a partial guard alone.

## Integration after I2

This integration uses the exact I2 controller
`8f87744478c5570bd0496621ebb87e3315ae20f524b9dd787d954a68340293e7`
as its preimage. The earlier J-over-H2+I patch remains separate evidence; do not
apply its complete controller over I2. The combined controller keeps I2's
historical-PHI dominance/dependency checks and single-incoming root-PHI gate.
The core remains unchanged at
`16d19490590158413181d5cb5a171281d82fee291eb52688b4e1bd913e014491`.

Prediction order in the shared implementation is: existing finite SSA,
existing correlated Boolean scenarios, existing STORE prediction when that
separate mode is enabled, I2 historical-PHI identity retry, and finally the
new partial-select retry only when explicitly enabled. The J entry point does
not enable STORE prediction. Old state/STORE entry points pass no partial
stats and never enter the partial retry. The partial retry itself has no
Boolean scenario or historical-PHI atom mode, so it cannot turn a partly known
historical PHI into a partial union. Every successful earlier prediction keeps
priority over partial hints.

The original J regression source remains byte-identical with SHA-256
`266721cf9a5662f038dc9a177acbdf027a19ba6569ecea9bff5ac7b2be1c1c2c`.
No original native model, strict precision gate or K implementation is changed.
This source merge has not itself been compiled or executed; integration
validation must use the new complete source pins and retain previous results.
