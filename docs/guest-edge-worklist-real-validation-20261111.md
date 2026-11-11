# Original-edge worklist: independent real validation (2026-11-11)

With explicit `guest-memory edge_worklist=1`, the fixed three-round pipeline
recovers both predeclared f-combo O0 states **14/index90232 and 34/index90392**.
It preserves state32/index90376 and every previously observed target-LOAD index
after each round. Default behavior remains byte-identical to the previous
runtime on all twenty captured IR files. Both modes pass the original full-state
oracle. This is improved bounded coverage, not complete unflattening.

## Predeclared gate and the only configuration change

The baseline is pointer-PHI v2 backend
`fadd2d5c1b12c7db913d2279255799f6195926799582e3c46d9b1eceb2136e30`,
with complete evidence archive
`guest-pointer-phi-real-20261111.tar.gz`, SHA-256
`9c8d8b85955fe7af1d1c9c1cd2fd4487cecfcdce137d9c2d9b6f69bcf5b2c0bd`.
Baseline execution evidence is reused from that archive; it was not rerun here.

Before candidate execution, `PREDECLARED.json` froze the four ELF inputs,
six fixture hashes, original oracle dependencies, old per-round indices and
the requirement that at least one of states14/34 reach a new actual target
LOAD. Its SHA-256 is:

```
416a5dbc1240b60fde1885d1962439d1c8218cb00cab3e0b5685c5e56c115e5d
```

The supplemental activation/default-byte contract is SHA-256
`ac2ae32d5468c32921ebe65f137f1aea09b0ee55cdcbfa11f4d0d74cac6f82b7`.
It pins the activation driver, audit and twenty baseline IR hashes. The existing
`guest_pointer_phi_real_review.py` rejects the baseline under the new gate:
state32 alone cannot satisfy the states14/34 requirement. This saved-output
negative control performs no new analysis or native execution.

The original `bounded_memory_pipeline_real.py` wrapper is unchanged, SHA-256
`4782b9c01f7e2cdfb70505b3e048ac5f3a7cf74fd6f65bb8321bdb7f85456c86`.
The independent `guest_edge_worklist_real.py` driver intercepts only the public
`PassPipeline.add` request and adds numeric `edge_worklist=1` to `guest-memory`
in enabled mode. Default mode omits the option. It records all four module
activations, exact pass options/order and run options. The driver and audit
have eight activation-corruption and three default-byte-comparison controls.
The original oracle, inputs, comparisons, actual-index metric and switch
preservation audit are unchanged.

Both modes run mem2reg, instcombine, GVN, guest-memory (`run_mem2reg=0`),
dispatch-specialize (`allow_plain_stores=1`), control-flow, DCE and a read-only
checkpoint, for **three rounds, max_pass_runs32**. Proof, copy and repair budgets
are unchanged. Each mode uses f-cff/f-combo O0/O2 and inputs
`0, 1, 0x80000000, 0xffffffff`, with the existing 1 MiB Guest mapping.

## Results

| Check | Default mode | Enabled mode |
|---|---:|---:|
| C-reference full-state matches | 16 | 16 |
| LLVM full-state matches | 16 | 16 |
| Register corruption detections | 16 | 16 |
| Full Guest corruption detections | 16 | 16 |
| Individual Exit-field corruption detections | 112 | 112 |
| Original target-switch contracts retained | 8 | 8 |
| IR files byte-equal to the frozen baseline | 20/20 | Changed as intended |
| Wrapper execution time | 101.9834 s | 105.2987 s |

There are zero exclusions, skips or normalizations. The two executions repeat
the same sixteen root inputs; they are not thirty-two distinct sample inputs.
The independent saved-output audits add no native or analysis executions.

Enabled actual target-LOAD indices, recomputed from saved IR:

| Root | Round 1 | Round 2 | Round 3 |
|---|---|---|---|
| f-cff O0, 0x102a0 | Set A below, 11 indices | Set B below, 16 indices | Set B |
| f-cff O2, 0x10080 | Unchanged 13-index set C | Set C | Set C |
| f-combo O0, 0x10770 | none | 90120, **90232, 90296**, 90376, **90392** | Same five |
| f-combo O2, 0x10090 | 73904, 73952, 74048 | Same three | Same three |

- A: `69640, 69648, 69672, 69728, 69736, 69760, 69912, 69944, 69968, 70016, 70032`.
- B: `69640, 69648, 69672, 69704, 69728, 69736, 69760, 69816, 69864, 69912, 69928, 69944, 69968, 69976, 70016, 70032`.
- C: `69704, 69712, 69728, 69736, 69768, 69784, 69792, 69824, 69880, 69952, 69976, 70080, 70096`.

