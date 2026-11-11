# Guarded target sets for natural indirect switches

`guarded-target-sets` is an opt-in LLVM pipeline component for a small predicted
set of natural business-switch targets. It keeps that business switch. It is
not a replacement of a central CFF dispatcher with another full state switch,
and its rewrite count is not evidence of complete loop de-flattening.

Predictions use a separate named metadata node, `dragon.indirect-target-hints`,
containing exactly one node with one JSON string:

```json
{"function":"view","tables":[{"base":"0x80","bytes":"07000000000000000900000000000000"}]}
```

The bytes encode little-endian i64 entries. There may be at most 32 tables and
4096 bytes in total. Every table must contain at least one whole entry, have a
non-overflowing range and a unique base, and yield at most 16 distinct targets.
Targets are deduplicated in source order. Every predicted target must already
be a decoded case in the actual switch; unknown cases cause conservative
rejection. Additional provenance fields may describe the source but prove
nothing about current Guest memory.

This component never reads or updates `dragon.guarded-entry` or its JSON
`memory` facts. File permissions, ELF read-only sections and predicted table
bytes do not establish runtime immutability. The separate hints may be stale
or wrong. Only the value actually loaded at the original execution point
selects a fast edge.

The initial discovery domain is a simple AS0 i64 target LOAD immediately used
by its block's switch. Its byte address has the form `Guest + base + index*8`,
represented by an i64 add and shift-by-three or multiply-by-eight. `index` need
not be a LOAD; arguments, selects and other SSA expressions remain unchanged.
The Guest base must be the existing simple descriptor-data LOAD from argument
1. Address arithmetic and access effects are copied, not proved or simplified.

A successful transaction copies one original incoming context. A small switch
on the same fresh LOAD goes directly to the existing predicted case targets.
Its default reaches the full original switch, including all original cases and
the original default, using the same SSA value. There is no second memory read.
Writes before and after the target LOAD retain their order. Repeated case and
default destination slots each receive the required PHI incoming value.

The internal `cloneGuardedTransitionSet` helper shares the existing transaction,
selected-edge, historical-input and live-out repair machinery. The existing
single-candidate API, layout, cost and compare/branch output remain unchanged.
Transactions preserve Module, Function and Argument identities, retain original
blocks in order and append new blocks. Discovery freezes original successor
slots, restores their ordinals and rechecks each edge after every transaction.
New generated blocks do not become candidates on repeated runs.

The explicit module defaults are 64 candidates, 32 rewrites, 128 copied blocks
and 16384 copied/repair instructions. Zero budgets and incomplete work remain
explicit. The pass is not in the default pipeline and is unavailable without
its independent hints metadata.

Run final structural acceptance after the final chosen pipeline: general CFG
optimizers may combine nested switches again. Do not add opaque barriers to
force a preferred layout. Validate the actual small switch, same-read fallback,
all state effects and error controls. Natural targets can themselves reenter a
shared dispatcher; for example the f-cff O2 table includes address 65712. Such a
case still requires its incoming state/PHI context to be recovered before a
complete-loop claim is possible.
