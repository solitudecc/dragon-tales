# Pointer PHI: independent real-program validation (2026-11-11)

The final v2 implementation preserves the previous per-round target-index
coverage and additionally exposes **f-combo O0 state 32, actual target LOAD
index 90376**. Four frozen ELF roots pass the unchanged full-state oracle.
This is a bounded precision improvement, not complete control-flow unflattening.
The failed v1 precision result remains in the archive.

## Frozen acceptance contract

Before either candidate execution, `PREDECLARED.json` fixed the inputs, old
coverage, candidates, pipeline and acceptance conditions. Its SHA-256 is:

```
aa4e7e3a3260b41742202fdae17ca9ecf070ec7c1821be3c8f453277172e4952
```

The unchanged `bounded_memory_pipeline_real.py` wrapper has SHA-256
`4782b9c01f7e2cdfb70505b3e048ac5f3a7cf74fd6f65bb8321bdb7f85456c86`.
It runs mem2reg, instcombine, GVN, guest-memory (`run_mem2reg=0`),
dispatch-specialize (`allow_plain_stores=1`), control-flow, DCE and a read-only
checkpoint, for three rounds with `max_pass_runs=32`. Neither rounds nor proof
budgets were increased for this experiment.

The inputs are the same f-cff/f-combo O0/O2 ELFs as the preceding historical
memory validation. Each has four inputs: 0, 1, 0x80000000 and 0xffffffff, using
the existing 1 MiB Guest mapping. The manifest pins all four ELF files, both
transformed C files, the old before-IR hashes and 18 oracle source dependencies.
The preceding complete evidence archive is
`guest-memory-history-real-20261111.tar.gz`, SHA-256
`31045d1616873f1d04f48f7b2a1cfb4c69c6404524adb3ee1eb882be2edd793e`.
This stage reviews that baseline's saved outputs; it does not count another
baseline execution.

Acceptance requires preservation of every old actual target-LOAD index after
each round, preservation of all eight original target switches and their
cases/defaults/selector LOADs, and at least one new actual index for the
predeclared combo states 14, 34 or 32. A copied switch or a constant appearing
elsewhere in IR does not satisfy the gain requirement.

`guest_pointer_phi_real_review.py`, SHA-256
`e821e840c5401cc38e23f2d09d7657ebd2bc826cf0486a693e3a8fe666bb3c43`,
recomputes these facts from saved IR. Before running candidates, its unchanged
gain gate rejected the archived old output. The review includes one positive
precision control and five deliberately failing precision controls, alongside
the existing four switch-corruption, seven false-gain and two switch-preservation
controls. Saved-output review is zero new analysis runs and zero native executions.

## V1 failure and v2 ordering

V1 passed C16/LLVM16 and all full-state corruption controls but failed the
predeclared precision gate. For f-cff O0 its three target-index sets were:

```
[70032]
[69672, 70032]
[69672, 69728, 70032]
```

Thus old indices 69760 and 69736 were lost despite a new combo result. The new
pointer proof was attempted inline while selecting the first successful STORE
candidate. It could consume the one-copy opportunity for an original LOAD
before a legacy GEP opportunity was considered. Full-state execution agreement
alone did not detect this loss of analysis precision.

V2 runs legacy direct-GEP opportunities across the module first, then legacy
hub-GEP opportunities, then pointer-only direct/hub attempts for still-pending
original LOADs. A successful legacy LOAD never enters the pointer phase.
The original candidate snapshot is visited once; newly cloned LOADs are not
rescanned within that invocation. The shared budgets remain unchanged.
Both the original audit and the frozen additional precision review pass v2.

## Final observed indices and validation

| ELF root | Round 1 | Round 2 | Round 3 |
|---|---|---|---|
| f-cff O0, 0x102a0 | 70032 | 69760, 70032 | 69736, 69760, 70032 |
| f-cff O2, 0x10080 | Same 13 indices below | Same 13 | Same 13 |
| f-combo O0, 0x10770 | none | 90120 | 90120, **90376** |
| f-combo O2, 0x10090 | 73904, 73952, 74048 | Same 3 | Same 3 |

The unchanged f-cff O2 set is
`69704, 69712, 69728, 69736, 69768, 69784, 69792, 69824, 69880, 69952, 69976, 70080, 70096`.
All previous per-round indices remain present. Guest LOAD forwarding counters
over the three rounds are respectively `1/1/1`, `0/0/0`, `4/5/1`, `0/0/0`.

