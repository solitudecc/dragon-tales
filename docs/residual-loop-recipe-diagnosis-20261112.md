# Residual loop recipe diagnosis

These four bounded analysis probes run on the two preserved recipe-B final
`after-bound.ll` modules (f-cff O0 and f-combo O0). They are separate from the
sealed A/B real validation. No target-program native oracle was executed and
no new production change or default-budget change was made.

Predeclared source/input/recipe manifest SHA-256:
`d813b8862d3dd1e471229d61150f1807f6bee45867e2f643fb6ccf61b0944d12`.
The source archive is recipe-B
`107a7879475e07c2129b1b102bb972d45ccb8d1964e35eef351bcac41ca608cf`.
The analysis driver parses each module into a StaticAnalysis LLVMLifter's
existing Module, links only the frozen backend, verifies before/after IR and
saves both serializations and per-switch LOAD/GEP inventories. Both original
input files remain unchanged.

Runtime: continuation main backend
`34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f`, shim
`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`.
All runtime, source, input and included-header pins matched after the four
runs. Initial driver compilation succeeded; all four probes returned0 and
verified successfully, with no180s timeout.

## Explicit profiles

Each profile is one sweep only, with verify enabled and exactly5 or6 pass slots.

- P1: `mba`, `predicates`, `dispatch-deflatten`, `control-flow`, `dce`.
- P2: the same sequence, adding `dispatch-specialize` before deflatten with
  explicit `max_conditions=4096`, `max_expression_nodes=128`,
  `allow_plain_stores=1`.

MBA, predicates and deflatten use their existing defaults. The manifest records
all resolved defaults: MBA100ms/16queries/128nodes/3variables/resource1000000;
predicates25ms/64queries/256nodes/resource200000; deflatten64dispatchers/
512incoming-edges/128expression-nodes/16values. P2's4096-condition limit belongs
only to this separate diagnostic profile, not the prior256-condition real recipe.

## Actual results

| Case/profile | MBA simplifications | Predicate folds | Specialized branches / switches | Deflattened dispatchers | Whole-module switches | Seconds |
| --- | ---: | ---: | --- | ---: | --- | ---: |
| f-cff O0 P1 | 0 | 0 | not run | 0 | 10→10 | 0.434 |
| f-cff O0 P2 | 0 | 0 | 13 / 0 | 0 | 10→22 | 0.552 |
| f-combo O0 P1 | 16 | 0 | not run | 0 | 5→5 | 0.846 |
| f-combo O0 P2 | 16 | 0 | 5 / 0 | 0 | 5→10 | 0.959 |

The combo MBA improvement is real pass-reported expression simplification, but
these probes supply no independent new native proof. It does not eliminate a
loop dispatcher. Predicate0 is bounded coverage: cff skips624 conditions and
combo skips1037 under their64-query budgets. MBA skips261/3309 respectively.
Their API success status must not be interpreted as complete coverage.

CFF P2 still reports dispatch `Incomplete` with one private budget event, despite
665 visited conditions below the explicitly enlarged4096 cap. Its precise private
cap is not identified by the aggregate public counter. Combo P2 visits1022
conditions and reports no dispatch budget event. Deflatten leaves10/22 unresolved
CFF-module dispatchers and5/10 combo-module dispatchers for P1/P2 respectively.
A zero aggregate pipeline budget flag does not override an individual pass's
Incomplete result or its skipped-query counters.

## Original actual table selectors remain

The review checks the saved IR inventories and exact switch case/target mappings,
not only aggregate router counts. In both profiles:

- Guarded f-cff O0 `memory.valid13` still performs a real Guest LOAD from
  `69632 + state*8`, and that LOAD still directly selects the original CFF switch.
  The natural business switch in `memory.valid261` (base70040) also remains.
- Guarded f-combo O0 `memory.valid137` still performs a real Guest LOAD from
  `90120 + state*8`, directly selecting the original CFF switch. The natural
  business switch in `memory.valid1445` (base90528) remains separately counted.
- Guarded f-cff retains all8 switches, including its existing state contexts;
  guarded combo retains all3, including its copied entry-prefix switch.
  P2 adds no new constant actual LOAD index to either guarded function.

The P2 switch-count increase occurs in the ordinary fallback roots:

- Ordinary cff root:2→14 switches, new actual LOAD addresses69640,69648,69672,
  69728,69736,69760,69912,69944,69968,70016,70032.
- Ordinary combo root:2→7 switches, new actual LOAD addresses90120,90232,90296,
  90376,90392.

These are additional copied contexts and address proofs. Their table LOADs
remain; they are not resolved target values or deleted dispatchers. This
separately identifies limited specialization progress without claiming that
all extra switches were routers or that more switches mean deobfuscation.

The omitted existing passes therefore provide some MBA simplification and
additional ordinary-root contexts, but do not by themselves resolve the
remaining actual guarded loop dispatchers in these two inputs. Further work
needs to prove state/table contents at those accesses while respecting writes,
calls and reentry; this result does not authorize assuming immutable table bytes.

## Evidence

The complete diagnostic bundle is `residual-loop-recipes-20261112.tar.gz` in
`~/vmc-runs/` and `/Users/c/Desktop/deob/validation-artifacts/`. Its per-file
manifest covers the two exact inputs, frozen source/recipes, single-process
compile command/logs, compiler version, runtime linkage/pins, four exact
invocations, timings/timeouts, full before/after IR, verifier results, full pass
reports and inventories. `review.py` reproduces the original CFF/business
switch checks and ordinary-versus-guarded counts; `REVIEW.json` retains their
selector/pointer evidence. No old A/B archive or comparison was modified.
