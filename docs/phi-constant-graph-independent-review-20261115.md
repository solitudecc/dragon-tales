# Independent read-only PhiConstantGraph review

Reviewed snapshot: `LLVMPhiConstant.hpp` SHA-256
`4f7d4e079ad1ece3d4c944b563141baf4aba3a6c0e77ed7772aadd752aac7172`,
`LLVMPhiConstant.cpp` SHA-256
`21d0f63c0108238c03d13e21477c049d4bf3564d0830bd357788fe847407244e`,
and tests SHA-256
`cdf1f2fe31995a81007fd79a7400ae188fe129222da59f4fea8de9449a2d5cd7`.
This review performed no compile, SDK invocation, native execution or source
edit in the implementation worktree.

No soundness blocker was found under the documented valid-Function and
immutable-SSA/CFG preconditions. The graph contains only same-typed integer
PHIs and one literal integer value. All actual incoming slots are checked;
every collected node must have a dependency path to a literal or previously
completed constant proof. This checks an unanchored subcomponent even when the
queried root itself has a literal arm. Equivalently, every bottom dependency
component must have a real constant anchor. In valid SSA, a reachable dynamic
execution starts from a finite incoming history; because no other leaf value
is admitted and every literal is the same C, initial and later loop-carried
values are C. An unanchored disconnected cycle is refused, rather than assigned
an arbitrary constant.

This proof does not identify a fresh LOAD with a historical value. LOAD,
argument, freeze, arithmetic, cast, undef and poison leaves are refused. A
numeric recurrence such as PHI/add/PHI is outside this graph even if its first
arm is constant. Distinct literal values anywhere in the closure reject the
entire root. The result is an exact constant independent of a query point;
it is not an alias fact, a contextual range or a new noundef contract.

New positive cache entries are published only after the complete graph,
anchor reachability and full publication cost have passed. Late disagreeing
arms therefore cannot publish a good earlier child. Work and node exhaustion
publish neither positive nor negative partial results. Decisive ordinary
refusals may be cached only for that root and unchanged function. A cached
dependency cannot turn a prior complete refusal into a proof without changing
the function: dependency edges and literal leaves are intrinsic to that same
immutable graph. Every cache hit still costs one work unit, and a zero-allowance
query refuses before lookup. Each query is capped by the current parent
allowance and 65,536 work; 512-node overflow is distinct from scalar-depth 32.

Integration creates a graph inside each candidate-local LoopRanges object.
Its get call receives the current Image work remainder and charges returned
work. Proof objects are reconstructed after mutation; the post-consumption
range pass uses a fresh object. Constant-overlay map entries do not alter the
SSA graph. Thus reuse across different STORE query points is permitted for
these exact constants, while branch-dependent PathValueRanges facts retain
their separate program-point checks.

One defensive boundary remains: actual-slot multiplicities are checked, but
two duplicate predecessor entries containing different PHI Value objects that
individually prove the same C can be accepted by this graph. Such an input
violates LLVM's duplicate-incoming value identity rule and is outside the
valid-Function precondition. The Image caller verifies its module before this
analysis. This is not a valid-IR counterexample. If explicit malformed-input
rejection is intended beyond the current precondition, add a per-predecessor
original Value-identity check and a deliberately malformed structural test;
do not execute that malformed IR.

The existing focused source covers long anchored cycles, literal conflicts,
unsupported leaves, unanchored subcomponents, duplicate/missing slots,
late-failure cache isolation, zero/tiny work, the exact 512-node boundary and
513-node refusal. This review does not establish that those tests have run or
that the complete table/cell invariant or real loop CFF removal has succeeded.
