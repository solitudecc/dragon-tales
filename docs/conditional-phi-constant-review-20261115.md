# Conditional PhiConstantGraph review

Read-only review of header `f08591207e36467cd0f52c9d257bdca9acbe65657302c3a3a1ab169602d00c9d`,
implementation `c3b913a5d57b84cb933ff48efce3f2e9fdf4b044ce4ab365ca91cc7a7aa15daa`,
and tests `e2fc32f9bce0bb93a180c2e5449bae4726c9849ceef5aabe64a49e848e0ddf19`.
No implementation edits, compilation or SDK/native invocations were performed
by this review.

No new soundness blocker was found under the valid immutable Function and
immutable H lifetime contracts. The extension accepts an incoming LOAD only
when that exact SSA key is present in H, with a non-null same-type ConstantInt,
same Function, simple access and no poison-generating annotations. It does not
look up H for PHIs, arithmetic, arguments or freezes. It does not establish an
address equivalence or reuse a fresh LOAD merely because its pointer equals
another LOAD's pointer. The original same-literal PHI closure, full actual-edge
coverage, all-component anchoring and prepayment before cache publication stay
in force. Returned values and caches are explicitly conditional on that H.

The outer joint proof has a well-founded first-clobber argument. Entry guards
establish T, and an independently constant real STORE establishes C and
dominates all selected reloads and cutpoints. Suppose S were the first original
P write to violate either invariant. Every dynamic H reload used by S's address
occurred earlier than S, after the real seed, and before the first violation;
therefore it had C's value. Pure PHIs only copy those exact constants or the
same literal, including historical loop-carried instances. The complete
original-P write proof then shows S is disjoint from T and C, a contradiction.
The seed itself is checked disjoint from T and writes exactly C's value.

This argument requires examining every original continuing write and unknown
call. A transient clobber followed by restoration must still fail. A seed
after a reload, partial-width seed, different Guest base, missing seed,
unclassified write or unknown call cannot be repaired by the PHI graph. Actual
bounds/fault operations and LOADs remain in the program. A terminal read or a
new same-address read absent from H receives no anchor.

Each C hypothesis needs its own unchanged overlay and fresh LoopRanges/graph
objects. Existing shadow-constant derivations may remain conditional facts in
the outer range walker, but the graph itself cannot trust an arbitrary H[PHI]
entry instead of checking that PHI's incoming values. No H-based value may be
published or used to prune CFG before the complete T/C proof. Consumption then
replaces only already-proved selected LOAD uses, retains the actual LOADs and
fault paths, and constructs fresh empty-H ranges for a literal-only recheck.
Prior conditional caches must not survive that mutation or escape into another
candidate. The caller continues to charge actual returned work to Image's
unchanged shared budget.

The new H pointer is appended after the old Function reference and both maps,
preserving their field offsets for controlled new-header/old-get comparisons.
Direct class clients must still be rebuilt with the matching new header for a
new runtime; the header has an inline constructor and changed object size.
This private-class issue does not affect clients using only the unchanged
public Image request/stats API. It must never be reported as an old semantic
defect.

The new source tests cover a conditional cycle, a fresh identical-address LOAD
without its own H key, wrong/null/type-mismatched H, different constants,
non-LOAD H keys, volatile/atomic/poison-annotation leaves, separate H scopes,
and budget failure before later success. This is a source review, not a claim
that those tests ran or that the real joint invariant or full CFF removal has
completed.
