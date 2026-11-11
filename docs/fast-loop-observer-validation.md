# Final-IR loop observer (validation only)

`fast_loop_instrument.cpp` consumes an already verified, frozen final module and an independently derived `final-ir-loop-observation-v1` plan. It is not a Dragon pass and must not be linked against a Dragon build. No optimizer runs after insertion.

Example compilation on the owned LLVM 21 VM, in a fresh evidence directory:

```sh
clang++-21 $(llvm-config-21 --cxxflags) -std=c++20 fast_loop_instrument.cpp $(llvm-config-21 --ldflags --system-libs --libs core irreader support) -o fast_loop_instrument
```

Record the exact command, exit/stdout/stderr, source/binary hashes, `ldd`, LLVM tool versions and any resolved shared-library hashes. Raw validation coordination owns compilation and execution; these commands are not evidence that anything ran.

Invocation:

```text
fast_loop_instrument input.ll PLAN.json output.ll report.json [double-count|omit-central]
```

The tool binds the input SHA-256 and function, verifies input and output, checks contiguous site IDs and existing blocks, conditional successor slots, exact membership candidate equality with the actual small-switch cases, and the plan's central-site coverage. The plan's semantic classification and completeness come from a separate inventory audit; the tool does not infer that every switch is a central dispatcher. Restrictive defined-function memory/speculatable attributes and restrictive explicit callsite attributes (including indirect calls) are rejected rather than left stale after adding host writes. The four initial frozen real inputs have no such defined-function attributes.

Each site executes immediately before its original terminator. A visit adds one; a branch site uses the existing branch condition (or its negation); a membership site compares the existing simple same-block LOAD's SSA result with the small switch's exact candidate set. The tool never evaluates the Guest address or executes the original LOAD again. It neither splits edges nor changes original branches, switches, PHIs, metadata or original memory accesses.

Two new host symbols are exported:

```c
extern uint64_t dragon_observer_counts[];
extern const uint64_t dragon_observer_count;
```

They use independent initialized storage, volatile host loads/stores, and flag-free modular unsigned 64-bit increments. The frozen driver uses a separate 1 MiB Guest array; observer storage is not part of that mapping. A separate linked C helper can write counters to another file at process exit. The original output file format must remain byte-identical and be compared in full with the uninstrumented execution. The observed runs are separate from the formal uninstrumented semantic campaign.

The report lists every emitted and deliberately omitted site and pins the input, plan and output. `double-count` doubles increments. `omit-central` leaves the counter array ABI unchanged but omits one planned central observation. These are only fault controls, never valid real observations.

`fast_loop_instrument_selftest.py --tool ABSOLUTE_TOOL --output NEW_DIRECTORY` emits an independent small loop, compiles plain/normal/two faulty variants using LLVM tools, and compares exact return/three-word memory state and exact observation counts for 24 cases. It requires the double-count control to disagree in all 24 cases, and omitted-central to disagree in the 12 cases that actually enter the central block. It also rejects four malformed plans before output creation. All commands, raw outputs, exits and binary streams are saved. At source handoff only Python syntax was checked; actual compiler/native results must be recorded by the execution agent.

Counts alone prove neither semantic equivalence nor complete deobfuscation. The independent checker must combine full-state equality, exact site coverage, actual repeated guard visits in identified fast SCCs, entry admission, central/fallback visits and the source-linked path classification. A conditionally closed fast graph that real inputs never use is not a completed fast loop.

## Observer self-check result

The execution agent compiled observer v2 against LLVM only and completed its frozen self-check: 96 full-state comparisons, 24 normal exact-count matches, 24/24 double-count detections, and 12/12 specified omitted-central detections. Four malformed plans, four restrictive-attribute cases and reinstrumentation were rejected; input files remained unchanged. This is an observer test, not a real-loop execution claim. The VM evidence is `~/vmc-runs/fast-loop-observer-20261112/tool-v2`; the tool SHA-256 is `be962c5e31ddee2e00bbb6535a49abc1f84af4ba13c1fd4eeba2703ae413aa49`.

A real repeated-fast-loop gate must require a repeated **same guard site** within a structurally closed fast SCC, not a sum of visits to distinct guards. The current one-invocation fixtures can require one observed entry and one successful admission, no admission fallback, no target/business failure, no central visits, and at least two successes at the same SCC guard. Multiple invocations require separate accounting. Counters are scoped to the selected guarded function; uninstrumented ordinary recursive callees cannot be claimed dispatcher-free from this observer.
