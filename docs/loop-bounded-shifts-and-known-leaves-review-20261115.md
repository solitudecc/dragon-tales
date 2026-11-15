# Bounded shifts and per-query leaf/shape accounting: independent review

This is a read-only mathematical and source review. It changed no production
file and ran no compiler or target. It is not a claim that the real sample's
remaining proof has closed. Shift fixture ownership stays with the separate
test author; the private accounting regression described below is a distinct
source-inclusion validation plan.

The reviewed bounded-shift Image source is
`b6b18cc70504e690c1eb3671858309237a06b48451ff9ace8561f6ada91dc01e`.
The known-leaf candidate is
`df7746ffbf9b5fc9b90ce6c5667a938658dea6c3532228c87e5116cf0ec5a8a7`.
The subsequent same-root Opaque consumer candidate is
`17badf6503b87724228fad78ee3b424f94683e24afff1532017f3789b76cc544`.
All three complete source bytes were independently snapshotted. The b6 copy
was reconstructed from the previously frozen mask candidate and the exact
shift delta, and its full SHA matched b6 exactly; it is not an approximate
old implementation.

## Bounded unsigned shift transfer

For ordinary unsigned W-bit values x in [aLo,aHi] and legal counts k in
[bLo,bHi], logical right shift is monotone increasing in x and decreasing in
k. Therefore `[aLo >> bHi, aHi >> bLo]` contains every result. Correlation
between x and k cannot invalidate this rectangular overapproximation. The
implementation first rejects an inverted count interval or bHi >= W; thus
bLo is also legal. W is already limited to 1..64 by the visitor. Count zero,
zero input, W=1 and the maximum legal 64-bit count 63 are all valid native
unsigned C++ shifts. No expression performs a C++ shift by 64.

Left shift is monotone in both operands only when unsigned modular wrap is
excluded. The complete-domain test `aHi <= mask(W) >> bHi` establishes that
every x*2^k fits W bits. Only then is
`[aLo << bLo, aHi << bHi]` valid. Otherwise the old Full result remains;
wrapping endpoints must never be used as an interval. AShr remains Full for
legal counts. Counts including the width or any larger value still refuse;
the transfer does not silently clip or mask them.

Actual operations and flags remain in place. An exact shift or nsw flag can
still produce poison where its original requirements fail. The interval is
only an ordinary-bit bound for defined results, not noundef, no-overflow,
exactness, stable observation or permission to remove an earlier consume.
Explicit undef/poison and other old hazard checks remain. Both count operands
are visited under the existing depth/node/work policies. No extra memory
content fact is created. The shared LLVM integer types enforce equal operand
widths in a valid module; this is not a cross-width reinterpretation.

The proposed independent test shapes correctly distinguish the endpoints:
84 >> {1,2} yields {42,21}, and 21 << {0,1} yields {21,42}. A test whose actual
42 hits a protected cell catches choosing only the smaller endpoint. A lower
endpoint target similarly guards the LShr lower bound. Raw shift/count shapes
must survive the official frontend control; an undef refined away by upstream
InstCombine is not counted as an observed undef refusal. Extending Opaque's
accepted variable-count shape, if needed later, is a separate policy change;
this Loop transfer does not alter the original raw get contract.

## paidKnownLeaves candidate

Df's new set is keyed by the original Value pointer, not the ConstantInt value
assigned by H_C. Distinct SSA observations that happen to map to the same
constant remain distinct charged leaves. Eligibility is restricted to integer
values up to 64 bits that are not explicit undef/poison and are literal or
known in the same immutable H_C. The existing visitor already treated known(v)
as a leaf. No Path result, Phi solver result, numeric LOAD cache or provisional
D range is inserted into this accounting set.

Every visit still spends Work before any credit and checks depth before a
seen leaf can return. Non-known visits retain their old node fee. Both outer
get/getForWrite clear the set once with their existing local counters. Nested
STORE queries, Coarse-to-Strong attempts and independent anchor phases do not
clear it. Numeric-cache rollback does not refund this set or already spent
nodes/Work. Different C candidates construct different LoopRanges objects.
The immutable Function/H_C lifetime is necessary, including during anchors;
this is not a cache that survives IR mutation or a change of hypothesis.

The current insertion precedes the increment that can report node 129. Such a
leaf has still increased the spent counter and sets the sticky exhaustion flag;
the failed visit and its ancestors must propagate refusal. A later repeated
leaf in that already failed query must not make the overall proof successful
or clear the flag. This boundary deserves a direct exhaustion regression.

No soundness blocker was found under those constraints. Nevertheless this is
an observable Loop budget-model change: repeated known-leaf occurrences now
pay one node per original SSA per outer query. The numeric cap remains 128,
but it is inaccurate to say that the old occurrence-based counting contract
is unchanged. Every actual visit still costs Work, and unknown/opaque costs
must not accidentally be discounted. The old closed/legacy APIs and old
Opaque get body remain outside this change. Any new bookkeeping allocation
is bounded by the admitted per-root node budget; lookup/cleanup costs and
compatibility should still be reported honestly.

