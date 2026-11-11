# Guarded finite STORE predictions

`guarded-store-corridors` is a separate opt-in builtin. It reuses guarded state-corridor discovery and transactional copying, adding a STORE prediction fallback only when SSA target-address enumeration fails. The existing `guarded-state-corridors` builtin and two-argument internal function keep their behavior; an additional internal boolean overload enables the fallback. Public C structs, default pipelines and budgets are unchanged.

The fallback recognizes a simple AS0 i64 Guest state LOAD used in an address of the form `constant + (state << 3)` or `constant + state * 8`. An OR in place of the outer ADD is also recognized as a prediction recipe and evaluated with unsigned bitwise OR; it is not an equivalence proof between OR and ADD. The actual flags and address expression still execute. Both state and target access must have the existing canonical Guest descriptor-data LOAD/byte-GEP shapes.

From the actual original seed block, discovery scans instructions backward and chooses the nearest simple i64 STORE to that Guest data SSA base whose stored value has at most eight finite predictions. The STORE need not alias the later state LOAD: this is deliberately only a hint. Its already executed value is enumerated in a historical view, never using a selected fresh path PHI to interpret an old value. Unknown or cyclic seed PHIs do not supply a value. Supported finite expressions and mask predictions are those of the existing G enumerator. Scans, shape checks, expression work, predicted addresses, table bytes and case lookups share the existing bounded work accounting.

For each predicted state, the table address is computed modulo 64 bits using the actual ADD/OR recipe. Existing manifest bytes supply candidate target values only. All candidates must be existing cases of the original terminal switch. One bounded candidate is retained for an original successor slot. No Guest LOAD is substituted, no alias relation is assumed and no check is deleted.

The transaction still copies the selected path in order, including plain STOREs, state LOADs, target LOAD and unknown bounds. Its small switch checks the actual target LOAD. Failed predictions use that same SSA result in the full original switch/default. Calls, synchronization, volatile/atomic operations and unsupported paths remain barriers. Original blocks and remaining edges stay available. Generated copies cannot recursively seed discovery; explicit repeated invocations can resume remaining original slots under the same limits.

The motivating combo O0 path is a concrete eight-block corridor. `memory.valid539` writes a select of 8/38 to Guest[RBP.30-168], then:

```
bb_89568 -> bb_68277 -> memory.valid131 -> memory.valid133
 -> memory.valid133.bb_68301_crit_edge -> bb_68301
 -> memory.valid135 -> memory.valid137
```

It reads the old state slot, writes the scratch state slot at RBP-264, tests the state, reads that scratch slot, and performs the actual table LOAD. The select's condition need not be resolved to predict both targets. A later write, alias or descriptor mutation can invalidate the prediction, so the actual LOADs and fallback remain necessary. A similar path writes select 10/48 at `memory.valid1594`.

## Validation contract

The standalone public-builtin test file covers:

- Existing G leaves the dynamic-STORE fixture byte-identical, with unchanged zero discovery/copy counters.
- A real eight-block LOAD/STORE/LOAD path with two state predictions and same-value target fallback.
- An unrelated last finite STORE, writes overwriting state or table, a STORE after the target LOAD, and an OR address recipe.
- Unknown stored values, an intervening call, duplicate original successor slots, zero budgets, bounded resumption and idempotence.

These structural tests are not a native semantic proof. The independent native matrix should use its own sequential byte-array model and preserve the ordinary/original paths. A minimal frozen matrix should cross state selection {8,38}, seed versus unknown entry, loop bounds {0,1,2,3}, table match/other-existing-case/unknown target, state overwrite, table overwrite before/after LOAD, and boundary descriptor sizes. The seed's original state STORE must itself obey the original bounds before the selected seed edge. Compare complete Guest bytes, registers, Exit, result and external-call count. Include explicit wrong-state substitution, omitted scratch STORE, reread-after-STORE, discarded fallback and duplicate-edge PHI controls. Instrumented fast-loop observations remain a separate campaign from these comparisons.

Implementation milestone (2026-11-12): the source with SHA256
`44f3c10d8df6ad203147cde3339b74268c88a4a7139d17e6e72cf62b9d1782eb`
passed the focused 43 cases / 1539 assertions in both builds and full CTest
1108/1108 (Z3) and 973/973 (no Z3). The old G build actually failed the four
new builtin cases (26 assertions, five failures including exception packaging);
this is API absence, not an old wrong-result claim. Source/build/old-test evidence
is frozen in `~/vmc-runs/guarded-store-corridors-20261112/` and its independently
verified metadata archive has SHA256
`05202a26336b1a1d2499b0fff502f4314e11a6678a62e5901eb93d4bd89c9991`.
The fixed 46-step real-IR probe rewrote 13 of 15 combo O0 candidates (119 blocks,
1783 instructions), but all four final modules still close only the previously
closed cff O0 region. Independent native and formal real replays remain pending;
API probes and IR verification are not counted as semantic matches.
A further selected-path STORE refinement is pending: its three regression cases
already fail on this implementation in both builds (43 assertions, three failures).
No complete-loop gain is claimed at this milestone. The additional finite STORE predictions may improve candidate coverage; actual fast-path closure and runtime guard hits still require independent verification. A shared unknown-load dispatcher that remains reachable cannot be reported as eliminated.
