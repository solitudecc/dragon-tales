# Guarded SSA state corridors

`guarded-state-corridors` is an opt-in LLVM pipeline component. It discovers a bounded path from an original incoming edge to a dynamic Guest target LOAD/switch, uses finite SSA state values to select a small set of predictions, and invokes the transactional guarded target-set copier. It does not prove that Guest table contents are immutable or that a prediction is correct.

The copied path retains its real LOADs, STOREs, arithmetic flags, conditional bounds and side exits. Its terminal small switch tests the same newly executed target LOAD used by the complete original fallback switch. A mismatching table entry, state overwritten on the path, or incorrect prediction therefore follows the original decoded cases/default. This is not a replacement for unresolved target handling.

## Invocation and ordering

Add `guarded-state-corridors` explicitly to `PassPipeline`; it is absent from `defaultLLVM()`. Its numeric options are `max_candidates=64`, `max_rewrites=32`, `max_blocks=128`, and `max_instructions=16384`. Costs are cumulative across transactions in this invocation. Hard maxima are respectively 4096, 4096, 4096 and 262144. The existing options/statistics layouts and C ABI are unchanged.

For the current experimental composition, use this component before the natural-business-table component: `guarded-state-corridors`, then the separately specified LLVM value cleanup, `constant-branches`, then `guarded-target-sets`. An earlier target-set transformation marks generated blocks and may hide original switch seed opportunities from a later invocation. Generated seeds are deliberately not reopened.

This implementation depends on the independent `GuardedTransitionSetRequest` core. The controller does not alter the old single-candidate API or the existing transition controller.

## Predictions and execution views

The named metadata `dragon.guarded-entry` contains a JSON manifest. This component reads its `view_symbol` and memory byte rows solely as predictions. It neither changes that metadata nor adds entry assumptions. Memory rows are limited to 256 and 4096 bytes, reject overlap, invalid hex and range overflow, and must be little-endian 64-bit Guest addresses for this implementation. This does not relax the separate natural-business-table component's 16-target limit.

The terminal must be a simple AS0 i64 LOAD in the same block as its switch, through a byte GEP on the Guest data pointer loaded from function argument 1. A literal address remains the responsibility of the existing constant-context controller. Finite address sets are evaluated with APInt at their IR widths, through PHIs, selects, integer casts and Add/Sub/Mul/Shl. At most eight distinct values, 128 expression visits and depth 32 are admitted. Oversized shifts, unknown values, undef, poison, LOAD, freeze and cyclic dependencies do not produce a finite prediction. Select conditions are not evaluated: the two value arms are only candidate sources. Instruction flags remain in the executed copy; enumeration is not a no-poison proof.

A fresh PHI outside the chosen path is not globally unioned: discovery continues backward until an actual incoming edge can select it. A path PHI selects the preceding path block, or the seed for the first block. The first PHI's incoming value belongs to the prior execution, and is evaluated with a distinct historical key; historical finite PHIs may union their arms, but cycles and unknown arms stop enumeration. Memoization is per proposed path and view, stores only successful results, and never crosses transactions. A first-block PHI must retain an original incoming value after edge redirection.

These rules recover a constant state on a particular business edge without treating all executions of the shared dispatcher as that state. Finite select arms can predict two targets without analyzing the memory or external state used to compute the condition.

## Bounded discovery and transactions

Discovery freezes reachable original block ordinals, actual successor slots, ordered path edges and predicted target values. Intermediate blocks must be branches; a switch can supply a seed edge but is not traversed as an intermediate dispatcher. Calls, volatile/atomic accesses, fences, EH and address-taken blocks are conservative barriers. A selected path has at most 32 blocks and 2048 instruction cost, including core snapshots and SSA repairs. No block name or machine address is a semantic proof.

Discovery has an independent one-million work limit, 32768 reverse-edge visits and 512 visited blocks per terminal. Instruction operands, effect scans, path construction, expression evaluation, table-byte lookup and candidate membership checks are charged. Original block admission is cached. Exhausting total discovery work happens before any transaction, returns `Incomplete`, and leaves the module unchanged. Per-expression/search truncation may retain separately established candidates, but also reports `Incomplete`.

Each original `(seed block ordinal, successor slot)` gets at most one selected suffix in an invocation. Branch seeds have priority over switch seeds; candidates are bounded globally. Search retains one suffix per terminal/block rather than exploring every diamond path. This is a precision limit, not proof of closure. After each successful transaction all body pointers are reacquired by original ordinal and the current actual edges are revalidated. Original block order, Function/Argument/Module identities and metadata are preserved by the core. New blocks receive the existing generated marker. Original slots left by a budget can resume on another explicit invocation; copied blocks do not recursively seed new copies.

The core retains original paths, snapshots historical region values, repairs live-outs and PHIs including duplicate successor slots, verifies the trial, and commits only a valid transaction. A rejection retains the source and is reported explicitly. The controller shares the existing counters; `fast_switches_created` equals successful rewrites, each with one complete same-value fallback.

## Validation scope

Dedicated tests cover selected states 34 and 37 through two PHIs, finite PHI/select values 0 and 50 with casts, intentionally incorrect hints, original slot duplication and budget resumption, historical cyclic PHIs, unknown freeze/oversized shift, generated suppression, zero budgets, and global repeated-expression work exhaustion before transactions. Public string registration makes the tests compilable against the older API; an old library rejecting the new builtin is API absence, not an old wrong-result claim.

Implementation milestone validation (2026-11-12, baseline `313df60`): the final seven regression cases compile against both frozen old libraries and fail seven assertions out of fifteen because the public builtin is absent. This is API-absence evidence, not an old semantic wrong-result claim. Both new related-component groups pass 35 cases / 1110 assertions; complete CTest passes 1100/1100 with Z3 and 965/965 without Z3. Final production source SHA256 is `fabb9d3cb940e0c6049f5ff70ed56803ac690dd6bbcdf353d15f46a87fc55042`.

Independent native full-state testing, real four-ELF replay and final-loop execution observations remain ongoing at this implementation commit. Their final results will be committed separately before merging this branch. Complete guarded-loop recovery requires both path closure on real final output and observations that real inputs execute the fast loop, with hits and fallbacks classified. A finite predicted set, preserved semantics, or one copied prefix alone does not establish that result. No full-corpus or new instruction campaign is claimed for this opt-in controller.
