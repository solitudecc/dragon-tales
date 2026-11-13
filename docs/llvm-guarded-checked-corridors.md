# Checked-edge continuation contexts

`guarded-checked-corridors` is a separate opt-in LLVM component. It continues
pre-existing `dragon.guarded-transitions.generated` branch edges and small
same-block actual-LOAD switch edges into original straight corridors. The
marker selects work; it certifies no state, memory, alias or reachability fact.
Original J/F/H/M controllers and the default six-step pipeline do not enable it.

The existing finite/correlated/partial-mask predictors supply untrusted existing
target cases. Every copied operation, arithmetic flag, condition, memory check,
LOAD and STORE still executes. Side exits retain their original targets. The
terminal small switch consumes the actual fresh target LOAD, and its default
retains the complete original switch on that SAME value. Wrong hints and table
mutation therefore keep their real fallback behavior. Calls, allocation,
volatile/atomic accesses and synchronization remain unsupported inside a copied
corridor. No helper effect or immutable-table assumption is introduced.

When a predicted original case returns to the first corridor block, that fast
case enters the copied header. Each copied root PHI has the selected seed's
actual initial value and one fresh incoming value per actual fast backedge
slot, including repeated switch destinations. A value from an earlier region
execution crosses the initial edge through the existing snapshot discipline;
it is not replaced by the current iteration's value. Non-root PHIs follow the
selected path predecessor on every iteration. The chooser before the seed is
not executed again. The complete fallback's loop case still enters the original
header. Other known cases enter their original business targets.

This is finite code duplication with an ordinary loop, not an inference that
an opaque path is impossible or permanently spins. Plain table reads and copied
writes execute on every iteration. Root PHIs and liveouts carry actual evolving
values. The existing transaction, SSA repair, identity and rollback checks also
cover the cyclic copy. Requests without a root-return case retain straight-copy
behavior.

Work is frozen before rewriting. Generated blocks are seeds only, never members
of a newly copied path. Blocks appended by this component also carry
`dragon.guarded-checked-corridors.generated` and cannot become its future seeds.
An explicit later invocation can process remaining original selected slots; it
cannot recursively grow contexts from its own clones.

Options retain `max_candidates=64`, `max_rewrites=32`, `max_blocks=128`,
`max_instructions=16384` and the previous hard limits. Predictor/search/core
limits are unchanged. New `loop_contexts` and `loop_backedges` counters count
committed cyclic copies and actual selected return slots. Other counters match
the partial-corridor interface. Any candidate keeps the result Incomplete;
these counters do not certify full dispatcher coverage or actual loop execution.

Implementation, old-code failure, full tests, independent complete-state native
comparisons and real-loop observations must be recorded separately. This
contract alone claims none of those results or full de-flattening.
