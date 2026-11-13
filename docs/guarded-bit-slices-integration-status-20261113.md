# Bit-slice milestone integration status

The Bit-specific loop-discovery repair and its independent validation are
committed on `fix/bit-slice-loop-discovery`. They are not merged into `main`.
The current evidence does not establish complete loop de-flattening.

The production changes remain opt-in. Main and no-Z3 pass all 1163 and 1028
CTest cases respectively, with each focused suite passing 98 cases / 8105
assertions. The final independent native campaign passes 23256 normal
full-state observations per backend, with all specified fault detections and
saved frames audited. These are synthetic LLVM function-level checks, not a
new native replay of the real four-ELF recipe. See
[the production repair](guarded-bit-loop-discovery-20261113.md) and
[the separate formal native results](guarded-bit-slices-independent-validation-20261113.md).

The four actual real60 executions are separately committed. Only combo O2
changes, adding two checked straight contexts and retaining complete same-read
fallbacks. Its seven stale frozen candidates are ordinary refusals, not budget
exhaustion. There are no new real target-native or fresh full60 executions.
The actual-pair proof is not an entry-closure proof; the sequential certificate
and its negative controls are separate ongoing validation. See
[the actual real60 results](guarded-bit-slices-real-profile60-results-20261113.md).

The pure shared-inventory output utility is separately committed and verified
against all 28 frozen complete inventories, including all four generic D37
anchors. It changes no production pass, default exporter, old gate or original
IR. See [the exact-output validation](shared-inventory-output-validation-20261113.md).

## Runtime and owned-cache provenance

The production archive containing both final runtimes and build provenance is
`guarded-bit-loop-discovery-production-v2-20261113.tar.gz`, SHA256
`236a49702828735baed2349a06e86d4c020e9a45cf3043e2068f9c6939a5ad8e`.
It has 113 payloads plus its manifest; both hosts verified every member, size
and hash. The VM now has a persistent copy under `~/vmc-runs/`, as well as the
retained tmpfs copy. The Mac copy is under
`/Users/c/Desktop/deob/validation-artifacts/guarded-bit-loop-discovery-20261113/`.
The formal native and actual-real archives bind this production hash without
editing their frozen runtime manifests or recounting their executions.

After all current owners closed their build-cache dependencies, exactly four
duplicate owned build libraries were retired: `lib/libdragon.so` and
`c_shim/libdragon_c.so` in each of `build` and `build-noz3`. Their contents
matched the separately retained formal runtimes. Both `dragon_tests` binaries,
formal runtime directories, source, all original/failure evidence and the
original dirty checkout remain. The exact device/inode/hash proposal,
process checks, before/after receipts and restoration notices are sealed in
`owned-library-cache-retirement-20261113-v1.tar.gz`, SHA256
`f019eca4de7cff5152698838994163316a9f1cbefde86d9adcd8bc693133f047`.
Both hosts verified all six payloads plus the manifest. The archive is in the
same VM and Mac parent directories above. This operation released 400609280
allocated bytes before persisting the production archive, and ran no source,
tests or native target. Future producers use explicit formal runtime paths;
changed production sources require a fresh owned build.

The remaining combo O2 diagnosis uses a separately named recipe/input and
retains the original real60 results. The 512 MiB diagnostic start, 64 MiB
running floor, 180-second command limits and 1 GiB fresh-full start gate remain
unchanged. No successful outcome for that later diagnostic is claimed here.