Every old per-round index remains present, including the f-cff O0 baseline
progression `70032` → `69760,70032` → `69736,69760,70032`. Both required combo
states recover by round2. The additional index90296 corresponds to state22;
this extra observation did not replace or weaken the predeclared gate.

Module Guest LOAD-forwarding counters across rounds are `17/3/0`, `0/0/0`,
`4/9/0`, `0/0/0`, respectively. These count rewrites, not distinct dispatcher
states. All twelve Guest-memory invocations still report incomplete. Ten of
twelve dispatch invocations remain incomplete; the last two f-cff O2 dispatch
invocations complete. Every dispatch invocation has `switches_rewritten=0`.
CFF counts change from one each to `21/17/6/4`: these are context copies.
All eight original business/CFF target switches retain cases, default source
PC and selector LOAD. Complete root execution agreement is not analysis
completeness or a proof of a general fixed point.

## Independent implementation review

Review covered the seven production files, including overload/default behavior.
The original memory-options layout is unchanged; old entry points select false.
All three legacy phases finish across the module before the optional queue.
Each function freezes original LOADs, blocks, terminators and successor-index/
target identities before its first checked-edge rewrite. Later work interleaves
functions; success moves an original LOAD to the queue tail, failure does not
blindly requeue it.

An accepted edge must still have the frozen terminator, parent block, successor
index and target. Redirecting it consumes that identity permanently, giving a
finite progress measure independent of the existing caps. New clone LOADs and
edges cannot become selected candidates during the invocation. Every attempt
rebuilds dominance, address/corridor information and proofs from current IR;
no stale facts or dominance result are reused. All phases share existing budgets.

The original-corridor check applies to its selected internal connections. An
external side exit may already lead to a previous clone; retaining that current
CFG side exit does not admit it as an original candidate. No claim is made that
all edges appearing inside a copied body are untouched historical edges.
No blocking correctness issue was found. This review and the real roots do not
replace the separate native worklist matrix or unit-test results.

## Retained failed validation method

The initial `candidate-v1-default-analysis` probe parsed saved textual IR with
`parseAssemblyInto` to avoid another native run. It **failed** the strict byte
comparison at the first root, f-cff O2. Even its before-IR already differed in
predecessor use-list ordering; subsequent SSA naming and traversal also differed.
This cannot establish a production default regression because the reconstructed
input is not the same in-memory lifting result.

The failed method, source, binary, logs, all five output IRs and diffs remain
archived. Its contract SHA-256 is
`f7a1e91f1f09f4c4eb74c4231f1f09776a852df49c9879897179af0213307679`.
It analyzed one module and executed no target function. The replacement uses
the unchanged full lifting workflow and passes the original twenty-file byte
gate without normalization. The failed C++ probe is archive-only; only the
Python activation driver and audit are added to the repository.

## Provenance and archive

Both full executions pin eighteen oracle sources plus seven production sources
before and after running. All fifty source snapshots and six fixture snapshots
are SHA-verified. `FINAL-SOURCE-BINDING.json` records the matches. Frozen runtime:

```
backend: ce87209b4dc79fa7280d6b53e2fa6edb8e77c9f9058e6c42b6be1894494d9186
shim:    73a1c316ea2902807ecda37e1acbd4da28785b695da51532ca0407af2927eb46
```

The root owns `~/vmc-runs/guest-edge-worklist-root-20261111/runtime-v1/` and its
lossless compressed runtime parts. This independent archive binds those hashes
instead of duplicating libraries; use the root's runtime-part manifest when
expanded library paths have been retired.

Complete independent archive:

```
Mac: /Users/c/Desktop/deob/validation-artifacts/guest-edge-worklist-real-20261111.tar.gz
VM:  ~/vmc-runs/guest-edge-worklist-real-20261111.tar.gz
SHA: 8612e8ece6af95c519912beb84f45e1d9f5938773788fa7098f2eb9dfc83fef5
```

The archive contains both full runs, the failed parsed-input run, all original
and supplemental audits, contracts, launch/completion records, source snapshots,
fixture inputs and review notes. The complete archive has 386 files, including its manifest. Both hosts verify
the exact member set and all 385 payload SHA hashes; the VM also verifies an
exact comparison with its expanded tree. The two duplicate full-run directories
were subsequently retired only after another exact subtree set/hash comparison
against the complete archive. The failed parsed-input directory and outer
reports remain expanded. `RETIRED-DUPLICATE-RUNS.json` and the two adjacent
`*-ARCHIVED.txt` notices record the removed paths and restoration commands.
No failed evidence is omitted to save space.
