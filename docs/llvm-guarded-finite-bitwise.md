# Finite bitwise predictions for guarded state corridors

This extension keeps the `guarded-state-corridors` API, default pipeline and all existing budgets unchanged. It adds finite APInt And/Or/Xor evaluation and a bounded prediction for an unknown integer AND a constant mask with at most three set bits. It does not add a proof of a jump-table bound, replace a state value or remove an original operation.

For two finite operands, evaluate the Cartesian product at the original integer width and deduplicate results under the existing eight-value cap. Each product is charged through the existing append/work path. This matters for addresses such as `(finite_state << 3) or disjoint table_base`: retaining the actual OR and its flags allows the controller to select table-entry predictions without rewriting the address arithmetic.

If exact enumeration cannot produce one operand of an AND, a literal constant mask with zero to three set bits supplies all of its submasks, including zero. A zero mask can therefore predict zero. Direct undef or poison inputs remain rejected. A dynamic mask is not guessed. A wide/full mask with more than three set bits cannot finite-enumerate an unknown source; finite operands can still be evaluated exactly. Sparse bit positions are handled at APInt width (up to the existing 128-bit cap), without host-width shifts or signed overflow. Bit scanning and each subset are charged. Existing expression-depth/node and global-work exhaustion remain visible; no diagnostic is reset because a mask supplies a prediction.

This is candidate generation only. The copied unknown source, LOAD, freeze and flags remain executable, as do the real target LOAD and its small-set guard. A wrong candidate or changed table still uses the same actual value in the original complete switch/default. The extension neither evaluates a Guest/heap condition nor assumes table immutability. It does not alter SwitchDetector's rule that a mask is not a proven table bound.

The motivating frozen D37 paths are:

- f-cff O2: `and i32 %R15D.27, 1`, then `or disjoint ..., 24`, then `zext nneg`; predictions 24 and 25 feed the actual incoming state PHI.
- f-combo O0 prefix: PHI 0/50, `zext nneg`, `shl nuw nsw ..., 3`, then `or disjoint ..., 90120`.
- f-combo O2 prefix: a select with values 0/50, `shl nuw nsw ..., 3`, then `or disjoint ..., 73792`.

These are concrete missing expression forms, independent of the previous run's incomplete status. The original combo O0 loop still reads its state from Guest memory; this extension alone does not supply that state. The combo O2 fallback-5 chain is also not an incorrectly classified guard: its prefix originally selected actual target 66038, and a second equal-address LOAD was correctly reused by GVN, proving the {66045,67495} prediction false. The earlier prefix OR must first become eligible for its own guarded prediction; the wrong-table fallback must remain valid.

Implementation milestone validation (2026-11-12): four new regression cases compile against both frozen F libraries. Three positive cases fail their `changed` assertions; the negative case passes (77 assertions total, three failures in each build). The builtin already exists in the baseline, so these are actual precision regressions rather than API-absence evidence. Both new related groups pass 39 cases / 1369 assertions; full CTest passes 1104/1104 and 969/969. Final production source SHA256 is `51fad81cbfa9d392068866396b40de887dfd947f715b09d8dcc558c4a95dbfac`.

Independent native comparisons and frozen four-ELF replay remain in progress at this implementation commit; final results will be recorded separately before merge. Initial actual real-IR probes still show only one of four conditional loops closed. Further gaps include repeated use of the same unknown i1 condition, finite STORE-derived predictions, and unknown CALL-return state. Neither completed discovery nor successful verification establishes full loop closure.
