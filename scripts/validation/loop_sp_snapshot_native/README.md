# Exact guarded SP snapshot: finite native check

This is a validation-only, bounded oracle for the NoFrame SP-observation fix.
It does not change a product pass or its defaults. `source.py` extracts the
exact frozen `LoopFixture::NoFrame` input from
`TestLLVMLoopImageTableContext.cpp` (SHA256
`4445ecfae163b3cf2e95b148296e4df06cc2a891b43df5e985476768b4fde483`).
The only finite-control changes are `b.end: br %head` to `ret void` and removal
of that actual incoming edge from the head PHI. The whole Module is verified
before and after the installed public `runLoopImageTableContext` call.

The C and Python models are independent byte-packed implementations. The arena
and complete descriptor-checking wrapper are reused unchanged from
`phi_low_bits_native_source.py`; the 13 comparator corruption controls, physical
single-XZ EOF checker and record framing come from `dispatch_corridor_native.py`.
The full comparison covers all 66,112 bytes: Guest[65,536], the external
512-byte register snapshot, the seven-u64 Exit, and the final returned-register
snapshot. A selected input makes RegBytes physically overlap Guest. No address,
flag, Exit or unmodified byte is normalized or excluded.

Nine new arenas cover two ordinary guard hits, a physical RegBytes/Guest-alias
guard hit, and SP/output/table0/table1/PC/size misses. The ordinary fallback has
observable register, Guest and all Exit-field effects. Fault IRs alter only
the raw input: terminal output 321 to 322, or removal of the fallback call.
Nontrigger cases must match the complete expected arena as well.

Preparation uses an owned, nonexistent output directory:

```text
python3 source.py RAW_DIR EXACT_FROZEN_TEST_TU EXACT_FROZEN_PHI_LOW_MODEL
```

Stage the three source files, the generated raw files, the two public headers
(`ImageTableContext.hpp`, `Execution.h`), and the original shared comparator as
`shared/dispatch_corridor_native.py`. Supply an explicit `PLAN.json` binding
every staged source/input/expected file and the exact compiler, llc, Python,
nm, SDK and solver path/size/SHA256. The frozen executed plan is in the evidence
root below; it contains the complete argv and file pins, not a path inferred
from an active build. Run `taskset -c 0-3 /usr/bin/python3 runner.py` there.

Each child command has a 180-second process-group deadline, a 512 MiB startup
free-space floor and a 64 MiB running floor. The order is fixed: C and raw LLVM
O0/O2 calibration, both fault variants and saved audits, then exactly one
installed public API call, then after LLVM O0/O2 and saved audits. The producer
has only an undefined API import, uses no implementation object or shared LLVM,
and records its actual SDK and solver mappings. The same arena inputs and
independent expectations serve every engine.

The first execution on ROCK5B passed without a fixture/harness revision:

- Before: C 18 + original LLVM 18 = 36 complete matches.
- After: LLVM 18 complete matches; 54 normal matches total and 702 controls.
- Faults: 36 observations, 16 specified differences and 20 complete nontrigger
  matches. These are deliberate controls, not old-product execution defects.
- Six saved streams contain 90 records, each preserving input, expected and
  actual full arenas. All record keys, independent expectations, hashes,
  comparisons, aggregate counts and physical XZ EOF were rechecked.
- One public API call closed with work 2,056, no exhaustion, preserved the two
  actual target reads and outside-function text/Function/Argument identities.
- Unicorn calls, new machine-instruction tests and full CTest runs here: zero.

Executed plan SHA256:
`c01d003aff03f4a950a5f81561337602db6ca0d23ae1fcc959553374450df2b1`.
Summary SHA256:
`6b324a96a4ba05aed67595ac0c664ad3fc59e292044b5283438334c955cc8894`.
Fixed SDK SHA256:
`48cbb8a3aa4002f8942655ba2716088366060676a525319e178d79bad1125db0`.
The same Z3 `.so.4.13` is bound to
`4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`.

Evidence is frozen under
`/home/ubuntu/vmc-runs/loop-sp-snapshot-native-20261115-v1` and mirrored in
`/Users/c/Desktop/deob/validation-artifacts/loop-sp-snapshot-native-20261115-v1`.
The focused same-client API test is separate: old edab 2 cases/48 assertions
fails only the new positive closure assertion; new 48cb 2 cases/77 assertions
passes. Neither those assertions nor earlier native campaigns are added to the
counts above. This nine-frame finite check does not prove all-input equivalence
or extend the arbitrary-Guest execution contract.
