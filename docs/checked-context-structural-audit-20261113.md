# Checked continuation context audit

This independent, read-only audit checks the actual checked-corridor IR. It adds
no production pass and executes no target program. The old scalar-EQ and V4
same-target closure algorithms remain unchanged; their results and refusals are
retained in the new report.

The default structure, cycles and selftest commands now delegate to their
`*_v2.py` implementations. The frozen v1 source remains in the separate
`checked-context-independent-audit-20261113` archive. V1 is unsafe for new
certification: an independently verified control showed that a local `%T` and
an identified struct type `%T` could be confused during printed-SSA pairing.
Changing the copied GEP source type to `%T.transition` changed its stride from
16 to 24, while the complete v1 CLI incorrectly accepted the copy. LLVM verify
and preservation of all module bytes outside the function both passed. This
was an audit false acceptance; the corrupted candidate was never executed.
V1 also rejected the official legal straight-context output when selected root
PHIs became literals (`%phase` to `1`, `%iteration` to `0`).

V2 first proves every actual value against the nearest original/copied source
definition on every reachable predecessor. This admits a proved SSA-to-literal
substitution without treating a historical observation as a fresh one. It then
compares exact printed operand type prefixes, maps only those proved operand
values, and compares the complete RHS bytes, preserving flags, literals,
metadata and all other spelling. The number of occurrences of every mapped
SSA token must equal its actual LLVM operand multiplicity. An extra occurrence
in an identified type or another unsupported position causes conservative
refusal. Consequently even an otherwise unchanged type/local-name collision
is refused; a separate noncolliding named-type copy is accepted. This deliberate
refusal is not evidence of a production defect.

The structure audit derives source/copy paths from changed actual edge slots,
not metadata or names. It proves copied effects and every unselected side exit.
The terminal small switch must use a fresh same-block simple LOAD, and its full
original fallback must use the same observation. Only a fast case whose old
target was the source root may enter the checked copied root. Actual CFG slots,
duplicate PHI predecessor multiplicities, selected historical inputs, fresh
backedges and ordinary exit values are checked independently. Unreachable
blocks remain present and require exact source operands. Ambiguous or
unsupported shapes are refused.

The cycle audit first re-proves the preceding actual V stage with unchanged V4,
then runs the complete copy proof. It projects only proved guard-success slots
and retains all ordinary/business edges. It classifies every full CFF switch
against the original D37 anchor and source Exit PC, including copied fallbacks.
Its result is conditional structural closure; it does not prove every guard
succeeds, a hint is feasible, or a native loop was observed.

Both CLIs independently parse and verify raw LLVM and compare the resulting
complete inventory byte-for-byte with the supplied JSON. The cycles CLI now
requires `--value-before-ll` and binds all three actual raw/JSON frames: pre-V,
pre-checked and post-checked. A hash alone does not certify the pre-V JSON.
Complete module content outside the selected function, including globals,
declarations, attributes and metadata payloads, must remain identical. Failed
imports retain the independent expected inventory and command receipt; reports
cannot overwrite prior evidence. The runtime is explicit `LD_LIBRARY_PATH` to
the official main48b9 snapshot, never the retired M runtime.

V2 focused validation in `checked-context-independent-audit-v2-20261113` records
32 commands: nine complete positive CLIs (the seven original official pairs,
the official straight pair, and the noncolliding named-type pair); the old
straight refusal and old corrupted-type false acceptance; and nine rejected
raw IR controls, each independently LLVM parsed and verified. The controls
cover the collision, changed GEP source type, wrong selected-PHI literal,
changed nonoperand literal, invented `nuw`, historical backedge/fallback and a
fresh LOAD replaced by a historical observation. Fourteen JSON corruption
controls and four frozen typed-operand regressions also match their expected
outcomes. No failed fixture was removed, normalized or excluded.

The four existing C59 products are audited without another candidate pass or
native replay. Each cycle result binds all three raw/JSON frames. The resulting
conditional closure remains T/T/T/F, with checked contexts 0/0/0/11; combo O2
still has a central reentry and is not complete CFF removal. Two additional CLI
controls reject a verified but mismatched third raw frame and an invalid third
LLVM input. The first proposed third-frame control selected two stages whose
inventories were byte-identical, so its expected refusal was invalid; that
accepted report remains. An initial mutation setup also found the same printed
line in both ordinary and guarded functions and stopped before mutation. The
final control changes that one literal only inside the selected function. No
audit rule or comparator was changed. A separate agent's read-only review
accepts eight frozen positives, records one conservative type/local collision
refusal and detects six faults; five added faults exercise direct JSON Proof
only and are not claimed as LLVM-verified raw controls.

All new audit work has zero candidate passes, zero native executions and zero
skips; historical native counts are not recounted. This
work adds no instruction/Unicorn/full-corpus campaign and changes no production
semantics or default optimization pipeline.

The structure CLI requires `--before`, `--after`, `--before-ll`, `--after-ll`,
`--inventory-tool`, `--inventory-runtime`, `--inventory-runtime-sha256` and
`--out`. The cycles CLI additionally requires `--value-before`,
`--value-before-ll`, `--source-anchor`, `--manifest` and `--hints`. The selftest
accepts the original pair directory and output path plus required
`--regressions`, naming the frozen additional input directory. Source, raw
synthetic frames, actual real frames, importer receipts, commands and failures
are retained in the v2 evidence namespace; the v1 archive is unchanged.
