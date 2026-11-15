# PathValueRanges branch-condition traversal review and test plan

Read-only source review binds Path header
`02e9783c1b090468f0615acf11488fd59d3d83e0dfe5b9fb4e027d2033eb24eb`,
CPP `c12b670253ce8b23826e3fd7c33fd7d60353415baa300359326c89023c70d50b`,
and the Loop factory
`e39195a44337b3f61231e654d4468e470f0ff6149d814e19378279884c234c81`.
No compiler, helper query, Guest/native or real-sample pass was run by this
reviewer for these versions.

The implementation only inspects BB terminators and explicitly decoded
condition dependencies. The old WholeFunction policy nevertheless charged
`block.size()+1` per BB, which also traverses the instruction list in the
installed LLVM21 representation. That count provided no semantic fact.
BranchConditions uses the lazy conditional expression to spend1 and does
not evaluate block.size(). It changes explicit traversal/charging policy,
not the comparison or range transfer.

The old three-argument constructor still has its old type and mangle and
delegates to WholeFunction. The new mode is an explicit four-argument
overload; no default mode replacement removes the old symbol. Invalid enum
values invalidate the object without facts. The CFG DominatorTree, witness
eligibility, single-edge dominance, single-incoming historical-PHI rule,
comparison/select/Boolean polarity, cache keys, exact query point,
availableWork clamp and every existing witness/BB/fact/node/depth/work cap
remain. The Loop alone explicitly requests the new mode; each call still
constructs a new object and the empty-H_C fresh check does not reuse numeric
caches. No alias, memory content, noundef or stable witness is inferred from
the new traversal setting.

No static soundness blocker was found. The object still requires independently
proved stable defined SSA witnesses. Instruction count is not such a witness.
Different freezes or current/historical values remain distinct even when
their source argument is the same. A guard after a STORE cannot bound the
earlier STORE, and a duplicate/bypass edge cannot become an arm fact. Other
WholeFunction users retain their previous budget behavior. No claim is made
that the real B proof closes after this scheduling change.

The independent new TU is
`tests/lifters/TestLLVMPathRangeTraversal.cpp`, SHA
`b5978b8bffc6168a9c16db5f2c996df22561c93114121cba5b4ecd9219d83fcd`,
14112 bytes,6 cases with tag `[path-branch-conditions]`. It is frozen source
only; there is no actual compile/verify or pass result yet. Its checks cover:

- Legacy3arg equals explicit WholeFunction, and ample-budget mode queries
  return identical facts. The constructor Work difference is exactly the
  independent count of every physical Function instruction, including dead
  blocks; the test does not call block.size() to measure this count.
- A valid function with8192 unrelated flag-free instructions and fixed256
  Work: WholeFunction must really exhaust its policy, BranchConditions must
  produce0..7 and its complementary range while preserving all IR and nodes.
- Missing, foreign and unreachable witnesses, distinct freezes, foreign query
  points, earlier STOREs, duplicated successors and bypass edges.
- A bounded-current loop PHI, separate previous-value PHI, one-edge copies,
  and a fresh next value: only the actual compared observation gains a fact.
- Remaining query Work1, own Work1,257 witnesses, depth34, balanced256-use
  Boolean node exhaustion,4097 BBs and an invalid enum.

The new enum/overload do not exist in an old SDK. An old compile/link failure
is API acceptance evidence, not old wrong execution. The size-budget example
compares the two explicit policies under one unchanged source and budget;
the owner's real Loop before/after same-public-API campaign is the separate
precision-gain gate. The test must retain any preparation or precision
failure, without adjusting counts, budget or expected ranges to pass.

The earlier history-effect cursor proposal is separately deferred in
`fragments/loop-history-effect-index-20261115-v1/`: exact a07 base only, no
candidate/patch/targets. The owner's v12 observed no history stack at first
failure, so that earlier hypothesis must not be restated as an observed cap.
