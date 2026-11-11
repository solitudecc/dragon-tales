# Same-read target sets: real validation (2026-11-12)

This stage adds one explicit `guarded-target-sets` pass to the frozen D recipe.
The four f-cff/f-combo O0/O2 ELF fixtures, sixteen inputs, C/LLVM full-state
comparators and all fault controls remain unchanged. This is not universal
removal of dispatcher code: the original full fallback remains executable.

## Frozen inputs and independent hints

The standalone probes consume the actual D final IR from archive
`005a8827bf936a4f73eea3e7815956761bcffc05f230cb3ab908f1383a7414d7`.
Pre-registration SHA is
`9c85c24263d277102e893292e7f8c3acfcadd6bce7e22e76fde0163d8a8c3644`.
Each independently pinned original ELF contributes six 64-bit little-endian
values from its natural business jump table:

| Module | Table address | Six candidate target addresses |
| --- | --- | --- |
| f-cff O0 | 70040 / 0x11198 | 0x1067f, 0x10689, 0x10693, 0x1069d, 0x106a7, 0x106b1 |
| f-cff O2 | 70104 / 0x111d8 | 0x100b0, 0x1034e, 0x103cd, 0x103b9, 0x103c3, 0x103af |
| f-combo O0 | 90528 / 0x161a0 | 0x13a89, 0x13ac8, 0x13b00, 0x13b4c, 0x13cb0, 0x13e3b |
| f-combo O2 | 74200 / 0x121d8 | 0x107fa, 0x10d59, 0x10d0b, 0x10d30, 0x10cc6, 0x10d7d |

These are **untrusted candidate hints**, not current Guest-memory contents,
immutable-image assertions or extra entry facts. Exact ELF hashes, file offsets,
raw bytes and actual selector PCs are retained in each wire JSON's provenance.
A separate `dragon.indirect-target-hints` node names the guarded view and table
bytes. The original `dragon.guarded-entry` manifest and memory rows remain
byte-identical. Every hinted target is an existing decoded case.

The public-API C++ driver parses the existing module, records the input and
post-metadata IR, runs exactly one pass, verifies the module and records output.
Options are max_candidates=64, max_rewrites=32, max_blocks=128,
max_instructions=16384; max_iterations=max_pass_runs=1. There is no extra cleanup.
The existing single-target guards and normal business-case branches remain.

Runtime: `~/vmc-runs/guarded-target-sets-20261112/runtime-main-v1`.
Backend SHA:
`c8bb7646ab64504576de536009d577b346e38beaffe16cc8e8067e29ff4ed4a1`.
Shim SHA:
`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`.

## Structural probes and retained harness failures

All four normal probes return zero, verify, and add exactly one two-block
six-case fast switch plus a complete same-value fallback. All four report
complete=1, rewritten_edges=1, fast_switches_created=1,
fallback_switches_created=1, original_switches_retained=1 and budget_exhausted=0.
Copied instruction counts are 5/5/5/7 respectively. The fallback's condition is
identically the same actual simple LoadInst SSA value as the fast switch;
no second Guest read is substituted. Every fast case has the same destination
as its case in the original full switch, whose cases and default remain intact.

The initial malformed-hint probe correctly rejected the hint and verified its
unchanged output, but its harness returned 3. The harness compared Function
printing before adding named metadata: inserting that metadata renumbered
existing generated nodes from `!1` to `!2`. The full post-metadata input and
output were already byte-identical. This failed source, result and diagnostics
remain in the archive. The separate V2 negative checks compare the complete
actual post-metadata input module to the output byte for byte. Missing hints
report Unavailable; malformed JSON reports Incomplete with rejected=1. Both V2
negative probes return zero, preserve the entire module and entry manifest,
and perform zero native executions.

The original closure classifier also rejected this later-stage input because
it only accepted a selector LOAD in the switch's own block. After D the useful
constant selectors reside in already proved same-read guard fallbacks. The
final independent classifier accepts these only when the actual guard, LOAD
identity and fallback relationship are verified. It still distinguishes
identical decoded case sets with different default exits. The old failure and
source remain; new controls exercise the cross-block anchor, different-default
classification and rejection of the wrong LOAD identity.

Each real-module review detects four specified corruptions: wrong candidate,
wrong fast destination, missing full-fallback case and different selector
execution identity: **16/16 structural fault controls**. These are not native
semantic executions.

## Complete native replay and resource retry

