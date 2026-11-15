# Program-point ranges for proved SSA observations

`PathValueRanges` is a private, read-only component for the loop Image proof.
It consumes independently proved, stable, defined SSA witnesses, such as the
small PHIs returned by a complete OriginalIR low-bit projection. It does not
infer definedness from a LOAD address or accept caller-provided numeric bounds.

A range belongs to the exact observation at the queried program point. An
actual unique conditional edge must dominate that point, and the witnessed
definition must dominate both the branch and the query. Duplicate successor
slots imply neither branch polarity. A one-input history PHI is transparent
only when its input already dominates the PHI itself; a previous iteration's
value cannot become the current value by following an incoming arm.

The bounded decoder supports unsigned integer comparisons, distinct constant
arms of a select followed by equality/inequality, boolean extension tests,
boolean negation, true conjunction and false disjunction. It derives no arm
from false conjunction or true disjunction. Separate Freeze instructions are
different observations. No Freeze is looked through, no branch is changed and
no fact is exported as a general range for a value outside its proved context.

The memory consumer must range a stored value at its original STORE, an
intervening write address at that write, and a PHI incoming value at that
predecessor's terminator. A later successful LOAD guard cannot constrain an
earlier unknown stored value. Cached memory-definition ranges must therefore
stay independent of the later LOAD consumer's context. A graph-only read of
the function is retained until the consumer finishes; any CFG or SSA mutation
requires rebuilding this component.

Construction, decoding and queries share 65,536 work units at most, with
256 witnessed observations, 4,096 blocks, 8,192 decoded facts, depth32 and
256 nodes per branch arm. Exhaustion produces no publishable range and is
charged to the enclosing Image proof budget. This helper does not change the
legacy Image API, default pipeline, alias model or table permissions.

Validation is pending. New structural regressions exercise actual edge
dominance, two freezes of the same input, earlier STORE versus later guard,
duplicate slots, bypass paths and select/boolean polarity. These are proof
component tests, not native equivalence or complete real de-flattening.
