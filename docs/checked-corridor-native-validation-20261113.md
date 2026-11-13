# Checked cyclic corridors: independent native validation

The final v3 domain retains1,536 distinct tuples: eight modes of192 inputs.
Its historical seed really observes an earlier execution's region definition.
That definition includes a same-address LOAD/freeze and every head visit writes
the address; a separate stale-observation fault distinguishes the initial
memory observation from later fresh iterations. Root scalar PHIs, duplicate
seed/terminal slots, finite counts0/1/2/3/7, table writes before/after LOAD,
nonlocal liveouts, outside-call reentry, region-CALL refusal, descriptor bounds
and genuine regs=Guest+32 aliases remain explicit inputs.

Each main/no-Z3 build independently produces the following clang21 O0/O2 totals:

| Observation | Per build |
|---|---:|
| Complete normal states | 6,528 |
| Business returns / checked failures | 4,814 / 1,714 |
| Fault observations / specified detections | 4,224 / 2,292 |
| Comparator byte controls | 71,808 |
| Saved gzip streams / complete frames | 56 / 10,752 |

Normal states include3,072 before,3,072 after and384 wrong-hint observations.
The counts describe repeated observations of the original1,536 tuples, not new
samples per compiler/stage/build. Every frame compares return8, callcount8 and
the entire73,728-byte arena: all registers, full64KiB Guest, descriptor/config,
seven Exit fields and every untouched byte. Source/pointer bytes remain exact.

Eleven actual faulty programs match their own independent sequential models on
every input. All preregistered normal-model differences are caught: duplicate
prefix384, omitted prefix384, frozen state140, stale history384, stale memory
observation384, selector reload96, omitted table STORE80, wrong root PHI172,
wrong liveout86, omitted case45 fallback70 and omitted sidecall112 per build.
The saved checker replays each complete frame, exact stream set, lengths and
EOF, and repeats all comparator controls. No skip, exclusion, normalization or
timeout increase is used; every native and saved-review group stays below the
original180-second cap.

Both builds pass18 public API recipes, including unchanged region CALL,
malformed/low/zero-budget refusal, wrong777 and straight44 predictions, object
and metadata identity, complete same-LOAD original fallbacks, actual root PHI
slots and fresh loop values. Resumption is `[1,0,0]` for one seed slot and
`[1,1,0]` for the duplicate seed; final bytes equal the normal recipe. Normal
loop contexts/backedges are1/1, or2/4 for duplicate slots. All accepted checked
copies remain explicitly Incomplete. Original J/F/H recipes remain separately
recorded. Main/no-Z358 IR products and18 reports are byte-identical, with58
damaged-byte comparison controls and no canonicalization.

The strict fixture-IR gate catches57 deliberate structural faults on actual
main outputs;35 are independently LLVM-valid damaged programs, including wrong
root initial/fresh values and prefix effects. These damaged IR files are never
executed natively. The gate checks this frozen fixture's PHI/fallback contract;
it is not a formal certificate for arbitrary public-module edits. Generic
source-to-clone proofs and real loop quotient metrics are separate work.

Reference calibration is separate from candidate totals. Final v3 before-only
O0/O2 each pass1,536 normal,2,112 faults/1,146 specified differences,16,896 byte
controls and19 streams/3,648 frames. Earlier v2 calibration (without the added
same-address historical observation) remains archived separately and is not
counted as v3 evidence. Initial v1 stopped at C helper compilation because the
token `0xcafe+i` is invalid; v2 adds the separating whitespace only. That failure
has zero native observations. The first structural-control script incorrectly
changed only one duplicate-edge PHI value and failed LLVM verification; v2
mutates both equal incoming values together, preserving all strict gates and
the original failed evidence.

The same public driver compiles and runs against old M953, exits3 for the absent
new builtin and retains a valid unchanged module/identity. This is feature
absence, not an old wrong-result claim. Driver SHA is
`c167ee51e5466663a24ddab6d8573e0e0778af3ff497aa2e21207dc7ede48904`;
model SHA is
`4915eadba6d42341fb49b6490f54d9e0ef445ff241c7503d1375c472967deda3`.
Main backend is
`48b9b76481c93fd6f18f0a7aca6a913e44e72d8becdc1e6a948a25369d4acc85`;
no-Z3 is
`ab748c4165795b0e2cb36b4e5c9fa55b90b60b0efa6101b0dcf6a7dc0d5032eb`.
All actual sources, commands, runtime/loader bindings, prepared IR and saved
bytes remain in the independently verified dual-host native archives.

This validates the opt-in LLVM component on defined inputs. It adds no new
instruction/Unicorn/corpus campaign, native undef experiment or real-sample
whole-loop completion claim. Historical57/58 failures and all old native
profiles remain separate. Fresh real structural/full-pipeline/loop observation
must bind their actual final compiler inputs before claiming completion.
