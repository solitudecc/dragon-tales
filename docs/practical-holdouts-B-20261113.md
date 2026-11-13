# Same frozen function: explicit decoded-region recipe B

B changes only the lifting configuration of the eight A input ELFs. It reuses
the existing public `lift_static_region` and `static_region_replay` selectors,
the same sixteen inputs, all four LLVM plans, ten public pipeline steps and
the unchanged complete function oracle. No C is regenerated, no Graph finite
pass is run, no table bytes become trusted constant facts, and no production
library changes. A's 256 original incomplete rows remain recorded.

Every function must have a unique positive executable ELF `STT_FUNC` extent.
Within it, existing static decoded-leader selection supplies additional starts;
private IGNIL is rebuilt from actual decoded machine instructions. Limits stay
4096 blocks / 65536 dispatch cases. Both raw and product views use FunctionStart
and decoded-region coverage, keeping actual target LOADs, checks and unknown
Transfer exits. This is an explicit symbol-bounded analysis view, not automatic
function-range recovery for arbitrary stripped binaries.

The standalone `practical_holdout_region_diff.py` copies the exact A payloads
to a separately pinned B freeze. It verifies old source pins plus exactly its
two additional reviewed dependencies, preserves all A ELF/source/input/pipeline
records, and records the old manifest as a payload. Source files and old input
manifests are never modified. Normal B manifest SHA256 is
`0ec963329f95b987f93cc0de8f6700b8497068f76b88fdf7e7dbd49101d9f0db`.
This configuration was selected after observing A's failures; it is not a
second independent unseen sample or an untuned generalization result.

## Complete measured execution coverage

| family | independent C checks | complete LLVM-own-Unicorn matches | Incomplete |
| --- | ---: | ---: | ---: |
| plain | 32/32 | 128/128 | 0 |
| manual combination | 32/32 | 128/128 | 0 |
| Tigress arithmetic | 32/32 | 128/128 | 0 |
| Tigress switch flatten | 32/32 | 128/128 | 0 |

All 512 native model executions reach the actual root RET and compare all
76 modelled register fields, 65536 Guest bytes, call observations and seven
Exit fields. This totals 38912 register-field and 33554432 Guest-byte checks.
Entry checks cover all eight images, with zero failures or unlifted instructions.
Thirty-two complete before/after Graph JSON pairs are byte- and structurally
equal; all 124 accepted function lifts have the requested actual LLVM coverage
and entry attributes. The Graph is not silently expanded.

The same three separately declared controls pass: wrong C return produces
32 exact one-bit return differences with unchanged output bytes while its
128 LLVM comparisons match its own machine code. Register and memory controls
each give 128 complete returns with exactly one specified difference: RAX XOR
one, or Guest `0x8000` byte XOR one. The other fields match. All 24 control
module Graph pairs remain exact, and 84 control function lifts retain the
requested attributes. No retries, normalization, exclusions or unexecuted rows.

The independent normal audit rereads 640 complete C/LLVM rows, all 32 Graph
pairs, 124 actual function attributes and 80 original IR files; 2063 source,
input, runtime and result file pins stay unchanged. Normal and control audit
sources/results are separately frozen under `B-normal-independent` and
`B-independent`, with exact dual-host file-set/size/hash checks.

## Remaining dispatchers and limited simplification benefit

Manual and flattened images still have one non-entry dispatcher switch after
both static and product stock O2. Additional decoded cases make those actual
paths executable; they do not remove the state machine or prove its transitions.
Guest-memory and dispatcher passes may still report Incomplete/budget limits.
Zero unknown queries is not complete query coverage.

The independently recounted final stock O2 operation counts are:

| family | C O0: static / product | C O2: static / product |
| --- | ---: | ---: |
| plain | 11647 / 11680 | 7065 / 7128 |
| manual | 20299 / 20489 | 7103 / 7156 |
| arithmetic | 21485 / 21790 | 8497 / 8546 |
| flatten | 17827 / 14787 | 7418 / 7485 |

Only flattened C O0 decreases (3040 operations, approximately 17.1%); seven
pairs increase slightly. This is textual IR size, not recovered business CFG,
complete semantic cones, machine-code speed or measured analyst-time savings.
Both actual MBA helper reductions and substantial remaining Guest/dispatcher
structure should be inspected in the saved IR. Broad claims of a universally
better default pipeline are not supported by this batch.

All eight B static/product pairs have byte-identical original unoptimized IR
(full size/SHA equality). A/B instruction counts are not an optimization
comparison: the incomplete A views omit executable business blocks that B
now includes. A's shorter truncated IR is not a simplification benefit.

Decode, actual region lift, static selection, Graph auditing, product passes,
stock optimization, native compilation, Unicorn and native timings are separate.
The A lift envelope includes B's additional auditing/attribute-render overhead;
the actual region-lift time is also recorded. Concurrent cold/warm cache effects
were not controlled, so these are diagnostic costs rather than a speed ranking.

## Delivery boundary

Existing components can provide complete, checked IR execution coverage on
this new encoded/flattened function when the declared function range is
available. Full CFF removal remains unachieved. The useful next work is a
separately scoped dispatcher/state-analysis pass, with explicit recognition,
facts and validation. No image immutability, arbitrary heap model or guessed
external effects have been introduced to force a result.

This is a validation-only milestone; no CTest, old real60/61 or instruction/
whole-corpus run is repeated or counted here. Complete evidence sealing is
recorded separately. There is no main merge or general black-box success claim.
