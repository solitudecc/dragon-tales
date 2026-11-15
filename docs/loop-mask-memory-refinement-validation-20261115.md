# Same-client validation of demand-driven mask memory ranges

The final unchanged test TU executed against fixed old SDK2c9 and, with only LD_LIBRARY_PATH changed, fixed new SDKa5d. Old:2cases/542assertions,1case/5assertions failed. New:2cases/692assertions,all passed. The five old failures were precision refusals at `result.closed`, not a missing API, invalid fixture or demonstrated wrong-result execution. The already-supported low-ones&7 control and all10 negative modes passed on both SDKs.

The new implementation keeps the current coarse constant-mask bound unless a complete STORE span remains ambiguous. Only a known nonzero mask with bit0 clear can ask for a stronger memory range; the same root node/depth/definition-point/Work budgets remain spent. Constant/singleton AND now uses upper=min(a.high,b.high). Independent anchors remain coarse and cannot borrow provisional ranges. Unknown calls/writes, partial coverage and hazard checks remain barriers. See `loop-mask-memory-refinement-design-20261115.md` for the preparation-time design; its uncompiled/unexecuted status describes that earlier snapshot, not this later focused execution.

## Fixture and retained gates

The independently built canonical raw fixture uses E1024 initialized22, then overwritten in A by ordinary_data&15. Thus E cannot bypass the test as a constant cell hypothesis. B's address has a shifted E observation beneath a low-discarding mask. The mask is a xor of the independently checked C LOAD and a literal, becoming known only inside the C hypothesis; upstream normalization cannot assume unverified Guest contents and delete the raw mask first.

Six positive modes cover0xfffffffc,0xfffffffffffffffc,0xfff0 with shift4, actual i32 load/shift/mask, an inner&15 with NO E definition, and the old&7 with NO E definition. Modes0–4 are the five observed old precision refusals; mode5 is the old-success control. The inner-bound case checks min without forcing an unnecessary E definition. The64-bit coarse wrap case requires the unified deferred-D span refinement as well as the outer table/C check.

Ten negatives cover an E domain0..63 that includes a C-clobbering address, partial E write, missing entry definition, unknown call, unmasked E value cycle, overshift, volatile/atomic E load, visible poison and fence. All require complete IR text equality and original Function/Argument/BasicBlock/Instruction identities after refusal. Positive gates retain the actual E LOAD and its live uses/type, actual C LOAD, Guest STORE count, at least3 actual target LOADs, ordinary fallback, caller metadata and valid Module, with no injected assume/unreachable/provenance tags.

An independent clone of each positive raw module runs only upstream LLVM mem2reg and InstCombine. It verifies that the actual E observation survives and that the original raw module is unchanged. Neither a raw LOAD count nor a rewritten expected program supplies the oracle. All16 source modules parsed and verified; none of the old failures was a malformed fixture. No input, assertion, state, threshold, exclusion or budget changed between old and new.

## Exact execution binding

The source is `tests/lifters/TestLLVMLoopMaskMemoryRefinement.cpp`,14919bytes, SHA256 `1d2c6bb0b30fba028a116855f9686ef2307f08774c96756204024c05185649da`. One O0 C++ client was compiled with GCC14/LLVM21 headers and Catch2v3.5.2. It imports the unchanged public `detail::runLoopImageTableContext` symbol from the actual SDK. No private core.o, TEST macro or shared LLVM was linked. SDK ELF exports the implementation; the client has only its undefined API reference.

Physical client SHA256: `702459b0c5aba0cc7aa3df4ed3a1b7a91fcca1f4f0801a26df186deb8ab95973`. Object SHA256: `fffc84d872302cea252531e125a33715b71169c07684f4b5b29f496be8a545d4`.

Old fixed SDK `/home/ubuntu/vmc-runs/loop-image-observation-range-sdk-20261115-v1/lib/libdragon.so` SHA256 `2c9ca7a777359f24b0ccf4a5bb88744297f1e0a86dc0376955c02310b20cb7ba`. New fixed SDK `/home/ubuntu/vmc-runs/loop-image-mask-sdk-20261115-v1/lib/libdragon.so` SHA256 `a5d90833dbeb137c74e783e63e28c5efdf7898e7a37825fd685000079b800a6c`. New SDK-FROZEN manifest `8812955dc73af7b24ab9fb2da2d7796cf4b41cfb303b02ff29f3550b4ecbd8e7` binds source/provenance. Actual LD_DEBUG link/init records and controller pre/post hashes show the respective single fixed SDK and exact Z3 `.so.4.13` SHA4da2bb31, without a shared LLVM or active-build DLL substitution. This is loader evidence; no /proc snapshot is claimed for these short C++ runs.

