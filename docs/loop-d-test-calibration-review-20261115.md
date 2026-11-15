# Review of D-range precision-test calibration

This review changed no production code, fixture, expected result or budget and
executed no SDK, C, Unicorn or Guest-native test. It distinguishes a frontend
normalization control from the newly supported D observation-range proof.
Earlier failed tests and SDK runs remain unchanged.

The reviewed memory-definition test is
`/tmp/dragon-loop-memory-definition-tests/tests/lifters/TestLLVMLoopMemoryDefinitions.cpp`,
SHA-256 `955ae102a10bf815364d8be5ff37fad4baebcb974057010d60b588589f5021dd`.
Its raw fixture builder, fifteen negative modes and tiny-budget case retain
their previous definitions. The replacement for the uncalibrated raw
`indexReads >= 2` assertion is an independent CloneModule of that exact raw
input, run through upstream PromotePass and InstCombine only. It invokes no
Dragon proof, adds no guard fact and performs no CFG pass. It verifies the
control and checks that the original module's printed bytes did not change.

The control records every surviving named index LOAD and requires index.b to
remain. Production output must retain every LOAD name in this complete
control set. Thus a raw same-block STORE followed by index.a can legitimately
be forwarded by the already existing frontend; this does not permit dropping
the historical B observation. The two-entry union variants still require both
when both survive the independent control. Target and pointer LOAD checks,
fallback, caller metadata, Function/Argument identity, typed fresh read,
terminal effects, fifteen refusal modes and all tiny-budget gates remain.
This calibrates the phase at which the LOAD-retention contract starts; it is
not a hardcoded change from two reads to one, and is not itself a semantic
equivalence oracle.

The test owner reported the same physical final-source client on old c535
failing all six positive closures (3 cases / 696 assertions executed), then on
new 2c9 passing 3 cases / 844 assertions, with zero skips. These are the owner's
measured counts, not new executions by this reviewer. The old ad05 source and
its first new-SDK assertion failures remain separate evidence.

The reviewed final local Loop and copied Depth sources were respectively
`fe795f35dffdab946a091bbeb21ac1f41e3cfe442d520df159470ccb8bf468f1` and
`5d12d4070b8a62dda7411bba85e65c8e7e166bef72d57f2d1dffd0b3552997a9`.
These include later constantValues gates and must not be confused with earlier
intermediate test hashes. A mechanical comparison of the Depth source against
the frozen 806345 version confirmed that the 24-layer chain, its magic,
existing default 1M budget and other refusal programs were unchanged.

GrayDefinition is retained as a positive under the complete positive contract.
Its ordinary path enters B after A's complete bounded index STORE and then
returns to B. The previous conservative refusal was the unresolved dependency
between that historical D range and B's dynamic work STORE address. New
observation-range induction is precisely the capability required to prove
those original writes disjoint. Moving this program from the negative list
into all positive read/error/fallback/identity gates is an explicit capability
change; it does not erase the old refusal or demonstrate native equivalence.

TwoCandidates independently seeds the first cell at 1024 with 500. Once the D
proof can close the whole write set without assuming the later C cell, the
first candidate can legitimately win. Its original first.read LOAD must stay,
but its uses may become empty only after that candidate's complete proof. A
test requiring that the later candidate always be consumed would assert the
old proof's scheduling limitation rather than the soundness contract.

The separate FailedFirstCandidate keeps the first seed/read but adds an actual
STORE of 501 at `1024 + 512 * (bounded & 1)`. The two possible 8-byte writes
are [1024,1032) and [1536,1544). The first truly destroys the proposed 500 at
the first cell; both stay disjoint from C at 4096 and the table at 8192.
Therefore a later independent candidate can still close the overall proof,
while first.read must remain with real uses. This is a genuine no-leaked-
constant control, not deletion of the earlier failed-candidate purpose.
Caller metadata and verification gates remain. These structural checks do
not replace the separate internal-cache transaction tests or a native oracle.

The original 806345 source and its b8-before / b243-after execution remain
frozen with their own historical expectations and counts. They are not
rewritten to claim that the later D capability already existed in b243.
