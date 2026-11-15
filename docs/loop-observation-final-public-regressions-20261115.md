# Final Loop and Depth tests: actual old/new public SDK regression

These executions use the final installed public API tests, not the earlier
source-inclusion cache units and not a Guest-native/C/Unicorn campaign. The
final Loop source is `fe795f35dffdab946a091bbeb21ac1f41e3cfe442d520df159470ccb8bf468f1`;
the final Depth source is `5d12d4070b8a62dda7411bba85e65c8e7e166bef72d57f2d1dffd0b3552997a9`.
The older 806345 b8/b243 result remains separate and was not reused or counted.

One standalone Catch executable was compiled once from those two TestTUs,
Catch2Main/Catch2 and the fixed b348 SDK. No core/helper CPP, helper object or
explicit LLVM library was linked. Both SDK public Image headers are byte-equal,
SHA `1316b8dcd74ed4e1c555cc6b5aa45d198a75d7109391a511d74f1e3c41991417`.
The public Loop/Closed API symbols are undefined imports in the client. Its
original RPATH names b348. The after executions only change explicit library
paths to the fixed 2c9 SDK, with no recompile or relink.

| SDK / tag | Test cases | Assertions | Actual result |
|---|---|---|---|
| b348 / Loop | 4 pass, 2 fail | 208 pass, 2 fail | Gray closure and first-candidate consume fail |
| b348 / Depth | 7 pass, 2 fail | 1197 pass, 2 fail | The same two new precision gates fail |
| 2c9 / Loop | 6 pass | 221 pass | All gates pass |
| 2c9 / Depth | 9 pass | 1210 pass | All gates pass |

There are zero skips or expected-failure exemptions. Every tag ran once on
each backend. The old failure sets are exactly `result.closed` in Gray's
`mode 13` section and `first->use_empty()` in the first-fully-closed-candidate
case. These are actual conservative precision failures through an API that
compiles and links on old b348; they are not old wrong Guest executions or
missing-symbol failures. The Gray REQUIRE stops its later gates on old, so
those unexecuted assertions are not called passing. New executes them all.
The truly clobbered first-candidate no-leak case and all other negative,
rollback, legacy, 24-layer/magic/default-1M and tiny-budget checks remain intact.
The two tags share a fixture prefix; their counts are kept separate and are
not presented as distinct new input domains.

The old backend is
`/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1/lib/libdragon.so`,
SHA `b3483229d516183c0a323319fa4d7b6c407fa1c33f1fae0b7ca6c79560431291`.
The new backend is
`/home/ubuntu/vmc-runs/loop-image-observation-range-sdk-20261115-v1/lib/libdragon.so`,
SHA `2c9ca7a777359f24b0ccf4a5bb88744297f1e0a86dc0376955c02310b20cb7ba`.
All four executions save actual child /proc maps and LD_DEBUG. They show only
the intended backend and original wheel libz3.so.4.13. Runtime/header identities,
all frozen hashes and the physical executable remain unchanged. All commands
retain CPU0–3, 180 seconds, 512MiB startup and 64MiB running floor. The added
read-only map observer changes no test argv, source, caps or expected result;
original compile and zero-probe plans remain preserved.

The evidence root on ROCK is
`/home/ubuntu/vmc-runs/loop-image-observation-final-public-tests-20261115-v1`;
the Mac copy has the same basename under
`/Users/c/Desktop/deob/validation-artifacts/`. Its 51 payloads total 23588500
bytes, excluding FILESET/CLOSURE controls. Complete regular nlink1 member sets,
sizes and SHA hashes were independently checked on both hosts. SUMMARY SHA is
`3f89e6dca5a7863f90a6c68a2447669dabcd7f14cd493cf24061c4c94b8b6c70`;
saved audit is `ccefe0fdf891e29e7e975f4647bfe95febece8040cc3e9fbb74e997963a7665e`.
The external dual certificate
`loop-image-observation-final-public-tests-20261115-v1-DUAL-VERIFIED.json`
has SHA `df3ceeb8a840d1747d739792c208477845bcf77e4050181aaaa1ac9c02bb8bd0`.
The root is frozen with no more writes or target executions planned.

This is installed-SDK precision/compatibility and structural validation. It
adds no native semantic-match count, C or Unicorn reference claim, complete
real-loop CFF claim, or reclassification of prior failed evidence.
