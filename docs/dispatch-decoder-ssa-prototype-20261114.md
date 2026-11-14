# Private scalar dispatch decoder SSA prototype

This prototype is unregistered: no public header, CMake entry, default pipeline
change, merge, full CTest or target-native validation is claimed here.

`detail::normalizeDispatchDecoder` accepts one selected original switch. It
extracts a constant-side integer decoder and distributes it over a complete,
acyclic encoded PHI/select graph. A dynamic arithmetic leaf may observe one
existing i1 or AND-1 SSA value. APInt evaluates the **entire** 0/1 domain and
checks every admitted original overflow, exact, disjoint, nneg and truncation
flag in both executions. Different SSA observations in one leaf refuse.
LOADs, calls, freeze and division are never cloned or evaluated. Original
effects, operations and non-selected liveouts remain. A direct new select
retains the observation dependency even when both decoded constants agree;
the structural poison control is not executed natively.

Node256, PHI128, instruction512 and work16384 caps are hard ceilings, only
lowerable. Decoder/source expression depth is32 with the decoder depth carried
into the source traversal. Every actual PHI predecessor slot is checked,
including duplicate slots. Gray cycles and late unsafe arms refuse before any
mutation. Publication verification failure removes all new instructions.

The separate `normalizeDispatchDecoderUses` entry consumes the same proved
equivalence at every original instruction use of the exact decoder-root SSA
value. It snapshots at most512 uses, charges them to the original work budget,
checks ordinary dominance or the actual PHI incoming edge, then replaces uses.
It never matches a similar expression or crosses distinct LOAD observations.
Failure restores all original uses. Original operations remain. The original
switch-only entry retains its behavior and exact work accounting.

## Executed validation

- v1:7 cases /223 assertions pass. A new private API is unavailable in the old
  product; this is not evidence of an old wrong execution result.
- v2:9 cases /260 assertions pass. The two added cases use the same fixtures
  and assertions against a before adapter that calls only the frozen v1
  switch-only API:2 cases /37 assertions,2 cases /9 assertions fail. This is a
  missing precision/consumer test, not a semantic wrong-result claim.
- An initial v2 Catch2 tag-wildcard invocation selected no tests and exited2.
  Its receipt remains. A separate explicit OR-tag plan selected all9 cases;
  neither fixtures nor assertions changed.
- v2 switch-only real compatibility:all11 canonical and bound-wrapper stage
  pairs,22 files, are byte-identical to the frozen v1 recipe. No normalization.

Actual linked runtime in every run is
`/dev/shm/ordinary-cff-predicate-production-v2/main/libdragon.so`,
SHA256 `32c22c410e96aae4a6bbff544af815d9ce4f4d02f4fd4303cb4ca1a6984e396b`.
No shared LLVM is additionally loaded. Compile/run guards retain512MiB initial,
64MiB running disk/RAMdisk limits and180s timeout, one compile/link at a time.

## Actual frozen manual O2 result: still incomplete

The canonical B raw is SHA256
`0619b5e6d0d22b90a4ef4351929cfa9f1b61b3590c2bd81cfa32118dba7c2eed`;
ELF `f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe`.
The existing guarded context uses the archived kernel SP0xc7f8 and all48 table
bytes. No ELF read-only permission assumption or output-pointer restriction is
added. Complete checked-context closure precedes GuestCellSSA and the decoder.

The selected decoder is the exact SSA root
`%1275 = xor i32 %1274, 1779033703`. Its six original uses are the switch,
`icmp ugt %1275,5`, `sub 4,%1275`, `icmp ult %1275,5`, `add %1275,-5` and the
zext used by the table address. All six are consumed in the explicit v2 mode.
Decoder stats:19 nodes,2 PHIs,6 new scalar instructions,work106,no exhaustion.

The all-use recipe verifies13 stages; adding the two existing default-option
components verifies16 stages. Every checkpoint preserves the exact ordinary
function prints and the original source module. Specialization visits113
conditions and1 switch, rewrites0, leaves1 unresolved, complete=true,budget0.
Provable deflattening visits1 dispatcher, rewrites0 edges, leaves1 unresolved,
complete=true,budget0. Both final graphs still contain1 cyclic central index
switch; both hard structure gates fail. Source/table LOAD recovery and explicit
finite scalar states are gains, **not complete loop de-flattening**.

The switch is in `memory.valid109`; its decoded PHI is in `bb_4765`. Runtime
size reads/checks, the original decoded>5 exit, flag calculations and table
address bounds remain between them. The existing deflattening consumer requires
a single-use state PHI in the dispatcher block itself, so this shape is outside
that admission rule. No budget or guard was relaxed to hide the failure.

Frozen VM evidence roots:

- `~/vmc-runs/dispatch-decoder-ssa-20261114-v1/`, audit SHA256
  `7854de98532486696197ba7aac1c32e4c737030dc8d3d82d90db51f4a71dbef3`.
- `~/vmc-runs/dispatch-decoder-ssa-20261114-v2/`, audit SHA256
  `190f6bd8fc517ab36c1f4d61cfe638694f6db90046fcaddb985d5aa135de67a7`.

No new native, C reference, Unicorn, full CTest, whole-corpus campaign or
semantic completion claim is included in these precision-only probe results.
