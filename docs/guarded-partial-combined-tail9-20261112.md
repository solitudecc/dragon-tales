# I2 + K + J: frozen nine-step tail probe (2026-11-12)

The four final I2 modules received exactly nine new explicit pass runs: K (`invariant-self-loops`) once, J (`guarded-partial-corridors`) once, mem2reg/instcombine/GVN/instcombine/DCE, constant-branches once, then guarded-target-sets once. Together with the already executed original47 this is a 56-step composition, not a changed baseline or implicit fixed point. The original47 is not re-executed by this probe. No generated native program, formal replay or runtime observer is executed.

## Frozen binding

Pre-execution contract SHA `19045820f29664704832be2b09f2c82a8ea252d5db67c02076c01a59ba5c3637` binds all inputs and sources. Inputs are exact I2 final IRs from archive `7e211aa4b1dd42cc170b16f13ee116e939992baf53c91de1cd8c2a0e0c78f434`. Runtime backend SHA is `62ff74c752915064d3cfe87d024ead24169d5449386f9b91ea1f9fa01801f39e`; shim is `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`, under `guarded-partial-corridors-20261112/runtime-main-v1`. The source binding is `f16d5fe` plus the frozen `15e9572b` patch, with the K merge parent `0170911`; complete copies and hashes of the frozen controller, core, K and pipeline sources are retained.

K uses unchanged defaults 64 loops/1000000 work/65536 blocks. J and E each use candidates64/rewrites32/blocks128/instructions16384. Constant-branches uses folds4096/instructions1000000/blocks65536. Each stage has exactly one iteration and exactly its declared pass count. Every call starts with at least 512 MiB available and has a 180-second timeout. No budget was increased and no unknown outcome was reclassified as success.

All four calls exited zero in 10.404, 4.524, 52.945 and 24.815 seconds, respectively. Every stage verifies and preserves entry metadata. All source/input/runtime/producer/runner pins are unchanged. The independent family audit first compares the complete parsed-before inventory to the frozen I2 after inventory, then uses the original D37 inventory as its source-PC anchor. It retains ordinary conditions and all actual natural-business cases, and excludes only validated same-read guard failure edges. Sixteen deliberate selector/case/target/fallback corruptions are rejected.

## Results

| Case | K isolated | J rewritten / attempted | J complete / budget | Strict conditional closure |
|---|---:|---:|---|---|
| f-cff O0 | 0 | 0 / 0 | 1 / 0 | true |
| f-cff O2 | 0 | 0 / 0 | 1 / 0 | true |
| f-combo O0 | 0 | 0 / 0 | 0 / 1 | false |
| f-combo O2 | 2 | 4 / 4 | 0 / 0 | false |

Combo O2 J copies 20 blocks/384 instructions and retains four actual-LOAD full fallbacks. Its four partial candidate attempts succeed, but `complete=0` is preserved because unknown outcomes remain. The earlier call/partial-state suffix is no longer the nearest central-reentry witness. The new witness reaches `bb_67384` → `memory.valid157/161` → `bb_68705` → `memory.valid292/294` → the original `bb_66032`/`bb_66038`/`memory.valid49`. K preserves the original unsigned state comparison and its unknown false continuation; true self-loops remain explicit divergent spins. Combo O0 still has its dynamic initialization-counter path and a search-budget incomplete report. Conditional closure remains 2/4, not 4/4.

## Next O2 blocker: finite but lost correlation

In the guarded view's final IR, `bb_68705` defines `%isnotneg = icmp sgt i32 %694, -1` (line2521); `memory.valid292` defines `%isneg117 = icmp slt i32 %694, 0` (line11714). `memory.valid294` computes `%3631` (line11780):

```
((isnotneg ? 4 : 0) | (isneg117 ? 82 : 0))
  + (isneg117 ? -39 : 0) - 2
```

On defined executions these predicates are complements, so this expression has states `{2,41}`. Its actual incoming value reaches `%RCX.10` on the `memory.valid294` edge. The next table address is `73792 + 8*state`; manifest bytes suggest targets69064 and67942 at indices73808 and74120. Those bytes remain prediction hints, never immutable memory facts; the actual table LOAD, guards and complete switch are retained. The bounded suffix from `memory.valid294` through the original dispatch contains no CALL. This residual does not require a callee summary or heap evaluation.

The current non-scenario select enumerator loses correlation and forms `{-41,-37,-2,2,41,45,80,84}`. Address enumeration therefore succeeds as a finite Cartesian set, but some entries lack usable table hints. `predict` retries correlated Boolean enumeration only when the address set is absent; later failed hint/case mapping returns no candidate. Even an independently keyed retry would not recognize the two inverse comparisons automatically. This is source-level diagnosis bound to exact SSA, not a newly executed instrumented attempt trace.

A proposed smaller follow-up is explicitly J-only: retain usable hinted/case targets from an already finite address set while classifying missing entries as unknown partial outcomes. Here it may predict states `{2,41,45}`; extra45 is an untrusted prediction, not a proved reachable state, and all of its actual case edges must stay in closure auditing. The actual-LOAD guard and full fallback must remain, budgets must still report exhaustion, and F/H's complete-set policy must not change. This follow-up is not implemented or tested by this v1 evidence. No unknown edge is removed to make the current closure gate pass.

## Artifacts and formal preparation

`guarded_partial_combined_real_probe.cpp` exposes separately `--tail` (the executed nine passes) and `--full` (D37 followed by the unchanged original47 suffix and these nine passes). The latter mode compiled but has not executed. `guarded_partial_combined_real.py` and `guarded_partial_combined_full_audit.py` prepare a later full 56-step replay using the unchanged C/LLVM comparator and inputs; they are not counted as validation here. Formal replay awaits a separately confirmed final recipe and runtime.

The complete raw evidence is archived on both hosts under `guarded-partial-combined-real-20261112/probes-v1.tar.gz`, with exact-set and every-payload SHA/length receipts. It includes all stage IR/statistics, source and input pins, linkage/smoke/compilation logs, classification anchors, the final shortest witnesses, audit controls, and the unexecuted formal adapters clearly separated. No prior I1 precision failure, partial status, budget diagnostic or remaining central path is discarded.
