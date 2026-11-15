# Mask memory refinement: independent source review

This is a read-only review, with zero compilation or target execution. The
reviewed v3 fragment is
`/tmp/dragon-observation-range-closure/fragments/mask-refinement-20261115-v3`,
MANIFEST `b7eab370dddbcb19bd75b9fd4982eef2a9373b0eeaada47c845e0c03e588bc8b`.
Its mechanically assembled candidate is
`8292a2018bf77c83630b509d38f3eac0fb4e255dd624f85daccefba31a879e34`.
The complete candidate/base2a3f difference was independently reconstructed
and equals SOURCE-PATCH exactly, SHA
`ca815c06a589cee78007f1bd0c8886eec969f6e732c9f169f28b5127cd987c43`.
The entire source prefix before LoopRanges is unchanged. No old Ranges class,
installed header, public API, default pipeline or budget is changed here.

No new soundness blocker was found under the existing immutable Function/H_C
contract. The min(a.high,b.high) upper bound for singleton AND is valid for
ordinary unsigned bits, with result lower bound zero. It asserts no noundef,
stability, noalias or memory content. Non-singleton AND retains its old fallback.
Both operands are still visited and original operations/consumes remain.

Coarse traversal still cuts memory dependence below known masks. Only a
same-width nonzero known mask whose low bit is clear marks an eligible weak
encounter. One Strong query is permitted only after a valid, correctly typed,
non-budget-failed coarse interval ambiguously overlaps the complete protected
span. Null, explicit hazard, invalid shift, non-simple LOAD and budget failures
do not trigger this retry. Strong uses the existing complete observation
resolver rather than inventing cell contents from the mask. Low-ones masks
such as &7 and &15 retain their old cycle-cutting behavior, even inside Strong.
The same original STORE, scalar depth, nodes, definition points and Work remain;
there is no second root get or counter reset.

Each original STORE owns an RAII mode and eligibility signal. Nested alias
queries start their own Coarse scope and local signal, so their eligibility
cannot enable the parent. Anchor evaluation disables Strong and signal writes
through independentAnchors across the entire stack. Destructors restore the
previous pointer on ordinary failure, budget failure and unwinding. Scalar
active-set isolation preserves the already reviewed historical STORE scope;
memoryActive and trial ancestry are unchanged.

Closed numeric LOAD facts remain valid under the same H_C regardless of which
precision mode obtained them. Conditional descendants still belong to their
parent D transaction and are rolled back on parent failure. No D interval is
exported to ConstantFacts, PhiConstantGraph, Path or Opaque. A previously cached
wide interval can prevent Strong from improving precision; retaining it is
sound and is not permission to invalidate caches or increase a cap for a gain.

Every continuing STORE must be disjoint from T and selected C simultaneously;
only the actual selected seed is exempt from its C check, never from T.
Deferred D aliases use the exact D span. Whole spans and overflow are checked,
not just starting addresses. Protected-span count is at most two and existing
per-STORE/alias fees remain. The extra attempt pays its own scheduling and
ordinary query costs. V3 differs from v2 by exactly one diagnostic guard:
preserve an inner budget-exhaustion result before emitting a generic overlap
message. All other v2 code/review fragments and the test source are byte-equal.

The reviewed new TU is
`/tmp/dragon-loop-memory-definition-tests/tests/lifters/TestLLVMLoopMaskMemoryRefinement.cpp`,
SHA `1d2c6bb0b30fba028a116855f9686ef2307f08774c96756204024c05185649da`.
Its E cell at 1024 is initially 22 and later overwritten with data&15, so it
is not a valid constant H_C candidate. E.B appears only in the trace STORE's
value and in the B address under test; outer address queries do not warm its
range through that trace. The raw mask is the real C LOAD xor a literal; only
the independently checked C hypothesis makes it constant. Upstream raw
mem2reg/InstCombine cannot assume those Guest contents. The control checks
that the E LOAD actually survives normalization.

The 32/64-bit masks, 16-bit mask plus shift4 and actual i32 variant can exercise
Strong. AlreadyBoundedNoDefinition must succeed from the min bound through an
inner &15 without proving E memory; LowMaskNoDefinition retains the old &7
success control. The ten negative modes retain full module/object rollback,
partial/missing initialization, real clobber, unknown call, cycle, illegal shift,
observable LOAD, poison and fence barriers. Positive LOAD/use/type, STORE count,
target/fallback and no-assume/no-unreachable gates remain. These are static
expectations only. The first old/new execution must preserve any unexpected
parse/normalizer/cap/precision failure rather than tune the program or limits.
This review claims no new closed real loop, native match or completed test.