All commands used CPU0–3 with180s process-group guards,512MiB startup/64MiB running free-space floors. Compilation31.905s and link3.213s returned0. Before focus returned5; after focus returned0. Each guarded focus completed in0.404s. No failure was retried, excluded or normalized.

## Complete receipts and source handoff

Before root `/home/ubuntu/vmc-runs/loop-mask-memory-before-20261115-v1`, Mac mirror `/Users/c/Desktop/deob/validation-artifacts/loop-mask-memory-before-20261115-v1/ROCK`:27payloads/26,147,936bytes. After root/mirror use `loop-mask-memory-after-20261115-v1`:9payloads/31,628bytes and reference the exact earlier physical client. Both hosts passed exact member-set, size and SHA checks. Roots are frozen/no_more_writes; full source/header/object/client/compile/link/loader/JUnit/failure evidence is retained. SDK/tool dependencies are explicit external pins rather than duplicate runtime copies.

| Receipt | SHA256 |
| --- | --- |
| Before presealed PLAN.json | `56314066d3492bff616fdb48998dbcffea7b9d734519c893cf913607e89ac8b5` |
| Before controller.py | `f149b55e7631cee48e8c39ea7699890c2c05d8e56ee5f9d9ccfa1d527864a19b` |
| Before SUMMARY.json | `962afbcafc0727eb74b63faa1c58632754ca85afafd1212546934cf95480bd85` |
| Before FILESET.json | `9f51246e9fc89cfc6497be1e67d7112c572ae7bdf204836978b351b12c159e22` |
| Before dual-host certificate | `79a9e8c500d98c0eb82d106cb7e0838b88783a11a4c544a2a2fada1256d36116` |
| After controller.py | `c99c7b13f16a4a3f9ad7cc5058aaa7727ed0274f2aeaff7625401709f42383c6` |
| After observed EXECUTION-MANIFEST.json | `42a94f42c5e25ec885658146efa3ca4035b2dcf34b94de82e310bd406c5c7e59` |
| After SUMMARY.json | `49c5969807547238d843ed7f906587fa0ef650e30c4b8d1c32b3f59bd22445f9` |
| After FILESET.json | `56412deeaf5df94be1f5be69211e8f392c4657cfca1f32828feac4ff6e617e3a` |
| After dual-host certificate | `66ca477db8fea633b7e27eac6741333935383d39e1b223fefce852f8b8d0adcd` |

The after execution manifest is explicitly a post-run observed record, not a claimed presealed plan; its exact SDK/source/client pins were checked by the frozen controller before execution. The before plan was sealed before any target. The same physical client and all assertions were retained.

The reviewed implementation candidate and actual SDK source match: `lib/LLVMImageTableContext.cpp` SHA256 `8292a2018bf77c83630b509d38f3eac0fb4e255dd624f85daccefba31a879e34`; registered `tests/CMakeLists.txt` SHA256 `70ad3edb44c3ebf4c044b248fffc71337f07277b552802dcfdc42103d92c559e`; test source1d2c above. v3 fragment manifest is `b7eab370dddbcb19bd75b9fd4982eef2a9373b0eeaada47c845e0c03e588bc8b`, source patch `ca815c06a589cee78007f1bd0c8886eec969f6e732c9f169f28b5127cd987c43`, and design snapshot `2e7373fb801f811c9c527564d002b233214ea50f10abfeed3d2cd52de6698104`. v2 remains frozen; v3 adds only the original budgetExhausted early-return gate so inner exhaustion diagnostics are preserved.

This ledger is independent of the other formally built client's2/692 result; those assertions are not added again. No Guest-native oracle, Unicorn, new C reference, No-Z3 run or full CTest suite ran in this campaign. Prior raw32+96, ad05 and d87ef counts are not recounted. These are main-SDK focused IR/precision/effect tests, not an all-input semantic equivalence proof or complete de-flattening claim. Actual real B was still reported incomplete at ordinary scalar depth33 at handoff; no new real after-native result is claimed.
