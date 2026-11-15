# Demand-driven mask memory refinement design — v3

Design/source preparation only; not installed, compiled or executed. v2 is preserved byte-for-byte. v3 changes one diagnostic gate only: before the outer protected-span failure, retain `if (!at && work.stats.budgetExhausted) return false;` so the innermost exhausted phase/message is not overwritten. Basis is root Image2a3f; BASE.json pins the full SHA. Prior proposal8956290b and D fragment4218bfc6 remain untouched. No public SDK ABI, default pipeline, Opaque policy or budget changes.

## Install points

- In LoopRanges public section add public-span-helper.inc after existing get(). WriteSpan is an internal type of an anonymous-TU class, not an installed API. getForWrite resets the old root counters exactly once and refuses use from an active observation/independent-anchor stack.
- Insert private-mask-scope.inc before rangeAtOriginalStore and replace that method. Existing three-argument calls retain Coarse/no-signal defaults; only the unified write helper can ask for Strong.
- In LoopRanges::visit only, use and-transfer.inc for operand demand and the constant/singleton AND upper bound. Do not edit the older Ranges class. Other scalar cases and the nonconstant AND fallback remain unchanged.
- Apply call-sites.inc only to loopWrites and the D trial's deferred alias loop. Keep per-source-STORE/alias Work charges and all call/read/simple-GEP/hazard gates. The helper returns a range only after the whole byte span is disjoint; this factors, rather than removes, the existing T/C/D overlap checks. C is included for every write except its actual selected seed.
- Do not change independentLoadRange's strict visits. Its independentAnchors state makes every mask Coarse even if the suspended parent was Strong; no new trial/assumption may open there.

## Numeric and scheduling rules

AND still visits both operands and checks their hazards. An exact constant/singleton side now returns0..min(a.high,b.high), because ordinary unsigned x&y cannot exceed either operand. A nonconstant/non-singleton AND remains the existing Full fallback.

The only stronger demand is a known same-width nonzero mask with bit0 clear. No special mask value/address appears in core. Low-ones masks, including&7/&15, remain Coarse even in a Strong outer expression. ResolveMemory=false from an enclosing mask still wins, so a hidden nested operand is not eagerly expanded.

Every original STORE gets fresh RAII mode/signal state. First run Coarse and validate its full span. Return immediately on a disjoint span. Null/type/hazard/budget failures do not retry. Only a valid ambiguous span that encountered an eligible weak mask may run one Strong evaluation of that same original expression/program point. Both evaluations use the same inherited depth, nodes, definition points and Work; never call get() again. Strong asks the existing complete observation-range resolver for eligible AND contents; it does not infer a content invariant from a mask.

Nested aliases start their own Coarse-first query. Nested anchors use Coarse/no-signal scopes and all active R assumptions stay disabled. Eligibility from a child/anchor cannot leak into its parent. Scalar active sets are isolated by the already reviewed original-STORE scope, but memoryActive/activeTrials/points/nodes/Work stay intact. All mode pointers restore on failure/budget exhaustion. Conditional load-cache rollback remains the D transaction's job. Closed load facts are retained as-is in all modes; a wide cached interval may limit precision, but no dangerous cache invalidation is added.

The existing per-STORE/alias charge covers the bounded span predicate as before. The additional retry pays its own scheduling unit, original-STORE scalar scope and all normal scalar/graph/snapshot costs. New top-level write scopes add their real fee; no cap or old traversal fee is reduced. Resource/compatibility failures must be reported, not fixed by resetting128/32/1M.

## Independent same-API fixture

New TU: /tmp/dragon-loop-memory-definition-tests/tests/lifters/TestLLVMLoopMaskMemoryRefinement.cpp, tag[image-loop-mask-memory]. It does not change frozen955ae/ad05/d87ef.

E1024 is initialized22 and overwritten in A by data&15 before B's E observation; it is therefore not a constant H_C candidate. B uses a small E LOAD beneath a low-discarding mask to form a work address below C4096. The mask itself is a pure xor of the existing C LOAD and a literal, so it becomes a known constant only inside the independently checked C hypothesis. Before that proof, official InstCombine cannot simply delete/reassociate the raw mask from assumed Guest contents. No new caller metadata supplies this value. Old no-H_C fallback still has an unknown mask expression and its existing coarse nonconstant-AND transfer.

Six positives exercise M0xfffffffc, M0xfffffffffffffffc, M0xfff0/shift4, actual i32 E/shift/mask, an already bounded inner&15 with NO E definition, and an unchanged low-ones&7 with NO E definition. The inner bounded case must pass solely from the corrected upper bound and must not force an E content proof. The&7 case is an old-success control. The64-bit mask can make coarse addition Full, so the child E-alias proof needs the unified D span helper as well as outer T/C handling.

Ten negatives cover E domain0..63 (includes an address hitting C), partial E write, missing entry definition, unknown call, unmasked E value feedback, overshift, volatile/atomic E LOAD, visible poison and fence. They require byte-exact source Module, all Function/Argument/BasicBlock/Instruction identities and Guest STORE count. Positives keep the actual E LOAD/use/type, actual C LOAD, Guest STORE count, target reads/fallback and no injected assume/unreachable/provenance metadata. An independent official mem2reg+InstCombine clone confirms E's actual LOAD survives the front normalization.

Expected old outcome is a precision refusal for the five newly enabled shapes, with the low-mask control already successful; this is only a static expectation until a fixed old SDK actually executes the identical TU. New source may expose a fixture/shape/cap problem and those failures must be retained. No old runtime error, native semantic result, complete B closure or final test count is claimed.

## Remaining risks

A mask remains an ordinary-bit bound, not a definedness or memory certificate. Independent anchor evaluation must never receive Strong or an active R. Unknown calls/writes/partial definitions/illegal shifts are still barriers, not opportunities for a second strategy. A stronger valid interval must still pass every original span check; it is never clamped to the desired region.

Double scalar traversal and newly needed complete E graphs can legitimately exhaust the same caps or consume Work before a later old-success H_C candidate. Preserve earlier A/B precision and all focused/full gates; report the precise exhausted stage before proposing optimization. A wide preexisting closed cache can also prevent a gain. Neither limitation justifies a reset, a new assumed table model or a sample-specific constant.

## Source handoff and validation status

The candidate full source and SOURCE-PATCH.patch were mechanically assembled against Image2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8. They are review artifacts only, not edits to the production worktree. Candidate SHA8292a2018bf77c83630b509d38f3eac0fb4e255dd624f85daccefba31a879e34; patch SHAca815c06a589cee78007f1bd0c8886eec969f6e732c9f169f28b5127cd987c43. Every other code-bearing fragment and the complete test source1d2c6bb0 are identical to frozen v2; V3-ONLY.patch records the single restored budget early-return.

No new native, API execution, compile, old-SDK regression outcome or full-suite result is claimed. The fixed old SDK must still execute the identical final test before implementation is accepted. The parent main/No-Z3 full-suite exclusive window is respected. Existing ad05 v1 failures, calibrated955ae v2 old/new results and d87ef literal-span evidence are retained separately and are not recounted as mask-refinement validation.
