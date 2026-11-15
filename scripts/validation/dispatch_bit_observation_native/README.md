# Arbitrary-position bit observation: independent native validation

These are the exact validation sources executed on ROCK5B for the first
analysis-view-integration milestone. They do not execute at import except for
the explicitly invoked replay/audit scripts. The replay uses the dated frozen
artifact paths recorded in its PLAN; SDKs and LLVM21 are external dependencies.

`producer.cpp` creates twenty LLVM fixtures: five word widths (8/16/32/64/128)
and four patterns (high one-hot mask, logical sign extraction, shifted/masked
sign, and the real sub/or/complement business-condition shape). Inputs are
unknown numeric SSA values. Every fixture stores the original complete state
word before reading a possibly aliased limit, then takes a real normal/error
exit. The switch remains semantically meaningful, with distinct outcomes.
Only the selected source edge is transformed through the public batch API.

The same physical producer binds to the old SDK and both new SDKs. All twenty
old calls refuse and preserve exact IR. All twenty new calls transform; main
and no-Z3 after IR are byte-identical, and all before IR is byte-identical.
Only the common before/after IR is executed, without recounting no-Z3.

`replay.py` defines an independent mathematical bit/word model and explicit
byte stores; it does not obtain expected outputs from the optimizer. Each
fixture has128 inputs (16 value rows x two selected edges x two sizes x two
alias layouts). Normal raw and transformed IR each run at LLVM O0/O2:

- 5120 raw calibration +5120 transformed =10240 complete80-byte matches.
- 30720 comparator controls alter return/memory/layout observations, all caught.
- 2560 independently altered-program observations:648 specified return faults
  detected,1912 complete nontrigger matches.
- All input memory bytes and output bytes are compared; no skips, undefined
  input execution, exclusions or normalization.

The80-byte protocol is uint64 return +64 memory bytes +uint64 limit offset.
It is an LLVM-pass oracle, not a new machine-instruction or full Guest-register
campaign; never mix its counts with the earlier66112/66136-byte protocols.

`saved_audit.py` re-derives the model only, checks every saved input and all
100 normal/fault streams, and requires each fault output to equal its specified
complete altered frame. It performs zero new target executions. All10240
normal frames,2560 fault frames and30720 controls pass the saved audit.

ROCK root: `/home/ubuntu/vmc-runs/analysis-bit-native-20261115-v1/`.
Baseline same-client focused regression: old2cases/500assertions fails one
precision assertion; candidate and both production backends each2/789 pass.
The root-level implementation report records the separate full suites and
real-case refusal/compatibility checks.