The formal adapter retains D's original 37 steps and appends the one public
pass: **38 total**. Python's in-memory lifted view remains D; the independently
parsed and verified E module is supplied as the final IR to the unchanged
native compiler/comparator. A saved-IR audit verifies byte equality between
that E output and the actual exported raw module, and between its unique
first-kernel-call binding and the actual exported bound module. Ordinary roots
and recursive-call names are not replaced.

The first formal group was stopped by the fixed 64 MiB disk-safety threshold
at 74.37 seconds, return -15. All source/runtime/input pins remained unchanged;
its four E transformations completed, but the final bounded-memory summary and
outer activation were absent. It is **not counted as a complete optimized
native validation**. Its earlier inherited summary does not substitute for the
missing final stage. The entire failed group is separately archived.

After complete dual-host archival and fresh per-file checks, only its redundant
candidate files were retired. The retry uses the independent `recipe-B-v2`
directory and a stronger 750 MiB startup threshold, with the same running
64 MiB stop threshold. No input, optimization budget, comparator or precision
gate changes. Retry pre-registration SHA:
`08d00aae4b61205369c67a279d43de605e961dd68b92876b8db1923e9c3a032e`.

The completed retry takes **121.78 seconds** and passes:

- C **16/16** and LLVM **16/16** complete-state matches.
- **144/144** executed fault controls: 16 registers, 16 Guest-memory and 112
  seven-field Exit controls.
- All sixteen input files byte-identical; source modules and Graphs unchanged
  for all four cases; runtime, sources, fixtures, contracts, harness, executable
  and hint pins unchanged before/after.
- Zero new exclusions, normalizations or skips.

The final E-export audit independently repeats the same-read/six-target/full
fallback checks and all sixteen structural corruptions. The unchanged earlier
continuation precision audit is run separately. Saved-IR audits add no native
executions and are not combined with the 144 executed controls.

The inherited three-round GuestSSA still reports 12 incomplete calls / 12
budget events; dispatch-specialize reports 12 incomplete calls / 16 events.
The new pass's local complete status does not erase these earlier limits.

## Actual closure boundary

With only verified actual guard failures excluded, and all six ordinary
business cases retained, **f-cff O0 has a cyclic fast SCC and no reachable
original central CFF selector**. The other three modules still fail exactly
the same closure gate. The final actual E export is used, not the earlier
hypothetical six-case graph projection.

This is conditional structural closure. It does not yet establish that the
sixteen executions actually traverse a fast loop, that every guard succeeds,
or that the full fallback can be deleted. A separate final-IR-only observer is
planned to measure entry admission, successful/failing guards, original central
visits and repeated guards in a verified fast SCC. Legal zero-trip inputs remain
semantic checks, not loop witnesses. No observer execution is claimed here.

The three remaining modules include register-PHI constant state inputs and
{0,50} PHI/select state expressions. Their later handling is separate; neither
heap assumptions nor guessed callee effects are introduced by this stage.

## Evidence and recovery

Standalone probes: VM `~/vmc-runs/guarded-target-sets-real-20261112/probes.tar.gz`,
Mac `/Users/c/Desktop/deob/validation-artifacts/guarded-target-sets-real-20261112/probes.tar.gz`.
SHA `8fc79363ce3d53065fa774d7ea5ef582d097178f7b314d05c2b738077e965dde`;
151 files including manifest, 150 payloads, 135255733 payload bytes.

Failed formal group: VM `~/vmc-runs/guarded-target-sets-full-real-20261112/recipe-B-failed.tar.gz`,
matching Mac validation-artifacts namespace. SHA
`ff9a1a55ce430068bc742badc400bc4a0aaaca35a8df95b6c82f13ebc750fef7`;
461 files including manifest, 460 payloads, 439309993 payload bytes.
Its candidate-only retirement has a separate dual-host receipt and exact tar
restore command; the sealed failed archive is unchanged.

Successful formal group: same namespace `recipe-B-v2.tar.gz`, rooted at
`recipe-B-v2`. The adjacent verification receipts bind its final archive SHA.
Each archive's `ARCHIVE-MANIFEST.json` lists every payload length and SHA;
verification checks the exact regular-file set plus the manifest and every
payload on both hosts, not just the compressed archive digest.

This campaign does not add an instruction/IGNIL or 69-program corpus run,
no-Z3 native replay, arbitrary-black-box guarantee, or full four-module loop
closure claim. Component native and whole-suite results are recorded by the
separate production validation campaign.
