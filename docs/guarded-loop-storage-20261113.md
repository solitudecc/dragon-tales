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


Two later completed runtime groups were also archived and retired with the
same exact-file, dual-host, original-manifest and process-mapping checks:

| Old runtime group | Archive SHA-256 | Retired libraries / bytes |
|---|---|---|
| `guarded-partial-addresses-20261112` | `67fc2636aa62d8447f52a7aafe1b5fadfe4cb36e917798a9287a51b5046398b6` | 4 /396452056 |
| `guarded-state-values-20261113` (pre-freeze a2/b4) | `cb60c4d4660a7034abf65fada61cb88600dc63c76f41de1abdc9af486134c25b` | 4 /400135192 |

Their six-payload archives, original manifests and retirement receipts are in
`completed-L2-runtimes-20261113` and
`completed-pre-freeze-values-runtimes-20261113` under both artifact roots.
Only their exact listed `.so` paths were removed. Current repaired ed2/7e4
libraries in `guarded-state-definedness-20261113` remain available. Restore the
archived bytes before reproducing either older library version.
