# Checked cyclic corridors: independent design and native plan

The proposed opt-in `guarded-checked-corridors` has a sound generic basis: it
copies a bounded amount of code into a real loop and checks each iteration's
actual selector. It does not prove a table immutable, a state range complete,
a loop infinite or a hint reachable. Code-size budgets bound compilation, not
the number of permitted runtime iterations. Existing J/M/core modes remain
byte-compatible and this controller freezes its existing seed edges once.

The central invariant is a simulation of the original selected edge. The first
clone-head entry receives the selected seed's actual PHI values, including
previous executions' values. Later fast backedges receive the copied terminal
execution's fresh PHI inputs. The clone root PHIs cannot be eliminated into
seed constants, and `SSAUpdater` cannot substitute a fresh value into the
one-time historical snapshot. Every return case to the root contributes one
incoming per actual successor slot, including duplicate switch destinations.

All intermediate source conditions, flags, LOADs and STOREs execute in their
original order on every traversal. A nonselected condition keeps its original
target. Internal nonroot PHIs select the actual copied predecessor execution;
unsupported internal reentries may be refused without changing the source.
The terminal's fast selection consumes the actual original selector LOAD after
all original terminal operations, including a STORE which follows that LOAD.
Successful cases whose original target is the head enter the clone head;
other admitted cases enter their original targets. The full same-read fallback
keeps every original case and default, including its original head destination.
It must not accidentally redirect the fallback back into the copied loop.

Original-region and copied-region liveouts, shared exits and exit PHIs require
actual-edge SSA repair. No invented value may fill an unavailable predecessor.
For a PHI use, availability is on the incoming edge, not in the destination
block. Removing one selected seed edge must remove only that slot's PHI input;
any duplicate unselected seed edge remains. Initial snapshots and fresh loop
values need separate mappings through repair. Trial/verification/rollback and
Module/Function/Argument/global/metadata identity follow the existing core.

Region CALL/allocation/EH/volatile/atomic/fence effects remain refusal barriers.
An original side exit to an external call is retained, including its actual
arguments and PHIs; after return the original CFG decides reentry. A prefix
call outside the copied region executes once and may change registers/table
memory before the initial state is chosen. Neither case supplies a memory
preservation or noalias fact. A new target guard is at the original switch's
execution point and consumes the same selector: it does not need a new claim
that arbitrary source state PHIs are globally defined. Any additional earlier
condition would require its own deferred-UB analysis/freeze, as in V.

Independent native source and sequential model are frozen before candidate
execution. Eight fixture modes each have192 distinct tuples, total1,536:

- canonical finite loop, with scalar root PHIs updated at every true backedge;
- STORE before the actual table LOAD, changing a later target to another case
  or an unknown/default value so the actual guard can fail;
- STORE after the actual table LOAD, catching an incorrect selector reload;
- a genuine historical head value chosen by the seed after an earlier original
  head execution, explicitly including the same-address LOAD/freeze whose bytes
  change on every head visit, then fresh values on copied iterations;
- duplicate seed and terminal successor slots, with complete PHI multiplicity;
- direct and nonlocal/shared liveouts with distinguishable current/history data;
- an original side exit through an unknown external callee which writes real
  aliased Guest/register storage and then reenters original code;
- a CALL inside the proposed corridor, which must remain unchanged/refused.

The160 base tuples cover counts0/1/2/3/7, histories0/1/sign-bit/MAX, initial
states42/43 and four actual targets (head/business/other/default). Another32
cover two genuine register/Guest layouts, four descriptor bounds and all target
classes. Trigger iteration and replacement target vary independently in the
frozen tuple list. Initial state selection and unknown prefix-call effects
execute once, while the copied loop changes state/history/accumulator each
iteration. Both table states have independent actual mutable bytes; hint values
are predictions only.

The comparator retains return value, call count, the entire arena including
all registers, the full64KiB Guest, descriptor/config and all seven Exit fields.
Native clang O0/O2 runs use the same frozen inputs and finite per-group180s cap.
Fault programs have their own sequential models and must both match those
models and differ at preregistered inputs from the ordinary model. Planned
controls include duplicated/omitted prefix effects, stale root state/history or
reusing the initial memory observation instead of each head's fresh LOAD,
selector reload, dropped full-fallback case, wrong root/exit PHI value, and a
sidecall bypass. Invalid duplicate-PHI IR is verified and never executed.

The public driver invokes the new builtin by name with the original four
options64/32/128/16384, checks `loop_contexts`/`loop_backedges`, complete
fallbacks, frozen-seed scheduling and object identity. Old/new use the same
compiled public driver; old unknown-builtin refusal is feature absence, not a
claim of old wrong execution. Wrong/malformed hints, low/zero budgets and
unsupported region effects remain separate recipes. Old oracle profiles and
native totals are never merged into the new totals.

No new runtime has yet been used for this domain. Source generation/model
validation, actual compilation, candidate runs, fault/comparator audits and
dual-host evidence sealing are distinct subsequent stages. None of this plan
claims completed real-sample loop de-flattening or instruction/Unicorn coverage.
