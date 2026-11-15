# First observation-range main CTest: saved-only failure review

This review executed no compiler, target, solver, native harness or CTest and
changed no check or budget. It read the completed first-run receipts at
`/home/ubuntu/vmc-runs/loop-image-observation-range-main-full-20261115-v1`.
It read MBA result rows only from the separately frozen failure payload, not
the mutable build/tests paths reused by a later run:
`/home/ubuntu/vmc-runs/loop-image-observation-range-main-a55-failure-payload-20261115-v1/payload`.
Its owner's full copy audit is
`loop-image-observation-range-main-a55-failure-payload-audit-20261115-v2/MANIFEST.json`,
SHA `adad03c80a38d774a9d623e3368c189a537c0ed0c24e65f2dd316ddee9f6c7a3`.

The original result remains FAIL: 1298 of 1305 registered tests passed, seven
failed and none skipped. Overall CTest exit was 8 at 634.572896495 seconds;
the 900-second suite guard itself did not expire. PLAN, controller and execution
receipt agree on CPU0–3 and parallelism 3. Source/runtime stat identities and
hashes remained equal before and after. These seven failures are not silently
converted into passes by the classifications below.

Four failures were unmet MBA precision gates. The C/Python test stopped at
`a64-add64`: cone 4 with add/and/shl/xor instead of the required cone limit.
The other three tests completed their saved rows. Normal has all 32 complete
state matches and 32 root returns; result and memory controls each have 32
specified differences and 32 root returns. This reviewer checked every row,
including the exact one-field control requirement (X0/RAX or memory), zero
skips/exclusions and 65536 compared Guest bytes. Register-field counts are
76 or 102 according to architecture. These are reread results of that failed
full run, not new native executions or a recounted independent campaign.

Each diff campaign still fails its precision gate: x64-add64 static O0 and O2
have cone 4, expressions_simplified 0, solver_queries 1, unknown_queries 1,
rejected_candidates 0 and budget_skipped 4. No saved reason_unknown exists.
The source increments unknown both for a Z3 unknown response and for a caught
z3::exception. Thus the counter does not prove a timeout, resource-limit cause
or exception. The existing code correctly leaves an unproved expression
unchanged; failed precision is distinct from an observed semantic mismatch.

The correct default MBA per-query timeout is **100 ms**. Python's
run_mba_simplification, MBASimplifyOptions and the builtin mba executor all use
100, with the existing 1M resource limit. Predicate folding uses 25 ms.
An early diagnosis description accidentally borrowed the predicate number;
this source review identified it and the owner corrected the wording. No
execution option or check was changed in response. Calling these four failures
"25 ms MBA timeouts" would be unsupported on both the numeric default and the
unrecorded solver reason.

The three diff summaries collect actual libdragon paths from /proc/self/maps
and hash them. Each reports only the main backend
`2c9ca7a777359f24b0ccf4a5bb88744297f1e0a86dc0376955c02310b20cb7ba`
and shim `9d95c6402f624f277900746e3493f2e65039a8e51b93561fde66454e84583efa`.
Their per-module captures also check actual MBA availability. The summary's
proofs_available field alone is argument-derived and is not a binding proof.
The early C/Python assertion did not save that process's maps. For it, only
the explicit configuration and immutable file receipts were observed here.

All seven registered commands explicitly set DRAGON_C_LIB to main
`build/c_shim/libdragon_c.so`. The controller sets LD_LIBRARY_PATH to main
build/lib plus the original wheel Z3 directory, and clears LD_PRELOAD.
Python _find_lib honors that existing explicit path; ffi.cdef reads the tracked
static headers directly and ffi.dlopen opens the selected shim. This path has
no generated shared C_LIB selector or cffi wrapper file that the NoZ build
could overwrite. The relevant source/header files were pinned unchanged.
The main and NoZ build directories and their CTest environment properties are
separate. No evidence of a NoZ backend mapping or generated-wrapper mix-up
was found. This does not invent a missing per-process loader trace for the
early C/Python failure, or rule out load contention as a timing factor.

Three other failures are actual CTest per-test timeouts: static Guest calls
and boundaries (180.115 s), external API images and boundaries (180.062 s),
and complete Guest CALL/RET (180.125 s). Each has TIMEOUT=180 in REGISTERED.
Saved output prefixes contain completed match records, and the final printed
records also match. The planned campaigns did not complete; their partial
prefixes are not full semantic passes. No completed mismatch was observed in
those saved prefixes. No exclusion, timeout increase or partial-success rule
was added.

The independently checked bootstrap reference receipts are
`rock5b-main-ucdev-reference-validation-20261114-v1/CTEST-INVOCATION.json`
and CTEST-RESULT.json. They record CPU4–7, ctest -j4, 1233/1233 passing and
229.347787144 seconds under the same 900-second suite deadline. The earlier
first-build stock-reference run took 193.811671497 seconds but failed two
RET-reference cases; it must not be mistaken for that corrected passing run.
The later Batch full receipt also records a passing -j4 run at 195.439686911
seconds, but its full.json alone does not encode affinity and is not used to
invent one.

Local Git comparisons against a7e2cb5 established byte equality of the MBA
implementation, options header, Python API, MBA diff harness, C/Python MBA
test and MBA fixtures. The harness and fixtures match the saved result hashes
`2e9ac6c7368c44e5f9061d0baa691313e514dd0f875c6d9fa6ce2152d5052b69`
and `c4d3793d55cbe78e53d3965be28a0f2d6463546f0d6dfafc3f6174c27cad1108`.
The whole suites nevertheless have different registered counts, core affinity,
parallelism and concurrent-build conditions. Their elapsed times do not prove
that A55 affinity was the sole cause. At this review cutoff a restored-baseline
CPU4–7/-j4 run was pending, with old checks and solver budgets intact. Its
actual result must be recorded separately; even a later pass does not erase
the original seven failures or uniquely identify their cause.
