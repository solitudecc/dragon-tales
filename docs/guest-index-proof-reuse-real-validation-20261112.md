# Guest 索引证明缓存复用：真实样本验证（2026-11-12）

本轮将同一 STORE 候选内已验证的索引依赖复用，保留 fresh/history 区分、全部 flags/undef/poison 检查及原预算。没有全局事实缓存或表不可变假设。真实验证分为一个固定输入单 pass 对照和一组完整 B 配方执行；两者不可混计。

## 运行绑定

基线 main `40f733e`，旧 backend `d6cfa1bfc4898abcd2ab771a368010c247b78eb98b27f93f9908b42e9ddd3c2f`；新 backend `b6a910b8ca34c2ce1d997cb936dcf68282aec0762cc29ac9a2fff6defc3c7f72`。共用 shim `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`。旧 `34d1` 结果只作先前完整 B 产物对照，不冒充本轮 cache 的旧代码基线。

单 pass 预注册 SHA `a35ab0619953470ce8e0c1c22ac32ca1a451d59ee1bcbb81ec728425c7bec668`；完整 B 预注册 SHA `50ed33f721b8381a3ca8bab70aa1ea19691ed88d45f303419363c7b75e00d2b6`。源码、模型、输入、运行库前后 pins 全部一致。

## 单 pass：实际 state43 由失败变成功

固定 B 的 cff O0/combo O0 最终输入，各运行一次 `guest-memory(run_mem2reg=0,edge_worklist=1)`；保留输入路径、ModuleID、driver 和预算。旧数据是此前实际执行的 d6 两次结果，明确复用；新执行两次，均 verify 成功。此组原生语义执行为零。

| 指标 | cff O0 旧→新 | combo O0 旧→新 |
|---|---:|---:|
| forwards | 0→14 | 0→10 |
| blocksVisited | 1246→1288 | 2101→2124 |
| phisInserted | 0→11 | 1→515 |
| incomplete / budget event | 1/1→1/1 | 1/1→1/1 |

cff `memory.valid227` 保留 STORE43，改接 `bb_67382.guest.edge40 → bb_66234.guest.edge42 → memory.valid7.guest.edge43`。复制路径的原 `%431` 状态 LOAD 被43替代；43传入 `memory.valid9` 和失败出口 `memory.invalid10` 的 PHI。原路径 `%431` LOAD 保留。相同严格 gate 在旧输出失败、新输出通过；两项故意破坏 redirect/liveout 的结构控制均被拒绝。

14 个 cff 原业务边的最近状态 STORE 值为 9、12、23、1、1、37、29、43、42、39、48、2、12、5。combo 的10处 STORE 包含 i32 局部值与 i64 分发状态，不能全称作 dispatcher 状态。逐边证据在 micro `REVIEW.json`。两模块原实际 target LOAD/switch 清单保持，单 pass 不提供表内容证明。

## 完整 B：一次新原生执行矩阵

仍为 SP-only 运行时 guarded context、同四 ELF/16 输入、一次显式 `entry-prefix(max_blocks=128,max_instructions=16384)` 加原3轮×8步骤，原 dispatch cap256、每轮 max_pass_runs32 不变。B 是既有显式大函数配方，不是提高默认32/2048预算。

冻结原脚本 `entry_prefix_continuation_real.py` SHA `d8b45cebf2497fc3d04251fda3185483b0f6df27b56605d8400305fa66cba447` 和原 comparator 未改。只改行政 runner 的输出 namespace、运行库路径。原严格审核 SHA `2db509438ed2ebebfcc15b6bd044b90881641ccb02e2dd072388014988a742b2` 原样通过。

- 新执行 C 16/16、LLVM 完整状态16/16匹配；寄存器故障控制16、Guest控制16、七类 Exit控制112，共144/144检出。
- 四模块 source/Graph 未变，16份输入逐字节一致；无新增跳过、排除或归一化。总耗时110.311秒。
- 原 cff O0 首 selector 删除且 bounds 直接到 `bb_67010` gate 保持；实际 guard LOAD70032、错误出口和原回入 switch 保留。四项首 selector 结构控制、五项 combo bridge 结构控制通过，另列而不计入144个原生控制。
- entry-prefix：cff O0 21块/748指令、O2 22/961；combo O0 110/7892、O2 40/3071，四者均 complete。combo O0 `%83` 原/复制值经 `entry.prefix.live` bridge 的结构 gate 保持。
- 后续 GuestSSA 12次均 incomplete、12 budget events；dispatch 12次均 incomplete、16 events。不能因语义匹配称分析完整。

独立最终 guarded IR 与旧完整 B 存档对照如下。统计只含被实际 switch 使用的 LOAD；新增 switch 是上下文复制，不是去平坦化完成。

| 模块 | 实际 LOAD/switch 旧→新 | 新增常量 LOAD 地址 |
|---|---:|---|
| cff O0 | 8→26 | 69648,69672,69728,69864,69928,69944,69968,69976,70016 |
| cff O2 | 26→26 | 无 |
| combo O0 | 3→7 | 90120 |
| combo O2 | 6→6 | 无 |

旧全部常量地址保留；cff O0 的69976对应 state43。原动态 CFF 和自然业务 switch 都保留：cff O0 `memory.valid13`/`memory.valid261`，cff O2 `memory.valid15`/`memory.valid85`；combo O0 `memory.valid137`/`memory.valid1445`，combo O2 `memory.valid49`/`memory.valid175`。不能将自然业务 switch 混作 CFF，也不能把地址已知等同表内容已知。补充 reviewer 首版错误要求旧 clone 名字全部保持而失败；失败源码和原因保留，最终只记录 clone 名字变化，不修改冻结 oracle 或精度门槛。

## 证据与边界

micro 双端完整包：`/Users/c/Desktop/deob/validation-artifacts/guest-index-cache-real-20261112.tar.gz`（VM 同名位于 `~/vmc-runs/`），SHA `4b0510e3cc9c0af7afe3123ed283c8a1260cd2020ec6efa1bc22e4ac324e1028`，35文件、34 payload共876239字节，每个长度/SHA及精确集合双端核验。

完整 B 在 `~/vmc-runs/guest-index-proof-reuse-real-20261112/recipe-B`；其 archive 与双端核验收据放在同级 `recipe-B.tar.gz` 和验证文件，Mac 对应 `/Users/c/Desktop/deob/validation-artifacts/guest-index-proof-reuse-real-20261112/`。最终包 SHA/集合由归档收据绑定，避免把包自身 SHA 写入包内构成循环。

这次验证没有新增单指令/IGNIL/69样本 corpus；模型约定和完整寄存器/Guest/Exit 比较沿用冻结 oracle。当前收益是重复证明不再抢占有限预算、真实状态 LOAD 转发以及更多实际分发表地址。表 LOAD 仍需执行；未知 alias 写、调用和回入路径不能从入口 ELF 字节推得循环内不变量。下一阶段需保留当前表值变化的 fallback，不能以缓存复用成果宣称循环已经完全去平坦化。
