# Explicit guarded-transition pilot (commands prepared; not executed)

Wait for root's approved runtime exporting cloneGuardedTransition. Use a new
owned namespace such as ~/vmc-runs/guarded-transition-prototype-20261112.
Preserve exact argv, stdout/stderr/exits, source and runtime hashes.

Copy the driver, GuardedTransition.hpp, contract and exact input IR there.
Input SHA: dab36d3d945342c283ecfac097fa84a6ba47de5f0e01e5e030914979586343ed.
Set transition_runtime to the approved frozen runtime, not the mutable build.

Compilation draft:

    c++ guarded_transition_explicit.cpp -I. $(llvm-config-21 --cxxflags) \
      -std=c++20 -fexceptions -O1 \
      -L"$transition_runtime" -Wl,-rpath,"$transition_runtime" -ldragon \
      $(llvm-config-21 --ldflags --system-libs --libs core irreader support) \
      -o guarded-transition-explicit

Preferred terminal-only request avoids copying existing state STORE/bounds again:

    ./guarded-transition-explicit before.ll guarded.ll helper-report.json \
      dragon_static_entry_x86_64_102a0.guarded.real_sp \
      memory.valid11.state.context807 1 66450 \
      memory.valid13.state.context809 - 32 2048
    opt-21 -passes=verify -disable-output guarded.ll
    opt-21 -S -passes='mem2reg,instcombine,gvn,simplifycfg,verify' \
      guarded.ll -o standard-cleanup.ll

This cleanup is a separate diagnostic, not a change to the frozen three-round
recipe. Check whether SimplifyCFG merges the guard back into the switch.
Record any recombination; do not add opaque effects/barriers to force a gain.
One may separately assess structural recovery AFTER final control-flow cleanup.
Helper success/verification is neither native correctness nor full loop recovery.

Optional full-path request, separately named and never silently substituted:

    ./guarded-transition-explicit before.ll full-path.ll full-path-report.json \
      dragon_static_entry_x86_64_102a0.guarded.real_sp \
      memory.valid225 0 66450 \
      bb_66234.guest.edge533,memory.valid7.guest.edge534,bb_66252.state.context806,memory.valid11.state.context807,memory.valid13.state.context809 \
      0,1,0,1 32 2048

The driver saves a parsed baseline. Parser use-list ordering is not a transform.
The production API has no sample address/name assumptions; these are explicit
diagnostic selectors only.
