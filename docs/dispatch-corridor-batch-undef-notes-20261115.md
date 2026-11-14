# Batch corridor: undef/poison review boundary

This review first considered the intended control contract in CPP
`ceaa7c61a2f6e6692782b2f0a0d883d8e406737d3d2104907e593a15e700b8a3`.
Root's subsequent actual focus exposed two implementation failures, described
below. The revised implementation is CPP
`81f40177a93da64362e6600b78eda701295ca7ab035db42d6c84fd81fed30e29`
and public header
`2f7cc6080b148ede586859b6a5fad60eef76272e72d1362d771ee881f3a2c467`.
The reviewer did not compile or execute Dragon, guest code, or a native oracle.
The original frozen implementation and its actual failures remain preserved;
the v4 fixes are a separate source version. This note is not a v4 passing-test
claim.

No counterexample was constructed in which an originally **defined** early
error return or bounds loop changes under the narrowly admitted transformation
below. This is a bounded source-review conclusion, not an exhaustive refinement
proof, a semantic test result, or a statement that arbitrary SSA values are
`noundef`.

## What the two values mean

The private evaluator correlates one `(SSA value, Current/Historical view)` atom
while calculating two candidate control outcomes. Different execution views and
different atoms cannot share that assignment. The two results are not exported
as a range certificate for an undef-valued instruction. In particular, they do
not authorize replacing data uses, memory indices, register contents, or LOAD
results with those constants. They do not authorize `noundef`, `assume`, noalias,
immutable Guest bytes, or a claim that a Guest LOAD must be defined.

The distinction matters for two uses such as:

```llvm
%wide = zext i1 %c to i32
%other = select i1 %c, i32 8, i32 0
%state = or disjoint i32 %other, %wide
```

For a defined atom, `%state` is 0 or 9. A general-purpose range analysis must
not silently assume that separate uses of `undef` are a stable concrete bit.
This implementation's permission is confined to the control replacements
described here, while retaining the original data computations and consumers.

## The consumption points stay put

The LLVM LangRef specifies undefined behavior for conditional `br` with an
undef/poison condition and for `switch` with an undef/poison selector. This also
applies to a conditional branch whose two successor slots name the same block.
See [conditional br](https://llvm.org/docs/LangRef.html#br-instruction),
[switch](https://llvm.org/docs/LangRef.html#switch-instruction), and the
[undef discussion](https://llvm.org/docs/LangRef.html#undefined-values).

The admitted rewrites retain these boundaries:

1. A two-state final switch becomes an equality and conditional branch using
   the **actual selector**, at that original switch point. Earlier memory
   checks, stores, error returns, and their actual SSA inputs remain before it.
   There is no early equality, new freeze, or constant-state data substitution.
2. A two-state bounds self-edge keeps the original comparison and conditional
   branch, including its actual condition and position. Only its successor
   slots change: both continue. Each of the two evaluated outcomes must have
   selected the original continuation. The head has no memory operation, call,
   or freeze. A historical use cannot borrow the fresh selector's proof.
3. Only a singleton with `mayPoison == false` may remove the corresponding
   condition consumption. An atom-dependent singleton, including one obtained
   by masking away the bit numerically, remains refused.
4. Flagged instructions are copied with their original flags. Every evaluated
   assignment must satisfy the supported flag checks; a violating assignment
   refuses the whole batch. Unrelated pure head calculations are retained.

## Why the attempted early-error counterexample does not apply here

The unsafe alternative would compare `%state == a` before an original memory
fault, select a constant-specialized corridor, and thereby consume undef/poison
before the original switch. The implementation does not do this.

If the original execution exits on an earlier error, the new terminal comparison
is not reached either. Its actual data values, operations, error condition and
error successor are not replaced using the two evaluator results.

At a preserved bounds branch, an undefined condition already makes the original
control transfer undefined. The new branch consumes that very condition at the
same point. If the condition instead has a defined result that selects the
original self-edge, the correlated assignments cannot all select continuation:
they are a subset of the legal assignments to the same pure expression. Such a
request must fail preflight. This reasoning does not turn the atom into a
defined value or justify changing any other data use.

At the final switch, a selector still containing undef/poison is already an
undefined control operand in the original. For a genuinely defined stable atom,
the correlated domain covers its false/true choices. If operations completely
mask an undefined input into a defined constant, that constant must also occur
on the correlated subset; the evaluator cannot justify an alternative constant
that differs for every legal assignment. Atom-dependent singleton folding is
nevertheless conservatively refused by the current implementation.

## Required structural checks and remaining validation

The no-`noundef` same-atom/multiple-use regression must check the terminal
comparison's location, retained actual bounds condition, two actual successor
slots, unchanged memory/error ordering, and absence of newly added `noundef`,
`assume`, or freeze. Such a structural test is not a native execution oracle for
LLVM undef. Initialized full-state native comparisons and deliberate wrong
controls remain separate validation requirements.

The review also relies on the ordinary batch contracts: frozen original
candidates, per-Use history, complete actual PHI slots, versioned available
values before global SSA repair, and atomic verification/commit rollback. It
does not certify those mechanisms merely from verifier success. No whole-loop
deobfuscation, native replay, or new test count is established by this note.

## Actual v3 failure and the separate v4 correction

Root reported the first actual SDK focus as 11 cases / 5281 assertions,
9 cases passing and 2 failing with 42 assertion failures. These were genuine
failures of the frozen contract; no assertion or old evidence was removed.

First, a copied bounds branch was unconditional in the published output.
The source had retained its condition during batch copying, but the later
`removeUnreachableBlocks` call folded it away. In LLVM 21.1.8,
`removeUnreachableBlocks` calls `markAliveBlocks`, which calls
`ConstantFoldTerminator`; the same-successor branch case replaces the branch
and recursively deletes its unused condition. This is an identified cleanup
step, not a conjecture about `CloneFunctionInto`.
See [the LLVM 21.1.8 implementation](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.8/llvm/lib/Transforms/Utils/Local.cpp#L2961).

V4 uses batch-only graph reachability deletion. It does not fold any reachable
terminator or simplify its condition. Before freeing unreachable SCCs it removes
their actual PHI slots and rejects any dead definition that still has a surviving
use; it never fills such a missed repair with undef/poison. All scans consume the
same shared work budget. Legacy cleanup remains unchanged.

Second, `trunc nuw` to i1 was accepted without its flag check because the
generic i1 observation rule treated the cast itself as an opaque atom. V4 makes
a batch instruction carrying its own poison-generating flags go through
expansion and concrete-domain validation instead. This fixes the admitted
flagged-cast case without claiming that an opaque observation is non-poison.

The final structural tests are separately frozen as
`9322fedf8aff039e6338a2b6002e7f1776f65de8dd999455275aaf6c990e1eed`.
They retain both original failing assertions and add the no-`noundef` two-use
shape above, including the actual bounds/terminal consumption checks. They also
distinguish an unselected padding switch edge back into the copied region from
a selected such edge. V4 checks the actual destination of every proved control
outcome; it still refuses any selected reentry atomically. Intermediate test
versions remain preserved: their added padding edges needed matching PHI
predecessors, the repeated-atom check needed the correct argument index, an SSA
name conflicted with a block name, and the padding positive incorrectly assumed
a finite-domain enumeration order. These are test-preparation corrections,
separate from the two actual product failures above; their original assertions
were retained. Root subsequently reported actual main v7 focused results of
13 cases / 5949 assertions and legacy results of 13 / 562 passing. This reviewer
did not execute those focused runs or derive a native/undef proof from them.
