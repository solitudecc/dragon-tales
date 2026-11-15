# Opaque integer admission and explicit PHI-region policy

This is an independent read-only review. It performed no compilation or SDK
execution. Opaque admission is only permission to use the type's Full ordinary
bit interval. It is not a constant, noundef, no-overflow, equality, stable
observation, address or memory-content certificate. All original operations,
flags, LOADs, faults, control consumes and side effects remain executable.

The original `OpaqueIntegerAdmission::get` keeps its raw 128-node policy and
original function body. Its tests are a separate source from the new policy.
The explicit `getPhiRegions` first validates every reachable physical integer
dependency and actual PHI incoming slot. Its physical graph has a separate
65,536-value cap and shares the caller's unchanged work allowance, at most
Image's 1M. Only after full validation does DSU group directly connected
PHI-to-PHI shape regions. It does not merge their runtime values. Every
non-PHI operation, argument, constant, LOAD and freeze remains a separate
structural node. The complete compressed footprint is capped at 128/current
caller allowance. This is an explicit new abstraction policy, not a changed
default raw-node limit.

All physical-node checks occur before compression. Explicit integer undef or
poison cannot hide behind a previously seen PHI cycle or exact H shift-count
entry. Simple LOADs and freezes are opaque leaves, not equal observations;
freeze deliberately does not inspect its poison/undef operand. Shift counts
must be legal literals or exact, same-typed, point-independent singleton H
entries, and their complete integer dependency shapes are still checked.
Funnel calls retain the narrow existing mathematical-intrinsic conditions.
Other integer call/effect dependencies refuse. Non-integer operands and the
contents/addresses below opaque leaves are not a whole-program effect proof;
the separate all-P STORE/call checks remain mandatory.

The cache records only a fully validated root's complete footprint N. A hit
costs one work unit but must still fit the current N allowance. It is separate
from the old raw-policy cache. No partial graph or budget failure publishes an
entry. The consumer must account N including a root already charged by visit,
guard zero before subtracting one, retain budget-exhaustion flags and charge
all work. A failed physical collection reports zero compressed footprint;
a fully collected over-limit graph reports its required N. Raw physical
checking is never skipped because compression would make the count small.

Reviewed implementation snapshot: header SHA-256
`60b9d99750b673d72b430ff541c0da9721691d75f0a4f121dc55a1b6d95b5f7a`,
CPP SHA-256
`0202860fc9d227c6de10d48d66ca50b32069f9c49d408e50abd688893f284093`.
No Full-only soundness blocker was found under the valid immutable Function
and immutable H contracts. Consumers remain restricted to resolveMemory=false
after existing known/Path/constant proofs; this helper must not become a Path
stable witness or replace an unsuccessful fixed-Guest definition proof.

The new test snapshot
`c69b2e085ef66edde9826e6b2471c2ac3f88c2e458ab3ea08f960342ad6b4e2a`
has the owner-reported actual result of 5 cases / 294 assertions passing.
Its early-refusal footprint assertion was explicitly corrected to the new
zero-until-complete contract; unsafe refusal, budget and unchanged-IR gates
were retained. The old raw-policy test source was not changed.

The subsequent test snapshot
`114988d0b6e88c820cd82ef6b977aee9a773e9ee2b063b2031035c7be0f5629c`
contains six cases, adding a valid 1000-PHI graph with a late undef/poison arm
that must fail before compressed-cache publication. It was source-reviewed,
but was NOT YET RUN at this review cutoff. Its source adds no native execution
of poison/undef functions. Do not copy the earlier five-case measured count
onto this six-case source.

The owner's BBox/private-v5 diagnostic post-assertion error remains preserved
with its original evidence. This reviewer did not execute or independently
verify that diagnostic receipt and does not reclassify it as a product PASS,
native match or discarded failure. The owner must supply its exact receipt
reference when recording later real-campaign results. Structural admission
alone is not real table preservation or complete loop CFF removal.
