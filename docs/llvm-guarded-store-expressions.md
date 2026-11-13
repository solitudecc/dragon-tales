# Guarded STORE-expression predictions

`guarded-store-expressions` is an explicit LLVM pipeline step for bounded
integer state encodings such as `zext((state + C1) xor C2) * 8 + table`.
It uses finite STORE values to predict possible table addresses, then retains
an actual target check and the complete original fallback. This is a checked
control-flow view; a successful copy alone is not complete de-flattening.

## Input and budgets

The selected module carries one `dragon.guard-predictions` named-metadata
operand containing a JSON string:

```json
{"view_symbol":"selected_function","memory":[
  {"offset":"0x2000","bytes":"1010000000000000"}
]}
```

Addresses are Guest offsets and bytes are hexadecimal in address order.
These are untrusted target predictions. They do not establish initialized or
immutable memory, alias separation, function-entry premises, or call effects.
The independent metadata name lets ordinary decoded-region views supply
predictions without claiming a guarded-entry certificate.

The step accepts the existing integer options `max_candidates=64`,
`max_rewrites=32`, `max_blocks=128`, `max_instructions=16384`. Hard maxima are
4096/4096/4096/262144. Existing expression, search, copied-corridor and shared
work limits apply. It is not part of the default LLVM recipe.

## Prediction and execution

Ordinary finite SSA/path enumeration runs first. If it cannot evaluate the
address, the bounded fallback seeks one simple Guest integer LOAD atom and a
finite STORE of the same integer width and Guest base. STORE address equality
is not asserted: an unrelated STORE may select poor predictions. Typed APInt
operations re-evaluate the complete address expression with each candidate,
including narrow modular arithmetic, extension and scale. A complete finite
address set may yield only a subset of mapped target predictions; missing hint
bytes and non-case hints remain covered by the original boundaries and full
fallback. This does not enable partial arithmetic evaluation or mask guessing.

The substitution exists only inside the predictor. Original LOADs, arithmetic
flags, STOREs, side exits and actual target reads remain executable. A target
check chooses a fast case only when that actual read matches its candidate;
its default executes the full switch using the same read. A later table STORE
therefore cannot change the fallback observation through an invented reload.

Historical and fresh observations are separate predictor keys. Multiple LOAD
atoms, unsupported dependencies, non-finite STOREs, unsafe effects or exhausted
budgets are refused. No value or definedness fact is exported from a hint.
Cyclic copies use the existing checked continuation helper and actual PHI
incoming edges; the pass reports loop contexts separately from total copies.

## Validation status

Both freshly linked production backends pass the new 10 cases / 777 assertions.
Full historical CTest passes main1163/1163 and no-Z3 1028/1028; the new cases run
separately against each actual shared backend. The exact Python registry check
includes the additional builtin and checks its options/default exclusion.

The same new test object runs against a genuine `origin/main c9cd781` backend
and fails all ten cases at builtin acceptance. This is a missing feature,
not an old execution defect. Prototype regressions additionally demonstrate
nested-poison admission and partial-hint mapping failures before their fixes.

An independent native campaign compares whole return/arena state at host O0
and O2 with normal and wrong predictions: 10240 actual before/after matches,
4096 fault observations and 3472 required differences detected. The 64 input
keys project to 62 effective i32 initial arenas and 64 i64 arenas; repeated
host/prediction runs do not expand that domain. State widths8/16 have structural
regressions; the independent native state domains are32/64.

Four frozen real ELFs pass128 complete Unicorn/native comparisons and64
Unicorn/Python-specification checks, with384 comparator-only corruption checks.
There are no exclusions, normalization, new native-C comparisons or new Guest
producer/lift runs in that replay. Saved streams were independently audited.
The original A/B evidence remains separate.

The real pass still constructs0/4/16/25 conditional contexts in manual O0/O2
and flatten-switch O0/O2. These copies do not establish complete de-flattening:
C-O2 conditional fast views have no closed business SCC after excluding the
original central dispatcher/misses. Final evidence sealing is recorded
separately before merging. No new instruction or full69-input corpus campaign
is claimed for this opt-in LLVM component.
