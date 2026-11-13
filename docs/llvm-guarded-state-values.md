# Guarded state-value paths

`guarded-state-values` is an independent opt-in LLVM pass. It does not prove a
counter range, a predicate, a calling convention, table immutability or alias
separation. A single state and target are untrusted predictions. Every committed
rewrite reports Incomplete, including a rewrite that stayed within all budgets.
The default pipeline and existing finite/STORE/partial controllers are unchanged.

Attach one `MDString` JSON operand to named metadata
`dragon.guarded-state-values`:

```json
{"hints":[{"function":"view","block":"check","state":"0x2","target":"0x1234"}]}
```

Both numbers are unsigned hexadecimal bit patterns. They must fit the actual
state and target integer widths. At most 64 rows and 65536 JSON bytes are
accepted. Function/block strings are nonempty, NUL-free and at most 512 bytes;
one row per function/block is permitted. Missing or extra fields, duplicate
rows and malformed numbers reject the manifest. This namespace never adds or
modifies `dragon.guarded-entry` memory facts. Names select a current block; the
algorithm subsequently checks its real instructions and edges.

The selected original branch must directly consume an integer `icmp` with one
constant operand and one nonconstant scalar state of at most 64 bits. Exchanged
operands are supported. The comparison may be in a dominating predecessor.
Explicit undef/poison and unbounded or cyclic state dependencies are rejected.
An already executed LOAD or freeze is an SSA boundary, not an invitation to
reread its memory or reexecute its freeze. Existing arithmetic flags remain on
the original instructions and are not used as proofs of definedness.

After all original seed effects, the pass compares that same state SSA with the
predicted literal. On a mismatch it executes the original comparison's branch,
with both original successors and their exact PHI-slot multiplicity. On a match
it takes the original branch direction obtained by evaluating its predicate on
the literal. A bounded private copy of that direction's tail executes all its
original stores, reads, bounds, flags and side exits. The terminal target LOAD
is compared with the single predicted target. A target mismatch uses the same
LOAD SSA in the complete original switch and default; no second read occurs.
The predicted target must be an actual case of that switch.

The original comparison is retained and computed once. Seed or preceding CALLs
and STOREs execute once, before the new guard, even when they modify Guest or
register memory. Calls, synchronization, volatile/atomic accesses, allocation,
unsupported terminators and cycles are not traversed in the copied tail.
Search chooses one bounded path; it does not assume its intermediate conditions
succeed. All originals, including looping and unknown paths, remain present.
An incorrect state, target, source table snapshot or alias guess therefore
retains an actual runtime fallback rather than a fabricated analysis fact.

The entire operation is transactional: an outer temporary Function contains
both the state guard and the existing guarded-transition core. If the inner
copy/SSA repair or verification fails, none of the outer guard is committed.
A successful inner transaction invalidates body pointers; subsequent work uses
fresh block ordinals. Final splice preserves the original Module, Function,
Arguments and original block-order prefix. Function/body metadata identities,
EH, blockaddress and debug identities are conservatively rejected. A consumed
seed is marked to avoid repeated growth; the marker is only a scheduling filter
and is never evidence of correctness.

Public budgets are `max_candidates=64`, `max_rewrites=32`, `max_blocks=128` and
`max_instructions=16384`, with hard limits 4096/4096/4096/262144. These are
cumulative. Each transaction additionally has 32 blocks and 2048 copied
instructions. The two state-guard support blocks and three new instructions
consume those same limits; core snapshots, target fallback and SSA repairs
consume the remaining budget. Discovery uses at most 512 visited blocks per
hint, paths without repeated blocks, and a shared one-million work limit.
Instruction/operand scans, dependency walks, path expansion and both complete
Function clones are accounted before allocation/commit. Work or copy exhaustion
is reported explicitly; it is not interpreted as a missing hint. Source modules
must verify before transformation; trial and final functions verify as well.

The reported counters use the existing controller vocabulary: candidates,
attempts, committed rewrites, unresolved candidates, rejections, exhausted
budgets, copied blocks/instructions and target fallbacks. Original-switch counts
here refer to committed hinted paths, not a census of every module switch.
No successful candidate proves that all original state values were covered.

The independent unit file `TestLLVMGuardedStateValues.cpp` covers swapped and
dominating comparisons, retained LOAD/freeze/flags, seed CALLs and stores,
wrong predictions, duplicate predecessor slots, exact transaction rollback,
function/global identity, metadata/EH-related exclusions and zero/copy budgets.
The old library does not provide this builtin, so its public-test failure is
API-absence evidence only. Native validation must separately use a frozen
sequential model, complete Guest/register/Exit state, and specified wrong-guard,
dropped-store, changed-table and reread controls. No execution results are
claimed by this implementation document.

An existing target-only fast-path closure report must remain available. A new
value-guard-aware report must identify actual equality SSA, original predicate,
selected successor slot and both runtime fallbacks from instructions, not tags.
Static conditional closure, full-state equivalence and observed repeated fast
loop hits are distinct acceptance conditions. This pass alone does not establish
complete loop deobfuscation.
