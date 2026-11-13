# Fresh58 state-value loop observation

This is a separate observer profile for `base56 + guarded-partial-masks1 +
guarded-state-values1`. It does not rename the failed/stopped historical64 or
57 profiles, change the production pipeline, or alter the old observer/checker.
The existing pure-intrinsic instrumenter accepts arbitrary branch labels; no
new instrumenter API is needed.

Before execution, freeze the four new Python sources, the actual final view
inventory, its actual V input inventory (M output), original source anchor,
manifest, state hints, V4 auditor source pins, native harness driver/includes,
four original input/output byte files and the exact actual `llc` raw input.
The plan generator requires the fresh native harness's recorded compile argv
and final58 source/actual raw paths and SHA256 in `native-bindings.json`:

```json
{
  "recipe_steps": 58,
  "final_ir": {"path": "/actual/value58.after.ll", "sha256": "..."},
  "raw_ir": {"path": "/actual/native/raw.ll", "sha256": "..."},
  "compile_command": {"argv": ["llc-21", "...", "/actual/native/raw.ll"], "cwd": "/actual/native"}
}
```

These are exact producer/command bindings, not guessed path templates. A saved
component-tail file is insufficient. If final raw instructions do not match
the staged inventory exactly, stop and obtain a separately verified inventory
of the actual stage; do not normalize or ignore differences.

`fast_loop_state_values_plan.py` uses the separately frozen V4 auditor to prove
single frozen-i1 state equality, unchanged original state/predicate/effects,
actual selected clone path and same-read complete fallback, and separately
retains the historical scalar-EQ-only result/failure. It emits both
`state_success` and `state_failure`. The state may be an expression or PHI;
there is no invented requirement that it be a LOAD or be globally defined.
Target equality and membership guards must reuse their actual selector LOAD.

The independent `fast_loop_state_values_review.py` binds every selected raw
instruction, CFG slot and complete PHI edge multiplicity to the frozen final
inventory. It rechecks the actual frozen condition and old fallback direction,
seed effects exactly once, direct/one-edge-PHI same-read target fallbacks and
the small-switch/full-switch mapping. It independently derives all central
dispatchers by the original manifest-covered selector/source Exit-PC family,
then recomputes reachability and iterative SCCs over actual raw success edges.
Only already proved state/target/membership failure slots are removed. All
ordinary branches, business cases and side exits stay in the graph.

Each normal input must preserve every original output byte, enter the view and
pass admission exactly once, and have entry/state/target/business failures and
central visits all zero. At least one of the original four inputs must count
the same validated small-switch guard at least twice in the same actual fast
SCC. All four inputs and their exact witnesses are reported; a repeated scalar
guard alone is insufficient. This observes a view-local conditional path, not
arbitrary-input guard success or recursive ordinary callees.

`fast_loop_state_values_real.py` uses the original driver/includes/input/output
bytes. It adds host counters only and compiles instrumented IR with `llc -O0`,
with no optimization after observation. Four double-count runs must preserve
all state bytes and have exactly twice every count, failing the ordinary gate.
The omit-central emission variant must be rejected as an incomplete emission
manifest without native execution. Saved counts, exact lengths/EOF, complete
states, emission input/plan/output hashes and unchanged intrinsic declarations
and effect attributes are independently rechecked. Every subprocess retains
the original180-second cap; failures and source pins are retained.

`fast_loop_state_values_selftest.py` checks one complete synthetic raw CFG with
a loop PHI state and unchanged seed CALL/STORE. It deliberately corrupts raw
IR, inventories, plans, SCCs, count directions and stream framing, including
omitting a central block from both the plan and its cached metric. These are
checker controls, not native real58 observations. Real execution stays pending
until the fresh58 strict structural gates and actual native artifacts exist.
