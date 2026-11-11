# GuestMemorySSA PHI-edge repair: real saved-IR compatibility

Four bounded analysis executions compare the prior backend with the PHI-edge
correctness repair on the frozen recipe-B f-cff O0 and f-combo O0 final
`after-bound.ll` modules. Both modules produce byte-identical complete output
IR under old and new backends. This is compatibility evidence only; it adds
zero target-program native executions and does not repeat previous C/LLVM
oracle successes in the current count.

The exact single-pass recipe is builtin `guest-memory` with
`run_mem2reg=0, edge_worklist=1`, iterations1/pass-runs1 and LLVM verification.
The same compiled driver, exact input pathname and fixed ModuleID are used for
both backends. Input parsing uses `parseAssemblyInto` on the lifter's existing
module. Inputs, canonical pre-pass IR, full post-pass IR, verifier results,
reports, compile source/command, execution argv, timings and pins are retained.
Each IR was gzip-compressed only after round-trip byte verification.

Predeclared manifest:
`7e7901f9fbf61bcd19b5798c244b2e610336b9549bf30ada6152afddd9bcd500`.
Input archive `recipe-B.tar.gz`:
`107a7879475e07c2129b1b102bb972d45ccb8d1964e35eef351bcac41ca608cf`.

Runtime bindings:

- Old backend: `34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f`.
- New backend: `d6cfa1bfc4898abcd2ab771a368010c247b78eb98b27f93f9908b42e9ddd3c2f`.
- Both use shim: `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`.

All four invocations return0, both pre/post modules verify, and complete old/new
reports also match. In each backend, f-cff reports195 Guest LOADs,252 STOREs,
0 forwards,0 inserted PHIs, changed0, incomplete/budget1. Combo reports427
LOADs,423 STOREs,0 forwards,1 inserted PHI, changed0, incomplete/budget1.
Operational insertion counts are preserved as reported; no completeness or
new optimization benefit is inferred from these results. No comparator,
inputs, budgets or exclusions were changed.

The old/new complete output equality is stronger than a switch/count comparison
but does not cover executions or different input modules. The actual PHI-edge
bug is covered by the separate focused regression and native validation owned
by the main implementation stage, not by falsely calling these unaffected
real modules old semantic failures.

Evidence: `guest-memory-phi-edges-real-20261112.tar.gz`, available on both hosts
under `~/vmc-runs/` and `/Users/c/Desktop/deob/validation-artifacts/`.
The archive's exact file set and every payload size/SHA are verified on both
hosts. `REVIEW.json` records full IR hashes and counts; `review.py` reproduces
the comparison. The prior runtime is no longer needed by this validation and
may be restored from its existing verified archive rather than relying on an
expanded runtime path still being present.
