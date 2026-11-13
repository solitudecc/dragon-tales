# Ordered raw-bound real60 structural audit

The four frozen real inputs have now been checked in their actual order:
D37 anchor, V-before/V-after, C59-after, exact C59-to-Bit LLVM import, and
Bit-after. All four audits establish their declared structural certificates.
Their conditional entry-closure results are **true / true / true / false**
for cff O0, cff O2, combo O0 and combo O2. Combo O2 still reaches the central
family at `memory.valid49`. These are conditional guard-success results,
not unconditional execution equivalence or complete loop de-flattening.

`checked_context_chain_audit_v1.py` requires a hash-pinned plan with all six
raw LLVM and complete inventory pairs, manifest, hints, reference tools and
formal runtime. It independently parses/verifies/reconstructs every complete
inventory. D37 uses its original five-field schema; the other frames use the
complete six-field state schema. No fields, instructions, comments or names
are dropped or normalized. Source/input/tool/runtime hashes and file stats
are checked before and after each successful certificate.

The original V4 guard/path/PHI checks apply to the actual V-before/V-after
pair. They do not constitute a new general proof of all unrelated V-stage
instructions. Complete ordinary-edge/value copy proofs apply separately to
C59 and Bit60. Prior terminal projectors transfer to the copied full fallback;
new fast switches receive their own actual case slots. The exact pure parser
bridge proves the complete intervening LLVM rendering and full inventory.
Central membership is derived from the unchanged D37 family/cases and Exit
PC evidence. Ordinary business edges and full error/unknown fallbacks remain.

The old byte-identity failure for combo O0's V-stage outside metadata is
retained. Its independently checked replacement certificate permits only a
complete, unambiguous numeric metadata graph bijection, preserving all typed
payloads, strings, `distinct`, references and ordinary outside bytes. It never
edits either raw module. Unsupported or ambiguous definitions are refused.
In particular, an earlier draft parser incorrectly accepted four LLVM-valid
multiline metadata mutations; its exact source and false accepts remain.
The fixed parser rejects all four. Original/main has no sequential-checker
API; this is a validation addition, not a claimed old product execution bug.

## Executed checks

The root-v2 audit uses the frozen old generic anchor tool and an explicitly
selected shared state-inventory reference. Its four CLI times were 27.598,
23.283, 92.110 and 81.107 seconds. All 24 raw/inventory role bindings passed,
all source/spec/tool/runtime pins stayed unchanged, and the result remained
T/T/T/F. The old one-case v1 audit and original inputs remain unmodified.

Root-v3 adds the separately validated shared tool's explicit `base` mode for
complete generic output, retaining the old anchor tool as the original plan
option. Its unchanged actual combo O2 positive establishes the certificate
with closure=false. Seven whole-chain controls all refuse at the intended
stage: reordered stages, omitted C59 frame, altered complete raw bridge,
stale JSON, changed copied GEP element type, removed full-fallback case/PHI
edge, and wrong fresh loop-PHI input. The three private instruction/edge
mutants are independently LLVM-verified with complete new inventories.
Five direct metadata controls and four preserved multiline controls also
refuse. These are separate from the full-chain controls. Each complete-chain
CLI is limited to 180 seconds; the eight-case orchestration took 411.735
seconds. No optimizer or target-native execution occurs in these audits.

Two additional independent whole-chain controls verify that an altered V4
outside metadata payload reaches and fails the V4 certificate, and that
omitting an actual central family member fails final coverage after the copy
proofs and parser bridge pass. They are counted separately from the seven
root controls. See
[their exact source and evidence](checked-context-chain-extra-controls-20261113.md).

## Complete evidence and limits

The combined archive contains 303 payloads plus its manifest, 268032946
logical payload bytes and 25113241 compressed bytes. It retains all source
versions, old failures, all actual audits, private raw mutants/full JSON,
commands, reference tools, independent reviews and pins. Both hosts verify
the exact member set, every size and SHA256.

- VM: `~/vmc-runs/checked-context-sequential-chain-20261113-v1.tar.gz`
- Mac: `/Users/c/Desktop/deob/validation-artifacts/checked-context-sequential-chain-20261113-v1/checked-context-sequential-chain-20261113-v1.tar.gz`
- SHA256: `6517570d3f406c3d4236feec4afd02fa87c4dfa7477e29e1887a42fe833a3ca5`

Untouched original frames are externally bound to their four prior complete
archives: all 48 logical frame references / 26 distinct raw LLVM-or-JSON
hashes were streamed and found in the archived members. The mapping receipt
is included, SHA256
`d1c5f6856dbc9a5413eff21027d0a561852f43e2e0009922cecc16ca1298e0e5`.
The formal runtime remains in the separate production archive `236a4970...`;
shared reference output validation remains in `36dda701...`. Neither is
replaced. No original evidence is retired by this audit.

The remaining combo O2 witness includes an actual unknown Guest stack LOAD
feeding `20-x` and the original dispatcher. The machine disassembly and
lifted operations agree on this path. This is not evidence of a new decoder
bug, proved memory feasibility, heap dependence or an impossible proof.
Target-level memory diagnosis is separate. This audit executes zero product
passes, target-native programs, fresh full60 replays, instruction/Unicorn
tests or corpus campaigns. Existing production/native results are not
re-counted. There is no main merge or all-four completion claim.
