# Same-function deflatten native differential check

Run against an already built library on the native build host:

```sh
python3 scripts/validation/same_function_deflat_diff.py \
  --repo /home/cc/dragon-tales-overlap \
  --library /home/cc/dragon-tales-overlap/build/lib/libdragon.so \
  --work /tmp/same-function-deflat-new-run
```

The output directory must not exist. The checker owns only that directory;
it does not configure, build, reset, or modify the repository. The only compile
work is a small IR producer linked against the specified library and four
small native executable products. It records the exact library hash before and
after loading the pass and refuses a library that changes during the run.

The producer calls the actual `runProvableDeflattening` implementation with
cross-call facts disabled. It saves both input and output IR and verifies each
module. These four functions have an independent scalar oracle in the C driver:

| Fixture | Inputs per product | Expected behavior |
| --- | ---: | --- |
| `phi_payload` | 2 | Left returns 10; right returns 20. A dispatcher PHI feeds the returned payload. |
| `load_phi_payload` | 2 | The same payload relation with a load-backed selector. |
| `mixed_state` | 18 | Known edge selects state 1; unknown edge tests nine state values, including signed/unsigned boundaries. |
| `load_mixed_state` | 18 | The same relation through a Guest-like memory slot. |

Before/after modules run at native O0 and O2: 160 normal comparisons. Each
product also flips exactly bit 0 of `phi_payload(left=0)` after execution,
giving another 160 control comparisons and four specified controls. The
checker independently reconstructs all 40 requested input keys, checks printed
expected values and match flags, rejects missing/extra/duplicate records and
incorrect exit codes, and confirms that each control changes exactly its one
specified result. A failed normal comparison always makes the checker fail.
There are no skips, exclusions, or normalizations.

`report.json` retains counts, every difference, pass statistics, compiler logs,
commands, source/IR/input/library hashes and repository source pins. Process
failure, parse failure, incomplete coverage, and library replacement are
infrastructure errors rather than semantic matches.

This checks scalar behavior for the listed synthetic functions. It does not
count volatile/atomic observations, instruction lifting, Guest full-state or
whole-corpus semantics. A pass reporting zero rewrites can establish that a
dangerous transformation was refused; the report keeps rewrite counts so it
cannot be mistaken for successful dispatcher removal.