V2 matches all **16 C-reference roots and 16 LLVM roots**, including complete
register/Guest/Exit comparisons. Deliberately corrupted results are detected in
all **16 register, 16 Guest-memory and 112 individual Exit-field controls**.
Exclusions, skips and normalization are zero. V1 has the same semantic counts;
its failed precision review is separately retained and is not a final pass.
The v2 wrapper execution took 111.3504 seconds; read-only reviews are not added
to that execution count. These are four existing ELFs, not new corpus coverage.

All eight original target switches retain their selector LOAD, cases and
default. State 32 reaches actual index 90376; state 14/index90232 and
state 34/index90392 do not. The saved round-2 combo IR orders the state-32
predecessor before those other two paths. Round 3 specializes it while both
other original STORE paths remain. The first-success/one-copy-per-original-LOAD
rule and the fixed three-round run explain this observed coverage boundary.
There is no direct evidence that either remaining state hit the 256-node cap;
no such diagnosis is claimed.

All **12 Guest-memory invocations remain incomplete**, with `complete=0` and
`budget_exhausted=1`. **10 of 12 dispatch invocations remain incomplete**;
only the last two f-cff O2 dispatch invocations report completion. Every dispatch
invocation reports `switches_rewritten=0`. CFF counts change from one each to
`4/17/3/4`; these are context copies, not removed flattening. Root execution
agreement does not prove analysis completeness or general loop convergence.

## Independent proof review and scope

The reviewed fallback proves a STORE pointer PHI against an existing direct-GEP
checked LOAD. It pairs pointer/frame PHI operands by incoming block, checks all
arms, and requires every obligation to reach a matching same-base GEP anchor.
A pending cyclic obligation is not accepted merely because it was visited.
Historical values remain separate from fresh corridor execution values.
Visible undef/poison, unsafe arithmetic flags, incompatible bases and unsupported
fresh dependencies remain conservative. Failed per-proof caches are discarded.

Review specifically considered crossed pointer/frame arms, a good anchor with
a bad sibling or ungrounded child cycle, old/fresh confusion, visible undefined
frame dependencies, and separately loaded Guest bases. No blocking correctness
issue was found. This does not claim LOAD-pointer-PHI support, arbitrary pointer
analysis or fully recovered indirect dispatch. The separate native agent's
matrix and its retained initial precision failures have their own evidence;
those observations are not added to this real-program count.

The launcher pins the 18 oracle sources before and after execution. Five final
production/test source snapshots were additionally checked against Mac and VM
after the run. `final-source-binding.json` verifies all 23 saved snapshots;
the additional five are explicitly not claimed as pre-run source pins.

## Evidence and restoration

The complete independent archive is available on both hosts:

```
Mac: /Users/c/Desktop/deob/validation-artifacts/guest-pointer-phi-real-20261111.tar.gz
VM:  ~/vmc-runs/guest-pointer-phi-real-20261111.tar.gz
SHA: 9c8d8b85955fe7af1d1c9c1cd2fd4487cecfcdce137d9c2d9b6f69bcf5b2c0bd
```

It contains 344 files including its manifest: all 343 payload files were
individually SHA-verified on both hosts, with exact archive member-set checks
and an additional exact comparison against the expanded VM directory. It
contains both candidate runs, all failed v1 reviews, final reviews, launch and
completion pins, source snapshots, the six fixture inputs and review notes.
The archive has no symlinks. Adjacent `*-mac-verification.json` and
`*-vm-verification.json` record matching archive hashes and counts. Restore
with `tar -xzf guest-pointer-phi-real-20261111.tar.gz -C <destination>`.

Runtime libraries belong to the separate root archive and are not duplicated
here. The launcher binds these exact backend hashes:

```
v1: 5c4d8da8d9d16d174c2fc6b726591e5c289c88bf06545925090b13a926312509
v2: fadd2d5c1b12c7db913d2279255799f6195926799582e3c46d9b1eceb2136e30
shim: e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec
```

At validation time v2 is frozen under
`~/vmc-runs/guest-pointer-phi-root-20261111/runtime-v2/`.
Expanded v1 libraries have been retired following the root's dual-host archive
verification. Restore their lossless `v1-main-libdragon*.gz` parts according to
that root's `RUNTIME-PARTS.json`; do not treat old launch paths as live libraries
or substitute a later `build/lib` runtime.
