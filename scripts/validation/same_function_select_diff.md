# Conditional state and reaching-store validation

This is a separate check from `same_function_deflat_diff.py`; it does not change
that check's fixtures or output contract. It links a small producer against an
existing library and writes only a newly created output directory:

```sh
python3 scripts/validation/same_function_select_diff.py \
  --repo /home/cc/dragon-tales-overlap \
  --library /home/cc/dragon-tales-overlap/build/lib/libdragon.so \
  --work /tmp/conditional-state-native-new
```

The producer calls the actual same-function pass and verifies before/after IR.
The independent C and Python oracles check return value, both state words, both
side-memory words and the unknown-writer call count. A reader declared readonly
has a genuinely write-free body; the unknown writer has no effect attributes
and is linked only after the pass has run. A same-function fact does not enable
cross-call entry specialization.

The 16 native fixtures cover freeze/noundef conditions for SSA PHI and memory
selectors; distinct, default and same destinations; preserved outer guards;
a bounded loop; retained computation, simple load and readonly-call gaps; and
an unknown CALL before versus after the final state store. Each runs with both
condition values, both outer guard values, and inputs 0, 1, the sign bit and
UINT64_MAX. The simple-load fixture also uses the guard input to choose a
separate or aliased state/side pointer. The loop executes one to three iterations.

Before/after at native O0/O2 give 1024 normal comparisons. Four copies of that
matrix each flip only the result bit for `select_freeze_phi(c=0,g=0,x=0)`, giving
1024 control comparisons and four specified control detections. Missing,
duplicate, malformed or incorrectly labelled rows/fields fail the checker.
All compiler logs, source snapshots, commands, IR, input keys and library hashes
are retained. There are no skips, exclusions or normalizations.

The unit group `[conditional-state]` has twelve cases with 71 fixture variants.
It additionally checks poison/undef/unproved conditions (also with equal
destinations), unknown arms, `freeze(select poison,...)`, live-out and target
PHIs, small shared scan/expression budgets, and constant conditions with an
unchosen poison arm. The latter includes NoFolder-produced i1 XOR expressions,
so a proof must preserve the old chosen-arm precision. Frozen `trunc nuw/nsw`,
`zext nneg`, and overlapping `or disjoint` also must not be treated as unique
constants; their actual frozen boolean values can still drive a conditional
branch. Ordinary OR remains a positive precision control. These nondeterministic/
poison contracts are structural unit tests and are not executed as native code.

The frozen 26e1fce-production library (`a33dc8...cf699`) gives visited 16,
rewritten 1, edges 1, unresolved 16: only the loop's initial constant edge is
recovered. It passes the complete 1024-row scalar/memory native matrix and all
four controls. This establishes the fixture semantics and compatibility, not
the requested optimization gain. The final base eight unit cases compiled on
the old library and failed 71 of 649 assertions. The two flagged-cast cases
failed 32 of 102 assertions, and the two disjoint-OR cases failed 10 of 67;
the ordinary OR controls passed. These are actual old-library controls, with
verified IR, rather than expectations inferred from source review.
Raw baseline evidence is in `/tmp/conditional-state-baseline-26e1fce-final/`
and `/tmp/conditional-state-unit-51-26e1fce/` on the VM; the added flag controls
are in `/tmp/flagged-state-unit-26e1fce/` and `/tmp/disjoint-or-unit-26e1fce/`.

The final frozen production backend has SHA256
`7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799`.
Its actual aggregate counts are visited 16, rewritten 13, edges 14, unresolved
3, cross-call rewrites 0, with both modules valid. All 1024 normal observations
matched. All four specified result-bit controls were detected across the 1024
control observations; skips, exclusions, normalizations and infrastructure
errors were zero. The two outer-guard selects and the store-before-unknown-CALL
path remain unresolved. This gain retains the original condition, memory reads
and effects, rather than narrowing a cross-call context.

Final native evidence, including all input/source/IR/library hashes and command
logs, is in
`/home/cc/vmc-runs/llvm-local-state-20261111/final-native-conditional-state/report.json`.
An independent unit executable, with `ldd` confirming the same frozen backend,
passed all 850 assertions in the twelve `[conditional-state]` cases. Its receipt
is in the sibling `final-conditional-state-unit-independent/` directory.
This is scoped LLVM scalar/memory/call-count evidence for the listed fixtures,
not machine-instruction or whole-corpus semantic equivalence.
