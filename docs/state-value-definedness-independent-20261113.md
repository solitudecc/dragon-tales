# Definedness of the extra state-value guard

The new state guard needs its own defined branch condition. Reading a simple
LOAD does not establish this. A public input can load an uninitialized alloca,
mask with3, and compare the result unsigned-greater-than50. That comparison is
always false, so its original branch and return7 are defined. An additional
EQ2 can still be undef; branching on it adds immediate undefined behavior.
The initialized concrete native fixtures do not test this contract.

[LLVM21 LangRef](https://releases.llvm.org/21.1.0/docs/LangRef.html#br-instruction)
and its [alloca](https://releases.llvm.org/21.1.0/docs/LangRef.html#alloca-instruction)
semantics establish that witness. Freeze only the fresh EQ's i1 result before
the new branch, keeping the original state, ICmp, effects and fallback intact.
For a defined state it leaves the decision unchanged. For undef, a true EQ
choice corresponds to a feasible state=k, for which the original one-constant
ICmp permits the predicted direction. Other old state uses retain their choices.
For poison the original ICmp/branch already has undefined behavior. This argument
supports the narrow branch refinement; it does not prove a range, memory fact,
hint reachability or all-runtime hint success. The [freeze semantics](https://releases.llvm.org/21.1.0/docs/LangRef.html#freeze-instruction)
provide the new condition's definedness.

The independent LLVM21.1.8-only probe records: upper62 state bits known zero;
original ICmp simplifies false with `CanUseUndef=false`; extra EQ is not proved
defined before the new branch use; freezing that EQ is guaranteed defined.
No undef program is executed natively. No Alive2 binary is available, and this
is not a formal proof of the complete transformation.

The first probe incorrectly asserted that ValueTracking would report an already
branch-consumed EQ as non-defined. That assertion failed and is preserved.
LLVM's [ValueTracking implementation](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.8/llvm/lib/Analysis/ValueTracking.cpp#L7140)
can infer definedness because using undef there would already make the queried
program undefined. The corrected probe records both before-use=false and
attached-use=true. The latter cannot certify that introducing the use was safe.

The original v1 oracle model/inputs/faults/driver remain byte-identical. A new
`guarded_state_values_prepare_v2.py` uses a strict audit: one freeze i1 of the
fresh EQ on the original state and requested literal; branch consumes exactly
that freeze; original state pointer/raw/state/ICmp definitions and fallback
condition/successor slots stay unchanged. Checker controls include two verified
synthetic positives and eighteen rejections, including actual old outputs,
freezing the wrong comparison, bypassing freeze and altering state/direction.
The first checker was too strict about the existing generated-terminator
annotation; its failed run is retained. Only that known annotation is handled
when comparing the original branch operands. Full native bytes are unchanged.

Independent diagnosis and checker evidence are under
`~/vmc-runs/state-value-definedness-independent-20261113/`. At that diagnosis freeze the separate
native profile `guarded-state-values-defined-condition-native-20261113` awaited
the repaired runtimes. Its subsequent replay is recorded separately in
`guarded-state-values-defined-condition-native-20261113.md`; the diagnostic
archive itself claims no new native success.