## paidOpaqueShapes consumer candidate

The 17 candidate implements a second, explicit Loop accounting policy. Its
map is per outer query, keyed by the same PHINode SSA. A first use retains the
current remaining allowance and must receive admitted, nonzero complete N
that fits it; only then is N-1 paid and recorded after the already charged root.
A repeated use still pays the root visit's one node, Work and depth check.
It calls the unchanged getPhiRegions with the previously paid N (at most128),
requires admitted and exactly the same complete N, and pays returned work.
It then omits only the repeated N-1 expansion fee. No ordinary value, equality,
definedness or stability follows from the ledger.

Both outer entry points clear the map; nested/Coarse/Strong/Anchor calls do not.
Failed, partial or zero-footprint admissions never populate it. A failure or
work exhaustion is not ignored because a key was previously paid. Different
overlapping roots pay independently; their graphs are not unioned or merged
by an equal constant. The original physical-node/actual-slot validation,
getPhiRegions full-N cache contract and shared Work are unchanged. Shapes use
only the immutable H_C, not provisional D numeric ranges, so their paid
expansion survives numeric-cache rollback as spent accounting.

The code's remaining-allowance expression is reached only after an ordinary
non-known PHI root has passed the <=128 node check; it cannot underflow from
an already exceeded counter on an admitted path. Cached N is revalidated even
when the new remaining allowance is smaller, because that complete expansion
was already paid in this root. This is a deliberate new counting model, not
permission to enlarge the original Opaque API's maximum or skip its checks.
Map identity must not cross Function, IR or H_C changes. No additional blocker
was found in the reviewed 17 delta.

The owner's trace contains seven Opaque uses of four distinct roots, with
complete N values 7/7/7/7/7/4/7. The old cache hits correctly cost one Work but
still return full N. Repeated expansion credit could save node charges under
the new policy. Earlier failed prefixes (including 21 literal visits with13
distinct leaves) do not establish the full query's final footprint or prove
that either accounting change closes the real sample.

## Planned private regression surface

Use each exact original Image TU, with only test-only friend access and Image
public-symbol renaming in a recorded overlay. Do not mirror the visitor or
install a new test API. Link only the common driver and fixed777 SDK helpers;
the included implementation is defined in the executable and is not installed
Image execution. No frontend optimizer runs on generated fixtures.

Forty distinct `select(c,1,2)` nodes joined by a balanced flag-free add tree
have 199 ordinary scalar visits and depth below32. Under known-leaf charging,
40 selects +40 non-known condition visits +39 adds +two literal leaves require
121 nodes. Old occurrence charging must really fail. New must return40..80;
the same non-known argument remains charged forty times. A Work budget below
the full visit requirement still fails. Reusing the same literal at depth33
after a legal shallower visit must fail, and outer get/getForWrite must pay
the two leaves again on the next root. A nested STORE query must not reset
debt. Conditional H_C cases use the same SSA leaves in one immutable scope;
different same-valued SSA leaves and different candidate objects are separate.
Singleton numeric/D facts are not eligible known leaves.

A repeated complete Opaque root under independent masks can distinguish df
from17: the same root's expansion is paid once while every use still pays its
root node. A balanced expression with enough distinct roots must still exceed
128, even when their graphs overlap. Each new outer root repays its complete
first footprint; small Work and depth controls remain. Exact fixture counts,
source/helper pins, old failures and results must be measured rather than
adjusted after failure. Cache-transaction extension is paused and its older
2a3f40 assertions are not claimed as validation of these newer bodies.

## Closed-overlay consumption and original-node inventory review

This section is a read-only review of the later Image source
`430b2647f5e6f1c058f27b150b6a0e9cdb51a0bf4e01031bf720b41f5b3c82e2`.
It is separate from the b6/df/17 private accounting probes. No execution,
real-sample closure, semantic replay or native result is claimed here.

After the entire original post-batch P write set has passed the joint T+C
proof, consuming its already closed immutable H_C overlay is narrower than
rerunning a whole-function constant-fold fixed point. The overlay contains
only the exact selected seeded-cell LOADs and the original
`loopShadowConstants` pure derivations. It contains no Path, provisional D,
numeric LOAD-cache or Opaque result. The shadow transfer rejects
poison-generating annotations, admits only its existing integer <=64-bit
whitelist, and folds an instruction only when all operands are ConstantInt
and `ConstantFoldInstOperands` returns ConstantInt. Poison/undef results are
not admitted. Freeze, arbitrary calls and ordinary LOADs do not become pure
derived facts. A PHI needs all actual non-self arms to have the same constant;
an unanchored self-only cycle does not produce a fact.

