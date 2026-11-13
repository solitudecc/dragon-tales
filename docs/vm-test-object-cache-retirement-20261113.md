# Owned test object cache retirement

The next bounded practical campaign needs its unchanged 1 GiB start gate.
The VM had about 500 MB free. The proposed old real60 expanded-IR cleanup
was not performed: its 610444292 logical bytes mostly share hardlinks with
retained evidence, so removing 132 names would release only 43536384 bytes.
All those IR files remain.

Instead, exactly 233 rebuildable `.o` leaves were archived and removed from
the owned worktree's `build/tests/CMakeFiles/dragon_tests.dir` and
`build-noz3/tests/CMakeFiles/dragon_tests.dir`. Every selected leaf had one
link and a recorded device/inode/size/allocated-byte/hash/mtime/ctime tuple.
All current owners had closed build-cache dependencies. Two same-UID
process scans checked open descriptors, mappings and working directories;
foreign-UID and the previously recorded security/session-daemon visibility
limits are explicit in the receipts, not represented as full visibility.

Before removal, both hosts verified the entire cache archive: 235 payloads
plus its manifest, every named member, length and SHA256. The VM persisted
the archive before unlinking. Both final `dragon_tests` executables, both
formal runtime directories and their manifests were checked unchanged.
Source, original binaries, validation results and the original dirty
checkout were not selected. This operation executes no test or native target.

- Cache archive: `owned-test-object-cache-20261113-v1.tar.gz`, 212598192 bytes.
- Cache archive SHA256: `72c03dafc8f5bbc9d28458ae558691c3a7de2faafc9d5484ff011ebb36da0d9e`.
- Exact proposal SHA256: `04bf524420b642f1c4032e44b021fc92deaabeef065904239dee39760636246b`.
- Operation evidence: `owned-test-object-cache-retirement-20261113-v1.tar.gz`,
  33283 bytes, ten payloads plus manifest.
- Operation evidence SHA256: `935df3d4de360e19f1b83a8bce3f6fabca12f81aae2258d819ca157863355b7d`.

Both archives are persistent under `~/vmc-runs/` and
`/Users/c/Desktop/deob/validation-artifacts/owned-test-object-cache-20261113-v1/`.
The operation evidence contains the exact scripts, proposal, full before/after
receipts, both verification receipts and per-build restoration notices. The
cache archive retains the actual object bytes and backup source. Both hosts
also verified the complete operation-evidence file set and hashes.

Unlinking releases 1015345152 allocated bytes; after persisting the backup,
available VM space is 1302953984 bytes. This is measured storage recovery,
not new semantic validation. Future Ninja builds regenerate missing objects
from retained source; the two linked test programs are preserved. Exact object
members may also be restored from the archive. No resource gate was lowered.
