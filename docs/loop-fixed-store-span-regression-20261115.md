# Fixed literal STORE spans in a loop range query

This regression isolates a range-query accounting problem through the unchanged installed C++ `runLoopImageTableContext` API. It is separate from the earlier ad05 fixture's six still-refused alias/value-cycle targets; neither that source nor its failed before/after evidence was changed, replaced or recounted.

The new fixture stores D=`data & 7`, then performs128 complete i64 STOREs to distinct fixed Guest offsets16384+8*n, values500+n, alignment1, before the index.a LOAD. All these literal spans are disjoint from protected table T8192, seeded cell C4096 and index D2048. B's work address uses the explicit original `index.b & 7`, so testing the literal-span path does not require the unresolved circular alias/range proof from ad05. All memory operations and their real observations remain in the source IR.

The frozen old SDK c535 failed the one positive at `result.closed`, reporting `loop local range expression cap: value=4008 point=a depth=1 nodes=131`. The same compiled physical client, with only LD_LIBRARY_PATH changed to fixed SDK0da, passed. This is a precision/budget-accounting regression; it is not a demonstrated old wrong-result execution or missing API. Structural literal disjointness avoids repeatedly asking the scalar expression walker to re-prove a constant address. It does not increase scalar128nodes/32depth, shared1Mwork, Opaque admission or any default pipeline budget.

| Configuration | Focused result |
| --- | --- |
| Fixed old main c535 |2cases,3633assertions;1case/1assertion failed,4negative modes passed |
| Fixed new main0da |2cases,4058assertions,all passed |

The tests check all128 literal STORE address/value/width/alignment mappings, equal original Guest STORE count, nondecreasing Guest LOAD count, retained actual data/index.a/index.b/pointer/pointer.b/t0/t1 LOADs and at least3 actual target LOADs. Ordinary fallback, named metadata identity, Module verification and absence of injected assume/unreachable/private proof tags remain gates. Four independent negative modes add a partial D write, table write, u64-wrapping8-byte span or unknown call. Every refusal must preserve the complete IR text, Function/Arguments/all BasicBlock/all Instruction identities, named metadata object and all original memory effects. No assertion, expected value, input, exclusion or budget was relaxed.

Test source: `tests/lifters/TestLLVMLoopLiteralStoreSpans.cpp`,12359bytes, SHA256 `d87ef05e66a506b2d3e4ae692080a5a696d1d3352298e681976123ce45912074`; tag `[image-loop-literal-span]`. The ordinary test client contains an undefined reference to the public API and no private implementation object/TEST macro/shared LLVM. Old/new actual loader records bind only the intended fixed libdragon and exact Z3 `.so.4.13`; source/object/executable bytes are unchanged between executions. Physical client SHA256 is `685baf9da4dbf5fb605ec7b921b9a2925abec2a78258b68e195fa767121eaedb`.

Fixed old SDK: `/home/ubuntu/vmc-runs/loop-image-memory-graph-sdk-20261115-v1/lib/libdragon.so`, SHA256 `c5354653955f0a19147d73b5c598b6ca61885813b764f91bae2d76118ff38ea9`. Fixed new SDK: `/home/ubuntu/vmc-runs/loop-image-fixed-store-span-sdk-20261115-v1/lib/libdragon.so`, SHA256 `0da0ca6f503a0bf4c2e8c8ef69d072484458aaf084cb2b3c45f3c997f7b77a65`.

Complete old/new evidence is frozen under `/home/ubuntu/vmc-runs/loop-fixed-store-span-{before,after}-20261115-v1` and the corresponding Mac `validation-artifacts/.../ROCK` directories. Both hosts verify every member size/SHA, including source,header,client/object,compiler/linker commands,loader diagnostics,JUnit and all failures. Before contains26payloads/24,852,265bytes; after contains8payloads/28,066bytes and binds the exact earlier client/fileset rather than a relinked copy.

Before SUMMARY `4493bc00a14ba3c22e18fb61cee14cbc7d02a35d136933f0539ed8d1fd01080e`, FILESET `84a72b298b47ba8a45b6d3c8635f000087de65e0f70580c0d94e6393043208c0`. After SUMMARY `f047cb1bd37a23f8c08957e315455ccfb5771ee82b015324d9f529fcf1b6e9b8`, FILESET `5a64cbd56554a3b1a2092ec1eb3d9e6167436dcea05946043aa4b835d8a04e9a`.

This campaign ran focused main-SDK structural/precision tests only, with CPU0-3,180s process-group limits and512MiB/64MiB disk gates. It did not run an independent native oracle, Unicorn, No-Z3 or the full CTest suite and makes no complete de-flattening claim. The earlier real raw32+96 calibration is separate and is not recounted here.
