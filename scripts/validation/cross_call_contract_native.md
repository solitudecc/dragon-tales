# Cross-call contract isolation regressions

These fixtures demonstrate three independent reasons to leave experimental
cross-function selector proofs outside generic `dispatch-deflatten`:

1. A correct `CallMemoryConstant` postcondition says that **both** callee return
   paths write state 42. The callee's entry state may still be 1 and return 17.
   Using the postcondition as an entry fact incorrectly chooses the 99 result.
2. A caller writes through a pointer loaded from outer state + 8. The callee
   reads through a different pointer loaded from outer state + 0. Equal inner
   offsets do not establish equality of the two objects.
3. A caller stores state 1, but the callee overwrites it with 2 before a later
   dispatcher. A call-entry fact does not prove that later selector value.

The metadata in the first case is a valid complete write effect, rather than
an intentionally false summary. It remains useful after normal return to a
consumer with a proven address relation. The compatibility option is set to
true explicitly: it must not reactivate the isolated experimental fallback.

Build the small producer against an existing library, in a fresh output
directory. No repository build or source modification is needed:

```sh
mkdir /tmp/cross-call-contract-run
c++ -std=c++20 -O0 \
  -I/home/cc/dragon-tales-overlap/include -I/usr/lib/llvm-21/include \
  scripts/validation/cross_call_contract_native.cpp \
  /home/cc/dragon-tales-overlap/build/lib/libdragon.so \
  -Wl,-rpath,/home/cc/dragon-tales-overlap/build/lib \
  -o /tmp/cross-call-contract-run/generate
/tmp/cross-call-contract-run/generate /tmp/cross-call-contract-run
```

The producer executes the actual Dragon pass, verifies its input/output and
saves `before.ll`/`after.ll`. Compile and run each module at O0 and O2, linking
the independent scalar/memory C oracle in `cross_call_contract_driver.c`.
For example:

```sh
clang-21 -O0 -Wno-override-module /tmp/cross-call-contract-run/after.ll \
  scripts/validation/cross_call_contract_driver.c \
  -o /tmp/cross-call-contract-run/after-O0
/tmp/cross-call-contract-run/after-O0
```

The four products make 24 comparisons total. The old experimental fallback
produces 12 before matches and 8 errors among 12 after comparisons, equally at
O0/O2. It reports three cross-call rewrites (one summary, two direct stores)
despite all modules passing LLVM verification. Artifacts from the actual old
run are `/tmp/cross-call-contract-old-20261111/` on the VM; the producer's
library hash before and after was identical.

An isolated generic pass should report zero cross-call rewrites and preserve
all 24 results/memory observations. There are no exclusions or skips. These
are LLVM contract regressions, not a machine-lifter or full-state corpus proof.
