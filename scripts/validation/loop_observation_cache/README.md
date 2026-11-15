# Exact-observation cache transaction replay

These are the source producers used for the dated internal-cache experiment,
not a native Guest execution harness. They compile the complete actual Image
translation unit with narrow access/publication overlays and import LLVM and
scalar helpers from the explicitly pinned ROCK SDK. No test hook is installed
in the production library.

The source is intentionally pinned to Image SHA `2a3f4b0c…`, SDK `b3483229…`
and the reference environment in `prepare_plans.py`. Use a new output directory;
do not write into the frozen evidence roots. Later source revisions require a
separately reviewed plan, not silently changing the expected hashes.

`prepare_overlay.py SOURCE_ROOT NEW_ROOT/frozen-v1` emits the exact source,
headers, access overlay and wrong-rollback control. Preserve this directory's
layout under `NEW_ROOT`, including `source-v2/observation_cache_probe.cpp`, and
run `prepare_plans.py NEW_ROOT`. The generated plans pin all input files,
compiler, library and resource limits. `guard.py` takes `--plan`, `--sha256`,
`--stage compile|probe` and a fresh `--out` directory. Compile must precede the
corresponding probe. The deliberately wrong control must fail its internal
cache assertions; that failure is evidence that the harness detects leakage.

`audit_saved.py INPUT_ROOT NEW_AUDIT_FILE` only checks saved evidence. It does
not launch a compiler, analysis API or Guest program. The ordinary mode omits
the publication fault injection and compares the five common scenarios; it
does not claim to execute the sixth injected-budget scenario.

Exact results, frozen roots and limitations are in
[the validation report](../../../docs/loop-observation-cache-transactions-validation-20261115.md).
