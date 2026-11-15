# Loop range depth accounting: same-client validation

This is a standalone synthetic LLVM precision and rollback regression. It is
not a Guest-native/C/Unicorn comparison, a CTest registration, or proof that the
real Flatten sample has closed. The program, all bounds and checks were kept
identical between the two SDK executions.

The frozen test TU is `tests/lifters/TestLLVMLoopRangeDepth.cpp`, SHA-256
`806345f09666de6bb8360bff8f3c2c9cd3da12623beb0b7e091cd7a2dca28242`.
It copies the original five Loop fixture cases and adds three cases. The new
positive keeps the original unmasked B address, actual C LOAD, STOREs and
guard/fallback behavior. A 24-block pure multiply/xor dependency chain is
physically listed in reverse order; its final xor is calculated so a C value
of 32768 still yields output address 32768. No assume or early replacement of
the LOAD is inserted. The request retains and checks the existing default
1,000,000 work budget. Separate tiny-budget controls use 1/32/256.

| Execution | SDK SHA-256 | Cases | Assertions | Positive |
|---|---|---|---|---|
| Before | `b8a1814a6bda9b952ebea0080176e7690c535ef6e762d79b2515e300214fff63` | 7 pass / 1 fail | 1170 pass / 1 fail | Refused at work 4710 |
| After | `b243a5c294c3a83b9f7c000afd53c0268170ba64dd02876c0241a36938f03090` | 8 pass | 1193 pass | Closed at work 5462 |

There were zero skips. Both used the same physical executable, SHA-256
`bacfde2e23007492ac20fc98b49199c9191c0e819ddbf04203ccc3a9d0e2004b`.
Its public Loop/Closed API symbols are undefined imports resolved by the SDK;
no product CPP or helper object was linked into it. The installed public
headers were byte-identical. Explicit library paths and saved LD_DEBUG traces
bind each run uniquely to its frozen SDK and the original wheel Z3 library.
All command pins remained unchanged. The after run did not recompile anything.

The before failure was exactly:

```
loop local range expression cap: value=4008 point=a depth=33 nodes=5
```

The 1M total budget was not exhausted. The old CFG predecessor distance had
entered the scalar dependency depth while proving a preceding STORE address.
This was a conservative precision rejection, not an observed wrong result.
The positive stopped at REQUIRE(closed), so its later output/error/fallback
checks were not counted as passing in before. In after they all ran: original
target reads and pointer LOAD remained, the output STORE used 32768, the entry
PC-default error and ordinary guard-miss fallback remained, and checked object
identities held. The wrong-magic program still refused with exact rollback;
its actual reason in both runs was scalar depth 33, not a native fault
detection. Original negative and tiny-budget cases also passed.

The accounting change separates scalar dependency depth from CFG-memory
traversal depth. It does not raise either 32 limit, root node/point limits of
128, or Image's shared work limit. STORE values and possible-alias addresses
are still queried at their original STORE, all actual predecessor slots are
checked, and nested LOADs retain scalar depth and memoryActive cycle checks.

Before evidence:
`/home/ubuntu/vmc-runs/loop-range-depth-before-20261115-v1`, with SUMMARY SHA
`614412f1b8f0d4e4f20d9df35f1361a286e17e175d84a740c6523239985790fa`.
After evidence:
`/home/ubuntu/vmc-runs/loop-range-depth-after-20261115-v1`, with SUMMARY SHA
`53be23cc6f8dc2d161de02263ef30def0c2e5694d3713e8bab9bbeaecfc16e9e`.
Each has command guards, plans, raw XML, loader traces, symbol evidence and a
complete regular-file manifest. Libraries remain external frozen references.
The before executable/source are referenced by the after plan without copying
or editing them. Copies use the same basenames under Mac
`/Users/c/Desktop/deob/validation-artifacts/`.

The older 96-layer fixture remains a separate failed precision campaign; its
bounded-address variant passed on the old SDK and is compatibility evidence,
not a performance negative. Neither result was reclassified or overwritten.
No after result here is counted as a new C, Unicorn or Guest-native match.
