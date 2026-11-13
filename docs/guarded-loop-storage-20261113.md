# Retired completed K/I2 runtime copies

The current validation and new real replay use the frozen state-values
`a2f82bdd` / `b4fae0a3` runtimes. The completed K/I2 raw libraries were archived
to release space for the real pipeline. No source, current runtime or test
result was deleted.

Exact archive SHA-256:
`5836162fccb0b71d5231a36da4e0fd776077ae6312268a07eb22fad9eed0442e`.
It contains twelve payloads: both main/no-Z3 libraries and original manifests
for `invariant-self-loops-20261112` and `guarded-history-singleton-20261112`.
Each original library was first checked against its frozen runtime manifest.
The source payloads remained unchanged while archiving. Both hosts independently
verified the exact archive member set, every length and every SHA.

Local archive:
`/Users/c/Desktop/deob/validation-artifacts/completed-K-I2-runtimes-20261113/completed-K-I2-runtimes-20261113.tar.gz`.
VM archive:
`~/vmc-runs/completed-K-I2-runtimes-20261113/completed-K-I2-runtimes-20261113.tar.gz`.

After checking no process mapped a selected library, retirement removed only
the eight manifest-listed `.so` payloads, totaling790,896,976 bytes. The four
original manifests, exact retirement plan, dual-host receipts and archive remain.
Restoration notices in each old runtime directory point to the archive. Future
old-version reproductions must restore the exact archived bytes first.
The new full pipeline has no dependency on these old runtime copies.

The separate cffO2 observer restored eight earlier H2 input/output payloads from
archive `f1e830cc` into a new preparation directory, checking each original
length/hash. Original retired H2 paths remained retired. Its initial missing-file
assertion and the later old-instrumenter refusal are preserved in the cffO2
observer evidence; neither is counted as a native observation.
