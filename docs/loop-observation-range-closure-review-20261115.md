# Exact-observation range closure: independent fragment review

This is a read-only source review of fragment v1. It does not compile or run
Image, alter the original program, or establish that the real Flatten sample
has closed. Earlier Gray/ad05 refusals and all private-diagnostic failures
remain evidence with their original outcomes.

Reviewed directory:
`/tmp/dragon-observation-range-closure/fragments/observation-range-closure-20261115-v1`.
Its MANIFEST SHA-256 is
`4218bfc6b2feff71f9377c88e0efe944ea3b98b9b8cde2b59d0f0db464b6d4e9`.
All five listed members were independently checked for exact size and hash.
The new resolver is `ddf5ccb9a9377a0c440e35e01e5ed194ebcb7225c59b644813eb660e33025858`;
support is `26c14f0a390d5653083823bf015a89066e486f33aec72d426730b06e0f1733fc`.
Against Image source `2bfd9dd823a913ad131c5c90406b7da454a5f2b5c66f76dd984f68289d972578`,
the strict companion was checked byte-for-byte after removing its introductory
comment and changing only its function name back to `loadRange`. The original
function hash is `0773f5336560125acc37ab44b80f3eb226652dd22ce02de8c7304a377e2e5f14`.

No soundness blocker was found within the following bounded contract. This
is approval to perform the required validation, not a substitute for it.

Each proposed interval belongs to one exact original simple integer LOAD L,
covering its dynamic observations. A second LOAD of the same address receives
no fact from address equality. It must complete its own reverse definition
graph, entry coverage and independent anchor hull. LOAD-prefix and block-end
points are distinct; actual predecessor slots, including duplicates, are
visited and charged. Only a nearest complete same-width fixed STORE ends a
segment. Partial overlap, unsigned span wrap, unclassified writes, unsupported
calls and observable memory operations refuse. Dynamic Guest writes remain
explicit deferred obligations. Every collected dependency component must reach
a complete STORE anchor, and an entry without such a definition refuses.

The anchor phase is independent of every provisional D interval. The numeric
LOAD cache is replaced temporarily with a copy of the cache from BEFORE the
outermost trial. All active exact-L assumptions are disabled. Nested LOADs in
this phase use only the original strict resolver; they cannot start another
provisional trial. Thus neither a parent's proposed R nor a successful
conditional child cache can manufacture the anchor's value. Existing H_C and
its separately scoped pure helpers remain conditional on the outer joint T/C
proof. H_D is not inserted into ConstantFacts, Path, PhiConstantGraph or Opaque.

Only after all original anchor values have independently produced a hull does
the exact L become usable during deferred alias checks. Each such check uses
the original STORE's SSA operands and program point. Different LOAD roots can
recursively establish different intervals; there is no same-cell union or
range-growth shortcut. This admits a joint first-bad-observation argument:
for the earliest ordinary-bit observation violating its interval, its nearest
anchor was independently within the hull, and every intervening write was
proved disjoint using observations computed before that write. Such earlier
observations cannot already be bad. Entry-without-definition is rejected.
This is an observation invariant, not a claim about D's contents at every
unobserved instant. The outer all-P T/C check still independently sees earlier
clobbers that this local read search can cut off at a later full overwrite.

The historical STORE scalar scope temporarily swaps only the pure-expression
active set. This avoids treating the outer current offset expression as a
cycle when evaluating its historical STORE use. The query still starts at
the enclosing LOAD's scalarDepth+1, with its original STORE point. Its fresh
local set still detects pure-expression recursion. It does not clear
memoryActive, the observation-trial stack, scalar nodes, definition points or
shared Work. In particular this is not a proof that historical and fresh
values agree.

The transaction lifetime was checked on every early return. Each trial keeps
its entry LOAD-cache snapshot. A successful child is only conditionally kept;
failure of any ancestor swaps back that ancestor's entire entry map, including
removal of successful descendants. Independent scratch facts are discarded,
not merged into the conditional cache. Phase/stack/pointer restoration uses
destructors and swap/pop/erase rather than another Work-dependent branch.
Snapshot-copy and retirement charges are prepaid; inserted cache entries also
reserve retirement. Failed proofs retain spent Work/nodes/points. The existing
32 scalar-depth, cumulative 128-node/128-definition-point and at-most-1M shared
work limits are not reset or increased.

These intervals are ordinary-bit supersets. They export no noundef, stable
observation, persistent memory value or noalias premise. Original LOADs,
STOREs, flags, fault order and control consumes remain executable. Provisional
facts must stay confined to this immutable Function/H_C proof context; they
cannot be reused after a CFG/operand change or another hypothesis context.

Required validation still includes the unchanged ad05 positives and all
negative/budget/rollback assertions. Particularly valuable cases are multiple
independent initializer arms whose full union is necessary, an earlier LOAD
before a later initializer, an unanchored dependency SCC beside a good arm,
partial writes, an unmasked value-update cycle, and a transient clobber fully
overwritten before every assumed observation. Tests must also force a child
success followed by parent alias failure and by parent budget exhaustion.
Public-module byte equality on refusal alone does not inspect an internal
numeric-cache rollback; a private test diagnostic should check the entry map,
phase, stack and memoryActive restoration directly. No such result is claimed
by this review. Snapshot and independent-proof cost may honestly exhaust the
remaining real-sample budget and must not trigger a budget increase or retry.
