# Observer admission for pure intrinsic declarations

`CallBase::hasFnAttr` also observes the called declaration's attributes. The
validation-only observer consequently refused the frozen cffO2 final IR because
its unchanged intrinsic declarations carry `memory(none)` and speculation
attributes. The old refusal occurs before any native execution.

The repair admits only a direct call whose target is an intrinsic declaration,
whose call is proved memory-none and nocallback. That intrinsic has no body
instrumented by this tool and cannot call back into the observed function; its
original effect and speculation promises remain true. All attributes stay
unchanged. Defined-function restrictions and every other restrictive callsite
refusal remain. This changes only validation tooling, never a production pass.

The new regression actually fails on old tool-v2 with `callsite has restrictive
effect attributes`. Both inherited and explicit pure-intrinsic attributes pass
the repaired tool, stay byte-equal to canonical input, and produce48 native
state pairs/96 executions with exact return,32-byte Guest and entry count1.
Five verified negative inputs still refuse without output: selected-function
effects, readonly selected-view caller, defined pure function, external pure
declaration and an effectful intrinsic. The original selftest is unchanged and
passes all24 normal/96 full-state observations,24 double-count detections,12
specified omit-central detections, four invalid plans, reinstrumentation and
four original effect-attribute refusal cases. Every command keeps the original
finite180-second limit; no comparison, exclusion or timeout is relaxed.

The new instrumenter SHA-256 is
`6f9f6288484448a900fdbf27e88d48e06c38414b1b781485ba529a9d11fee8a2`.
It links only the existing dynamic LLVM21 library and does not link Dragon.
The tool archive has953 payloads and is exact-file-set/hash verified on VM and
Mac: `fast-loop-observer-pure-intrinsics-20261113.tar.gz`, SHA-256
`cd5a9825118a799474f59b52de1e108a5cafeea710726c9d81d6fe1f8cbd03ec`.
No Dragon rebuild or new CTest/Unicorn/corpus campaign is claimed for this
validation-only change.

A separate cffO2 replay retains the original failed attempt and runs a new
namespace with only the executable changed. The raw IR, PLAN, driver, eight
frozen inputs/expected outputs, gates and per-command180-second limit are
byte-identical. Four normal and four double-count executions exactly match the
original full states. Every normal run enters once, has zero entry fallback,
target/business failure and central-dispatch counts, and repeatedly executes an
actual guard in a fast SCC; guard id5 executes12 times on every input. All
four bad-counter streams are precisely twice normal and are rejected; the
omitted-central emission control is detected.

`fast_loop_saved_review.py` independently parses the actual raw CFG, matches all
inventory edges, checks same-LOAD EQ/full fallback and small-switch cases,
recomputes fast SCCs of8 and90 blocks, rechecks every saved count/state and
compares all intrinsic declarations/attribute groups across normal/double/omit
emissions. The inherited runner's SUMMARY.scope retains a historical cffO0
label; actual cffO2 identity is bound by PREDECLARED and the independent review,
never inferred from that old string. This is evidence for the frozen cffO2
view, not four-module or arbitrary-program loop closure.

The real replay archive includes both the original refusal and new run:
`fast-loop-observer-cffO2-L2-intrinsic-validation-20261113.tar.gz`,139 payloads,
SHA-256 `8d49252b6ebfd7983c47b0ee87b3d3cb9e984f20005a5cb40f24cba544bfe53c`.
Both host archives are under `validation-artifacts/` and `~/vmc-runs/`, with
complete file-set/length/SHA verification receipts. No evidence is retired.
