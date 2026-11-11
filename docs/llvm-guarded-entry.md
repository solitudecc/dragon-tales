# Guarded entry views

`LLVMLifter::createGuardedEntryView` creates an independent module with an
additional entry function. Candidate register and Guest-memory bytes are checked
in the generated IR. A matching entry can be simplified using ordinary LLVM
passes; a failed check calls the original function. This is an explicit,
optional analysis view. It does not change the original module, the default
pipeline, the Graph, or the machine program.

The analysis remains static: constructing and optimizing the view does not run
the target. The output contains both the conditional fast path and the ordinary
fallback. A conditional simplification is not an unconditional recovery of a
table's contents. An ELF reader may propose bytes, but neither ELF section flags
nor those proposed bytes establish immutable Guest memory.

## Contract

The source must use `StaticAnalysis` with `FunctionStart`, and retain the raw
lifter's canonical register initialization and single-entry PC switch. Optimized
or already guarded sources are rejected. The selected entry is identified by
its original symbol and checked architectural PC, not a guessed block label.

The returned `LLVMLifter` owns its LLVMContext and a bitcode clone of the complete
module. The ordinary functions remain available. The added symbol is
`<ordinary-root>.guarded.<contextId>`. Internal and recursive calls continue to
use the ordinary root. A loop backedge enters the machine body directly and does
not check or reapply entry facts. The source and caller-owned input vectors may
be destroyed after successful creation.

Checks run only on the matching external-entry PC edge. Existing register
snapshots supply register comparisons. Descriptor sizes and non-null bases are
checked before additional Guest reads, which use alignment 1 and read only the
requested byte spans. A mismatch or insufficient size invokes the ordinary
root; a wrong PC retains the ordinary entry-default behavior. Existing ABI
requirements for valid descriptors and mapped backing buffers still apply.

The view adds no `llvm.assume`, synthetic Guest STORE, `noalias` promise or
immutable-memory attribute. Possible aliasing writes and unknown calls still
invalidate downstream facts. The guarded function does not inherit function
optimization attributes whose promises have not been re-established after the
new reads and fallback call. Sources with convergence or duplication restrictions
are conservatively rejected. Entry locals must remain private register snapshots.

This is an Execution ABI transformation for raw lifted machine regions and their
ordinary Guest helpers, not a general equivalence transformer for arbitrary LLVM
programs that inspect host call frames, use nonlocal host control transfers or
expose host stack addresses. `module()` remains mutable: arbitrary subsequent
edits are not certified by the API or its manifest.

## Inputs and ownership

C++ uses `GuardedEntryContext`, containing `entry`, `contextId`, `provenance`, and
two vectors of `GuardedEntryBytes { offset, bytes }`. Register offsets refer to
the byte-packed register file; memory offsets refer to Guest memory. Register
facts must be covered by the existing entry's register loads.

- Context IDs: 1–64 ASCII letters, digits, `_`, `.` or `-`.
- Provenance: 1–4096 UTF-8 bytes, without NUL.
- At most 256 nonempty ranges and 4096 fact bytes in total.
- Overlapping ranges and offset overflows are rejected, including equal overlaps.
- Sources are limited to 4096 functions, 4096 globals, one million instructions
  and 64 MiB serialized bitcode. Serialization incurs its cost before the byte
  limit can be checked. A little-endian 64-bit host pointer/index layout is required.

The C API reuses `dragon_finite_entry_state_t` as a synchronous input format;
these bytes are candidates for checks, not unconditional initial-state facts.
`dragon_llvm_lifter_create_guarded_entry_view` returns a separately owned lifter
or null. Free its optional error string with `dragon_cstr_free` and destroy the
view with the usual lifter destructor.

Python exposes the same operation:

```python
facts = dragon.FiniteEntryState(
    entry,
    registers=[(32, 64, candidate_sp)],
    memory=[(table_address, candidate_table_bytes)],
)
with source.guarded_entry_view(
    facts, context_id="table_view", provenance="candidate bytes from this ELF"
) as view:
    manifest = view.guarded_entry_manifest()
    with dragon.PassPipeline(["mem2reg", "instcombine", "gvn"]) as pipeline:
        report = pipeline.run(view)
    guarded_symbol = manifest["view_symbol"]
    ir = view.ir()
```

Consumers must explicitly select `view_symbol`; merely exporting the module
still leaves all original symbols intact. The view is placed first for existing
module-wide pass budgets. Ordinary functions retain their relative order. No
claim is made that optimizing the whole view produces byte-identical optimized
ordinary functions to a separately optimized source module.

`guarded_entry_manifest()` / `guardedEntryManifest()` returns the source bitcode
SHA256, selected symbols, checked bytes, minimum sizes, context ID and provenance.
Ordinary modules return `{}`. The manifest records provenance; it is not a proof
certificate for arbitrary later IR edits.

## Verification scope

`TestLLVMGuardedEntry.cpp`, `TestCGuardedEntry.cpp` and
`tests/python/test_guarded_entry.py` cover ownership, source preservation,
admission, limits, loop/recursive routing, attribute handling and API errors.
The API does not exist at baseline `7de235d`; the final C++ and C tests fail to
compile against that baseline's headers, and Python reports missing native
symbols. That is evidence of a new API, not an old runtime semantic defect.

`scripts/validation/guarded_entry_native.cpp` builds independent LLVM fixtures
using the real lifter's entry prologue and default exit. Its byte-array oracle,
`guarded_entry_diff.py`, compares complete shared arenas and call counts, including
Guest/register/descriptor/Exit aliases. It tests normal, mismatched, short and
wrong-PC entries, writes, calls, loops, recursion and base reloads. Protection-page
subprocesses check that failure paths do not read unavailable table bytes.
Deliberately faulty IR and comparator mutations validate the checker itself.
`guarded_entry_saved_audit.py` independently rechecks saved snapshots;
`guarded_entry_precision_audit.py` separates the body LOAD from guard-check LOADs.

`guarded_entry_real.py` uses the frozen four-ELF oracle. Its adapter changes only
the exported wrapper's one verified first call to the selected kernel. Recursion
and fallback retain the ordinary symbol. Candidate table bytes come from the
original ELF and are runtime checked. The SP-only and SP+output-pointer recipes
are separately recorded. Real success requires a CFF target-selector LOAD or
switch simplification in the guarded body; checking API availability or passing
a semantic replay alone is insufficient.

Validation results and remaining precision limits are recorded separately in
the stage report when those runs finish. No new instruction campaign, general
heap analysis, VM devirtualization or standalone rewritten binary is included.
