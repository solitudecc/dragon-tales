# Partial Boolean and zero-mask predictions

`guarded-partial-masks` is a separate opt-in LLVM builtin. It uses the existing
untrusted `dragon.indirect-target-hints` and guarded-entry view. No new source
facts, calling convention, table immutability or alias assumption is added.
Existing J/F/H controllers and the default optimization pipeline do not enable
this mode. Run it explicitly after the earlier controllers if needed.

Every legacy prediction is tried first. Only a failed legacy query invokes the
new bounded retry. In that retry an unknown scalar i1 atom may supply the two
bit patterns0/1 as Cartesian candidates. Historical and fresh keys remain
separate; no cross-use identity or Boolean correlation is asserted. A historical
PHI still obeys the previous all-incoming rule and cannot be replaced by a fresh
edge's value. This retry retains MaxNodes128, MaxDepth32, MaxValues8 and the
shared one-million work cap. It is not the correlated scenario enumerator and
does not claim to complete a failed correlation proof.

If one AND operand is ordinary unknown and the other has a finite candidate
set containing zero, this retry may retain only the zero result. Other outcomes
stay unknown. Any visited explicit undef/poison, dependency cycle, out-of-range
shift, depth/node/value exhaustion or global work failure aborts the query;
these failures cannot be discarded as ordinary unknown. No operation, flag,
LOAD, freeze or dependency is removed from the actual execution.

For example, the source expression

```
(unknown & sext(boolean)) | select(boolean, 0, 40)
```

may predict0/40. Zero can be an impossible Cartesian combination, and source
hint bytes can be stale or wrong. These are target predictions only, not a
complete value set, a definedness proof or substitutions into the source.
The existing core executes the real prefix, side checks, stores and actual
target LOAD, then tests the loaded value against the predicted existing cases.
Its full fallback consumes that SAME LOAD SSA with every original case and
default. Mutable memory and incorrect predictions preserve their original
behavior. The new retry does not create a branch on its unknown Boolean atom.

The public options, hard limits and four partial counters match
`guarded-partial-corridors`. New bit/mask candidates always report Incomplete;
counts describe committed predictions rather than all runtime states or literal
switch instructions. A result with no new partial candidates does not certify
whole-program dispatcher coverage. A separate explicit invocation keeps this
retry from spending the old controllers' earlier pass budgets.

Implementation, old-code failure, full tests and independent native/reference
execution must be recorded separately. This document alone claims none of
those results, real-loop closure or improved program execution.
