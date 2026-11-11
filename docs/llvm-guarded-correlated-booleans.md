# Correlated Boolean candidate retry

`guarded-state-corridors` can retry a failed SSA table-address prediction using
at most three Boolean SSA identities and eight complete assignments. This adds
no builtin or option. The original finite enumerator runs first; a successful
result bypasses the retry. The opt-in STORE controller shares the enumerator,
while its STORE fallback retains its existing contract.

The motivating expression is `select(c, 0, 8) | zext(c)`. The new retry predicts
states `{1, 8}`, retaining the same assignment for both uses of `c`. Independently
combining the two uses into `{0, 1, 8, 9}` would lose the useful correlation.
These are candidate hints only: the actual address expression, target LOAD,
small target switch, and same-value complete fallback switch still execute.
Neither table immutability nor a predicate proof follows from enumeration.

Only a failed original query that encountered an unknown Boolean atom starts
the retry. Atoms are i1 arguments, simple LOADs, comparisons and freezes.
Comparisons are opaque Boolean predictions; this does not symbolically evaluate
their integer/FP inputs. PHIs, casts, select and supported arithmetic retain
structural evaluation. Direct undef/poison operands are rejected. A freeze is
not stripped or replaced. All arithmetic flags remain on the original IR.

An identity contains the SSA value and the historical/fresh execution view.
External instruction definitions normalize to historical; arguments have only
one identity. A root PHI uses its selected real predecessor and transitions to
historical evaluation of that operand. A later fresh execution of the same
LOAD or freeze has a separate identity. Fresh PHIs outside the selected path
still require extending the path to a real incoming edge; they are not treated
as arbitrary Boolean atoms.

Missing identities lazily fork the current assignment and replay evaluation.
Memo and active-cycle state are new for each replay. Each complete scenario
must produce a finite address set. An unknown non-Boolean value, unsupported
operation or cycle in any visited scenario rejects the entire retry. The retry
does not keep only the successful arm of an unknown select. It also does not
use the old unknown-AND-mask fallback to conceal such a failed scenario.

The existing global one-million work limit covers replays, path-index creation,
assignment copies, node visits and accumulated values. The existing depth,
node, path and cloning budgets remain unchanged. More than three distinct keys,
eight complete scenarios or eight result values reports a limit. Failed
original expression-budget diagnostics are retained; the retry never resets a
budget. Ordinary unknown non-i1 queries without a Boolean atom do not replay.
The fallback discovery remains bounded and is not a complete path analysis.

The four dedicated unit groups cover exact shared-condition values with
argument/comparison/freeze/LOAD atoms; three/four-key limits and unknown-arm,
undef, poison and cycle rejection; distinct old and fresh executions of one
LOAD; and legacy finite predictions plus zero budgets. Every fixture verifies
input/output IR and checks actual selector sharing with the full fallback.
The historical fixture also checks Function/Argument identity. VM old/new
execution and native differential validation are owned by the integration
stage; no execution result is claimed by this design document.

An unchanged successful enumeration does not invoke new evaluation. Whole-module
compatibility still needs the existing saved-IR/counter comparison, since newly
supported opportunities and their accounted work may change which later
opportunities fit a shared finite budget. This is not a completed-loop claim:
real fast-loop execution, fallback counts and complete-state comparison remain
separate acceptance gates.

## Implementation milestone: 2026-11-12

The implementation is integrated on H2 commit `f54e434`, retaining its complete
selected-path STORE scan. Combined production source SHA256 is
`83ad02c4481e02a97608dabb03fd90aa4cc88bd3230734f13dfeec6bebc9d7c1`.
The final four public regression groups compile against the preceding H1 runtime
and fail in both builds: 57 assertions, 11 failures, with the existing builtin
available. The initial test source had a C++ ternary pointer-type compile error;
its source and compiler failure are retained separately. The corrected source
only adds the required explicit `llvm::Value *` cast and is the actual old/new
test with SHA256
`3768aaf59394303fe1154d58f4b34b360648d9dbe649ff1212aa72ba788e1764`.

New focused runs pass 50 cases / 1855 assertions in each build. Full CTest is
1115/1115 with Z3 (129.64 s) and 980/980 without Z3 (100.51 s). Independently
prepared real-LOAD API fixtures also actually fail the shared-condition precision
gate on old H2; candidate native comparisons and formal four-ELF replay are
still pending at this milestone. No complete-loop improvement is claimed yet.

Source, old failures, build and unit logs are frozen under
`~/vmc-runs/guarded-correlated-booleans-20261112/`. The metadata archive is
`guarded-correlated-booleans-metadata-v1.tar.gz`, SHA256
`8a3802c6cd568372157a6410f9b20117c3d58687c881db245faa28b0df040a36`;
all 51 payloads were independently checked on the development host. The adapted
freeze launcher initially retained the previous H artifact basename; its rename
receipt records the filename-only correction with unchanged archive bytes.
