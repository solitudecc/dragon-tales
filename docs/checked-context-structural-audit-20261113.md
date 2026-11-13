# Checked continuation context audit

This is an independent, read-only audit of the actual checked-corridor IR. It
adds no production pass and executes no target program. The old scalar-EQ and
V4 same-target closure algorithms remain unchanged. Their results, including
refusals when a fast target now names a copied root, remain in the new report.

`checked_context_structure_audit.py` locates each changed original successor
slot, derives its source/copy path, and proves the copied opcodes, flags, typed
operands, conditions, and every unselected side exit. Its terminal small switch
must use a fresh same-block simple LOAD. The complete original switch and
default must remain in a fallback using that exact observation (at most one
fallback-local PHI is admitted). Only a fast case whose original destination is
the source path root may enter the corresponding fresh root.

The audit checks actual terminator operands against every CFG successor slot,
and every PHI predecessor multiplicity, including duplicates. Root PHIs prove
the selected historical input independently of every fresh terminal backedge.
Intermediate selected PHIs prove their actual selected edge. Original exit PHIs
retain every remaining and added edge with the corresponding source value.

SSA repair is checked by source-definition occurrences. Each original or
proved copied definition sets the current value of that source variable. The
audit walks every reachable actual predecessor backwards; encountering another
such definition prevents an old value from being treated as the current one.
New repair PHIs must carry the correct value on every actual incoming edge.
This checks edge-specific history, rather than just sets of old/fresh leaves.
Unreachable blocks and all their CFG/PHI slots remain present; source operand
changes in an unreachable block are conservatively refused. Unsupported or
ambiguous shapes are audit refusals, not evidence of a production defect.

`checked_context_cycles_audit.py` first re-proves the original V stage with V4,
then runs the new copy proof. It projects only the already proved state/target
guard-success slots, their proved copies, and the new same-read small-switch
success cases. All ordinary/business edges stay in the graph. It classifies
all complete CFF switch families against the original D37 anchor and source
Exit PC, including newly copied full fallbacks. The new result is conditional
structural closure; it does not prove every guard succeeds, every hint is
reachable, or an actual native loop was observed.

Both command-line audits bind the JSON inventories to actual raw IR with
`checked_context_inventory.cpp`: independent LLVM parsing and verification,
then complete byte-exact inventory comparison. The audit also checks complete
module content outside the selected function, including globals, declarations,
attributes and metadata payloads. Importer/runtime hashes, commands and actual
frame hashes are recorded. A failed importer or comparison keeps its expected
inventory and receipt; successful temporary inventories are removed.

The CLI uses `--before`, `--after`, `--before-ll`, `--after-ll`,
`--inventory-tool`, `--inventory-runtime`, `--inventory-runtime-sha256` and
`--out`. The cycle audit additionally requires `--value-before` (the actual
pre-V JSON), `--source-anchor`, `--manifest`, and `--hints`. Both refuse to
overwrite an earlier report.

The frozen source is recorded in the independent VM namespace
`~/vmc-runs/checked-context-independent-audit-20261113/SOURCE-FREEZE-v1.json`
(SHA256 `843016bcc52dda0116e3e2bf1a051c1f9b568e3c858dedf7e9a0187109acf05e`).
Seven official main48b9 synthetic actual before/after pairs pass, including a
historical snapshot, duplicate seed/terminal slots, liveouts, an external side
call and STORE-before/after-LOAD. All fourteen raw-IR/inventory bindings pass.
Fourteen JSON corruption controls are detected. Three additional corrupted
IRs pass LLVM verification and are then refused: a historical initial value on
a fresh backedge, a historical selector in the full fallback, and an invented
`nuw` flag. These are structural controls, not additional native executions.
The four old profile58 final inventories used as identity transforms retain
T/T/T/F exactly; they are not counted as a new candidate or full replay.

Exploratory failures are retained separately: an assumed flagged native
fixture had no such flag (the control now invents one); the first identity
checker followed unreachable predecessors as if they were entry-reachable;
and a pure inventory startup found the retired M runtime unavailable. That
loader failure parsed no IR and counts as no control or target execution.
The same frozen importer, with explicit official48b9 runtime, reproduced all
fourteen original inventories byte-for-byte before final controls. No
exclusions, comparator relaxations, production replays or native counts were
introduced by this work.
