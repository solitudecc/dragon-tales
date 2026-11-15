# Closed Loop constants and structural inventories

This change concerns the opt-in Loop API. Old Image APIs, their folding
implementation and default pipelines are unchanged. All work still shares the
original1M ceiling. No CFG path, memory effect or proof obligation is removed.

The real c6 build (Image17badf) completes the joint original-P table/cell write
proof at work951979. Its subsequent global fixed-point constant fold starts at
951980 and exhausts the limit before the fresh empty-H recheck. This is an
executed phase diagnosis, not an inference from the final preserved-store count.
The owned function at that failed point contains733 blocks and12021 remaining
instructions; no pre-fold instruction count or fold-round total was recorded.

The replacement freezes a consumption plan in original instruction order while
the existing shadow worklist's ordinals are available. The plan contains only
the selected cell's immutable H_C facts and its checked ConstantInt pure
derivations. It does not contain D observations, path ranges, opaque Full sets,
arbitrary memory facts or guessed constants. Publishing the plan is still
hypothetical; uses change only after every original P write passes the joint
proof. Each actual use is prepaid for enumeration and RAUW. Every original
instruction, LOAD, STORE and branch remains. The original cell-LOAD counter
continues to count those LOADs, separately from pure derived facts.

The subsequent fresh Path construction and completely new empty-H LoopRanges
instance remain mandatory. A failed or exhausted consumption/recheck discards
the entire owned trial, with no partial public mutation. Pure cleanup is left
for a later documented optimization pipeline; consuming a proof does not need
another global fixed-point search for the same constants.

The430b/407 build passes all12 focused groups but still refuses real B. Its
exact private-stage replay establishes:

| Stage | Work |
| --- | ---: |
| Recover inventory:16814 instructions and158 private markers | 788238 |
| H_C plan ready:1803 entries | 931247 |
| Joint write proof complete | 957389 |
| Constant consumption complete | 964401 |
| Fresh path facts complete | 985231 |
| Fresh write recheck fails | 1000000 |

The first actual failure occurs during fresh write rechecking, at the original
byte STORE in `memory.valid145`. Metadata cleanup, final whole-module
verification and publication have not executed. This build has no published
after IR and no new Guest-native validation. A remains a conservative refusal.

After recovery, low-PHI projection changes only uses; shadow/path/range proofs
are read-only, and constant consumption changes only uses. Thus the already
paid instruction count and private-marker identities remain exact. Markers use
non-tracking WeakVH handles, with original metadata-identity checks before
clearing. The same count still pays the final verification fee and enforces the
same instruction limit; the actual whole-module verifier still runs. Any
future insertion, erasure, cloning, CFG or effect transformation in this region
must invalidate and rebuild these inventories.

A further candidate records all CallBase, LOAD, STORE, Fence and other
mayWriteToMemory instructions during that same complete recovery scan. It
stores physical identities, parent blocks, opcodes, result types and STORE or
call types, never numerical or alias facts. Original order and every record
are retained. Checking these identities and then invoking the same effect
classifier can replace repeated walks over thousands of ignored pure
instructions in joint/fresh write checking and STORE-address enumeration.
Every original effect still receives the original barrier/span/range proof;
fresh caches, all address dependencies, low-PHI proofs and whole verification
remain. This is a scheduling change, not an increased budget. Its actual
execution and complete validation must be reported separately.