The plan records typed original instructions by the already collected stable
Function ordinal. It cannot publish facts while the overlay is hypothetical.
After full joint closure, each entry is revalidated for same Function,
matching type, non-null ConstantInt and admitted instruction shape. Every
actual Use is paid before RAUW, including the second traversal made by RAUW.
The consumer retains every original instruction, including the real LOAD,
and does not simplify CFG, remove flags, erase effects or introduce assume or
noalias. Constant propagation into a side exit is valid only because that
exact SSA value has been proved; the exit and its original effects still
execute. Replaced users' operand identity necessarily changes, so this must
not be described as preserving every original operand identity. The ordinary
root and other Functions are outside this owned view's plan.

The old Path object is discarded before consumption. The subsequent check
constructs fresh ranges with empty H_C and fresh scalar/memory caches, then
rechecks every original preserving write, verifies the complete module and
uses the existing body transaction. Work exhaustion anywhere discards the
owned trial; a partially consumed plan is never committed. The original
`foldConstants` body and all other callers remain unchanged. Counts of
consumed seeded LOADs remain distinct from the number of derived scalar
substitutions or unused entries in the plan.

`recoverLoop` already scans all post-batch instructions and validates every
internal marker's schema and role. It may record the exact instruction count
and the complete marker list during that paid scan. From this point through
the final check, PhiLowBits only changes snapshotted Uses while retaining its
Trunc nodes; shadow/path/range helpers are read-only; the new consumer only
performs RAUW. No operation adds or removes an instruction or block. Therefore
the count and marker inventory remain exact. The consumer's integer plan
cannot contain the pointer-valued Guest-base LOAD or terminators. Table
cutpoint LOADs are disjoint from the seeded cell and are not H_C entries.
Final marker cleanup checks the original handle, Function and MDNode identity
before clearing exactly those validated internal tags.

Reusing this inventory removes a redundant full scan; it does not remove the
actual block/instruction caps, the original instruction-count verification
fee, whole-module verification or commit. Any future insert, erase, clone,
body replacement or CFG optimization in this interval invalidates the
inventory contract and must rebuild it or reject. The shared Work limit is
still1M with no reset/refund. No additional soundness blocker was found under
these fixed source-level contracts. Suggested focused controls are a fully
known pure chain/complete PHI, refused flagged or poison-producing operations,
side-exit uses, preservation of actual effect nodes, and consume-time budget
failure with exact source/module/object rollback.

## Physical-effect inventory review

The later candidate
`b4dd926b3c5027b3318d0f1a3c11c86dc8080711320b630b20c9bd6a348ae40e`
was reviewed read-only. Its effect inventory contains every CallBase, LOAD,
STORE, Fence and other mayWriteToMemory instruction, not merely recognized
Guest writes. The already paid recovery scan records entries in original
Function/block/instruction order with no deduplication. Each entry has a
non-tracking WeakVH, parent, opcode, result type and STORE value type or Call
function type. Recording these O(1) fields adds no hidden operand, metadata
or callee-body traversal to the scan.

Each later visit spends Work and validates the original identity/signature
before checking membership in the unchanged preserving region. The extracted
`checkLoopEffect` retains the old mathematical-call, simple-read,
simple-integer-Guest-STORE, unknown-write/fence, width, T+C span and complete
original-STORE `getForWrite` checks. Joint and empty-H_C fresh proofs both run
those obligations in the same order. The ordinary path also uses the same
complete list. Replacing the address-projection pass's initial pure
enumeration with this list preserves its P-region/STORE/GEP filter; all later
dependency discovery and PhiLowBits checks remain.

The only skipped instructions are those the old write checker classified as
having no relevant effect. This remains valid because all intervening steps
preserve instruction/CFG nodes and instruction kinds. RAUW may change an
address or stored scalar operand only through the already closed plan; fresh
range proofs still re-evaluate that original STORE. The inventory itself
publishes no alias, content, range, definedness or stable-observation fact.
Every entry and each actual range obligation still pays Work, no shared cap
is reset, and whole-module verification and transaction remain mandatory.

There was no new static soundness blocker. A defensive identity-check ordering
improvement is to reject a mismatching/null parent before asking an
Instruction for its Function; the present retained-node contract never
detaches one. Any later insertion/deletion/clone/effect rewrite would require
a new inventory or refusal. This source is outside the earlier b6/df/17
executions. The owner's private trace reached joint closure and consumption
but exhausted work in fresh checking; that identifies the repeated physical
enumeration cost and does not itself prove real closure or native semantics.

The owner subsequently applied the defensive parent-first checks in source
`a07d5816b744748b5d0173c53ede10524b5c7732ed034e1b68e50970e75eec99`:
effect identity checks compare parent before getFunction, and the consumer
and marker checks also reject a missing parent. This mechanical hardening
does not expand the reviewed proof or the b6/df/17 execution scope.
