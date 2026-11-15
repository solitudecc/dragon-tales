# Read-only review of the Loop memory-definition graph

Reviewed fragment:
`/tmp/loop-image-memory-definition-fragment-20261115-v1/loadRange.cppfrag`,
SHA-256 `d4f93b7e9245fdfbf0e7b7263d5a0ebbc059005510fe5a691a136e6c5ea7f8c3`.
This review did not edit Image/CMake or run this graph in an SDK. The separate
depth-accounting execution is documented in
`loop-range-depth-accounting-validation-20261115.md`.

No new soundness blocker was found in the fragment's supported domain. For
one exact fixed Guest LOAD, the graph scans backward through actual CFG points.
A fully scanned single-predecessor corridor can share a node because no
intervening operation has been allowed to modify the requested cell. Every
instruction, block point and actual predecessor slot remains charged to the
shared work budget. Joins and nearest full-width definitions consume the
parent's remaining 128 definition-node allowance; nested LOAD queries do not
reset it. This is a bounded graph representation, not an increased recursive
memory-depth limit.

Points are `(BasicBlock*, exact before-Instruction*)`, with null meaning block
end. Consequently a LOAD prefix cannot read a later STORE in its own block,
and the block-end state used on a backedge is different. Owner maps are local
to one LOAD query; completed ranges are cached only by that exact LOAD SSA in
the unchanged Function/hypothesis context. Two LOAD prefixes in the same block
must never share a mere `(block, prefix-bool)` cache key across queries.

An owner link to an already scanned or pending segment is only a graph edge.
Success still requires every collected node, including every dependency
sub-SCC, to reach a real nearest exact-width STORE anchor. A good first arm does
not validate a seedless second component. Entry without a definition, partial
or possibly aliasing writes before the anchor, unknown calls and unsupported
observable memory operations refuse. Duplicate predecessors retain their
actual slot coverage and work charge. Matching full STOREs may stop the local
read-content search because they overwrite all older bytes.

Coverage is only one layer. Each non-target write encountered along a segment
must first have its address ranged independently at its ORIGINAL STORE and
proved disjoint from the queried cell. Each anchor's original SSA value must
also receive a complete range at that STORE. Their union is then a conservative
LOAD-value range. The graph cannot first use its eventual union to declare a
recursive alias check harmless. Unmasked nested reads of the unresolved LOAD
remain blocked by memoryActive, while an existing literal-mask opaque-bits
proof can independently bound a value without reading its contents.

In particular, the historical Gray fixture is not a memory-effect-free loop.
It has no new target-cell STORE in B, but B's work STORE address is
`4008 + 4 * LOAD(D)`. Proving that STORE disjoint from D itself needs D's value
range. Complete reverse-CFG coverage alone does not close this value/alias
self-dependency, and the current graph conservatively refuses it. The original
negative must not be silently reclassified from the phrase “store-free”. A
separate ranged-memory invariant would be a new supported capability.

The outer T/C proof still scans EVERY original continuing write and call.
Stopping a read search at a restoring STORE cannot hide an earlier T/C
clobber. Under a C hypothesis, every selected exact reload is dominated by its
independently constant real seed. A first-clobber argument validates those
assumptions only after the complete all-P joint proof; no CFG pruning or LOAD
replacement is permitted before it. New non-fixed/non-Guest simple integer
LOAD handling supplies only the type's Full ordinary-bit interval. It does
not imply content, alias, definedness or stability, and preserves the original
LOAD and fault behavior. Fixed Guest reads do not fall back to Full after a
failed definition proof.

For a future D-range extension, the invariant should concern only exact LOAD
observations O assigned R. Every such observation needs complete initialization
coverage before that observation itself; a later seed cannot justify an earlier
LOAD used in a deferred write. Multiple independent initializers require their
full entry cutset, not a substituted single-dominator requirement. Real
initialization ranges can be computed independently of D hypotheses, while
the already scoped external H_C remains conditional on the joint T/C proof.

With exact O-to-R proposals, every deferred write must independently prove
disjointness at its original STORE and every relevant full-store value must
fall inside R. New observations, anchors or a widened R require all dependent
range/alias results to be recomputed; partial proposal caches cannot escape.
The proof is by the first failing observation together with the first T/C
clobber, rather than by claiming that D belongs to R at every unobserved instant.
No missing initialization, partial overwrite, unknown call, unanchored component
or alias/value cycle can be accepted merely because a structural SCC exists.
All root nodes/points and Image work stay shared and spent across revisions.
R is not a constant or a Path stable witness, and D LOADs are not RAUW'ed.
This paragraph is a design boundary, not an implemented or executed result.
