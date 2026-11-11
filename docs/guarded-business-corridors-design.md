# Bounded business corridors

`guarded-transitions` can opt into `business_corridors=1`. The default and the
existing two-argument C++ helper keep the constant-terminal search. The new
three-argument internal overload preserves the existing options and statistics
layouts. This component extends candidate discovery; `GuardedTransition.cpp`
continues to implement every semantic transformation.

A business candidate starts at an actual original successor slot after a simple
constant Guest STORE and ends at a switch using a freshly executed target LOAD.
The initial shape is an i64 target at `Guest + (table_base + (state_LOAD << 3))`.
The Guest base must be the same simple descriptor-data LOAD used by the STORE.
The controller walks original branch predecessors with a fixed 32-block /
2048-instruction path bound. Calls, synchronization, non-simple accesses,
allocation, EH, selected-path cycles and previously generated blocks stop search.
All selected edge slots, rather than only predecessor block names, are retained.

The STORE value selects a predicted table entry from manifest bytes. This is
only a heuristic: the STORE may address unrelated data; later writes may replace
the state or the table; the predicted bytes may be stale. None of these is a
memory fact, an alias proof or a premise for removing a check. The copied path
retains every LOAD, STORE, condition and side exit. Only equality between the
actual terminal target and the prediction permits the direct case edge. The
false edge uses the same LOAD result in the full original switch and default.

Existing constant-terminal opportunities run first. All candidate discovery is
frozen before the first transaction. A breadth-first queue interleaves terminal
searches and is capped at 32768 examined edges, 512 visited blocks per terminal,
and the shared scan cap. Each original `(seed ordinal, successor slot)` selects
at most one path. Repeated successor slots remain separate candidates. After a
successful transaction, original block ordinals are restored and every selected
edge is rechecked; no stale body pointers or facts survive a transaction.

Success changes only that original slot to the generated copy. The copy does not
become a candidate. Other original slots remain eligible after a budget stop, so
repeated calls may finish pending work without recloning consumed edges. There
is no permanent original-seed suppression marker. The default module budgets
remain 64 candidates, 32 rewrites, 128 copied blocks and 16384 copied/repair
instructions. Individual business transactions additionally obey 32/2048.
Budget exhaustion remains explicit; this feature does not increase default caps.

`original_switches_retained` includes selected dynamic business terminals when
the option is enabled. It is not an all-dispatcher count. `rewritten_edges` means
an actual guarded copy committed, not that a candidate was proven constant or
that the whole loop was de-flattened.

Acceptance must separately verify complete output state and faulty controls,
unknown/modified table fallback, fresh versus historical loop values, duplicate
edge PHIs, effect boundaries, exact budget behavior and default compatibility.
Full-loop progress requires checking normal business-to-business fast paths;
copy counts and the existence of guarded terminal switches alone are insufficient.
The state41 explicit pilot proves only transformation/verification feasibility;
it is not an independent native validation of this controller.

Discovery effect admission is cached per original block and charged to the
shared one-million-unit scan limit. Every instruction examined for a STORE
prediction is also charged. Exhaustion stops before any transaction and reports
the already discovered candidates as unresolved. The selected numeric prediction
is frozen with its edge; later transactions revalidate the current table shape,
existing case and complete path, without rescanning a large seed. No memory
relationship is certified by this prediction. Unsupported shapes stay outside
the discovery domain; `complete` is not a whole-function completeness result.

Only one suffix is discovered for each visited (terminal, block) pair. A diamond
can therefore retain its other ordinary side exit into original code. A repeated
call being stable is not evidence of a closed fast loop; the independent CFG
closure check must retain such exits and all ordinary conditional edges.
