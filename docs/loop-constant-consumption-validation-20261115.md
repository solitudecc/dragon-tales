# Loop image：闭合常量消费同客户端回归（2026-11-15）

最终 v3 使用同一 public API `detail::runLoopImageTableContext` 和同一个已编译客户端：旧固定 c6 SDK 在 1M shared work 处失败，新固定 407 SDK 全门通过。阳性覆盖真实检查上下文下的 768 级非线性链，并逐项保留 768 次真实 Guest STORE；阴性和早期预算回滚保持原 Module 的完整身份。本轮没有目标函数原生执行、Unicorn 或完整 CTest，不能据此声称真实大样本完整去平坦化。

## 最终测试输入与独立前端控制

源为 `tests/lifters/TestLLVMLoopConsumePlans.cpp`，17,560 字节，SHA256 `a0e1a2a01bb859fe5501a0c2c5767dcde3c1a2122273467fc1e43c732342c5f5`，tag `[image-loop-consume-plan]`，2 个 TEST_CASE、5 个场景：一个长链阳性、unknown call/poison/shift64 三个阴性、一个 maxWork128 的早期回滚控制。

原输入有独立入口 SP49152、输出快照32768和两项 Guest 表 guard。C4096 是实际初始化 STORE 后的 Guest reload，普通 raw IR 中仍未知。A 从 C reload 出发，依次执行 768 个基本块，每步对 i64 做 xor 非零不同常量、再乘不同奇常量，均无 flags，按无符号模64计算。物理声明顺序与实际 CFG 执行顺序相反。最后输出地址是 `F(C)-F(32768)+32768`，其中 `F(32768)` 由源中 unsigned C++ 数学预先计算，未读取任何产品输出。

每步 mul 后向各自固定地址 `16384+8*n` STORE 一个 i64，align1，避开表、C 和输出32768。这些真实效果给 mul 增加一个 use，使官方 InstCombine 不得把整个纯链下沉到末尾。所有写入均参与原完整写集合证明，不是可忽略的测试注释。

独立控制只在原 Module clone 上运行官方 PromotePass 和 InstCombinePass。它必须 verify，原 Module 文本不变，并检查：

- C 的实际 LOAD 仍有 use；1536 个链节点名称与 opcode 存活。
- 每个 mul 与对应真实 trace STORE 位于其原 `consume.chain.n`，全部 768 次 STORE 的地址、类型、align1 与 mul 来源正确。
- 每个前项支配后项；实际无条件 branch 的 successor 为后项基本块；物理 block ordinal 严格反向。仅保留节点名称不算保留这个测试形状。
- 唯一实际值123的 i32 STORE 的指针可沿 i8 Guest-root GEP 链解释；每个索引必须为 i64，遍历受 depth32/visited guard 约束。常量项模64累加，动态项保留；前端控制中必须仍有动态项，不能由名字猜地址。

生产 after 要保留全部独立控制存活的链指令及 opcode、真实 C LOAD、Guest STORE 总数、Function/Argument 身份、metadata 身份、fallback 与 target LOAD。还要逐项检查所有 768 次 trace STORE 的固定地址、i64 类型、align1 和预计算的每步 `F(32768)` 值。唯一实际 STORE123 的完整 Guest-root 偏移必须没有动态项且等于32768；检查的是实际 effect 的 pointer，不能用一个已经无 use 的旧 GEP 冒充。

三个危险输入要求不闭合、不发布，整个 Module 字节以及 Function/Argument/BasicBlock/Instruction 身份、metadata 和 STORE 数量不变，verify 成功。maxWork128 场景明确是早期预算失败/完整回滚；它不证明消费阶段中途耗尽后的回滚。

## 同源同客户端实际结果

| 固定 SDK | TEST_CASE | 断言 | 失败 | 实际结果 |
|---|---:|---:|---:|---|
| old c6 | 2 | 13991 | 1 case / 1 assertion | 所有前端结构控制通过；阳性实际调用 API 后 `result.closed=false`，消息 shared work exhausted，打印 work1000000；三危险控制与早期回滚通过 |
| new 407 | 2 | 24011 | 0 | 同一组5场景全部通过，完整 effect/指令/地址/拒绝门通过 |

两次 JUnit 都 errors0、skipped0。旧阳性 API 调用1次，危险及早期回滚4次；新版本同样1+4次。old 的公共消息只暴露 shared work 耗尽，不能从该消息独立定位“第一笔失败恰在 fold 内”；内部阶段结论需要另外的诊断。旧结果也不构成旧目标执行语义错误：本轮没有执行该 Guest 函数。

编译仅一次36.516秒、链接一次3.614秒，old focus2.210秒、new focus2.410秒。断言数量不同源于旧阳性在 REQUIRE(closed) 提前停止，不能把 wall time 当消费算法加速测量。新阳性实际断言 `budgetExhausted=false`、work<=1M；成功精确 work 数字未打印。

同一个 physical client 位于 `/home/ubuntu/vmc-runs/loop-consume-plan-before-20261115-v3/tests`，SHA256 `32b51bd8aa50c37479cd7994cd4a13f2fd51d7089f9537a861d72de210eee4f0`，object `04eeda58bc519921be2366b9090e3766e9328547148f3cc51bc51291fecf3cbe`。after 只切换显式 LD_LIBRARY_PATH，没有重新编译、链接、更改 source/filter/reporters/断言/1M预算。

只链接测试 object、Catch2Main/Catch2 与固定 SDK，不带 private implementation object、TEST 宏或共享 LLVM。nm 检查客户端 API 为 UND、旧 SDK 为 T；after 只有指定新 SDK 被 loader 初始化，对应同一现有 API。LD_DEBUG=libs,files 记录实际初始化的 SDK 与固定 Z3.so.4.13，未加载共享 LLVM。本小客户端没有 `/proc/self/maps` 捕获，不能将 loader init 描述成 proc maps。

- old：`/home/ubuntu/vmc-runs/loop-image-paid-opaque-sdk-20261115-v1/lib/libdragon.so`，SHA256 `c6e134bfe6e69b9aff92b00fc81fee03bc3373000b72a5bbaeac4eed98e78e09`。
- new：`/home/ubuntu/vmc-runs/loop-image-consume-plan-sdk-20261115-v1/lib/libdragon.so`，SHA256 `407772ef13146f39984c347ad1360f9e02669e86394fe92d524252338a0433cd`。
- new SDK-FROZEN：`9ae21fadfc57d6eb01f5c7dd61d8c85b3906baf5cea4a5358d0070cbddb15dc6`。权威 entries/sourceCount 为23、total30。邻接 description erratum `3e58527dfecd6fc4696bf2e2b8c1f6fc4bb110b3c07ad6dda12a273b67c86641` 仅把过时 prose22改为23，SDK payload 和 manifest 未变；两者均已 pin。
- 相同实际 solver：bootstrap venv 中 `libz3.so.4.13`，SHA256 `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`。

CPU0–3，单编译任务，每命令180秒、启动512MiB/运行64MiB空间守门不变。没有使用 active build 的可变库。

## 完整保留的前置失败与场景变化

v1（9539源，14005B）的独立控制要求旧名 `consume.address`，官方 InstCombine 合并 sub/add 后该名消失。v1 编译通过，2/164 断言失败1条；阳性 API0、阴性及 tiny4 API通过。v2（5308源，14606B）改查真实 STORE，但 selector 过强要求 GEP 直接以 Guest 为基址；官方控制又在该检查失败，2/165失败1条；阳性仍0 API、其余4 API通过。二者不能计为旧 runtime 预算失败。

独立 print-only 诊断版141c完整输出官方控制 IR，原断言保留且只运行阳性 CASE，实际0 API。实际 IR 显示 STORE 仍是 i32 123，但指针是 `gep(gep(Guest,consume.m.767),-566006595956690816)`；同时全部1536个 xor/mul 虽然名称存活，却被 sink 到一个 consume.tail 块。因此，纯链原场景不再具有逆序扫描性质。完整 IR、stderr 与失败源均保留。

v3 经审查明确新增768个真实 trace STORE，构成新的效果输入场景，并增加实际 GEP 链及支配/CFG/物理布局控制。它不是在 v1/v2 上偷偷更改输入后宣称原输入通过；也未删除那些失败源或降低768、阈值、effect检查来通过。

## 双端完整冻结与覆盖限制

ROCK `/home/ubuntu/vmc-runs/` 和 Mac `/Users/c/Desktop/deob/validation-artifacts/` 下的同名根已逐成员集合、size、SHA256完整核验：

| 根 | payload数 / 字节 | FILESET SHA256 |
|---|---|---|
| `loop-consume-plan-before-20261115-v1` |29 / 26413537|`f9282f51ad6f08112c3cc35be4143099ff2a1360b39fd2b44d3b9fe65180ab8b`|
| `loop-consume-plan-before-20261115-v2` |30 / 26410744|`de1098cbf8dd58674419a0309ec145a89e9d4ca061b846bdf9578266fd740c90`|
| `loop-consume-plan-control-diagnostic-20261115-v1` |31 / 26783238|`7b52f5e08363e5eeffb6fabf44e434e84c71f55f0addff367a285f7334f4cafa`|
| `loop-consume-plan-before-20261115-v3` |30 / 27013557|`13921fe1d5d62b104a625e5396688a5b07117d2a131e9a604b92fe8cd1ad4005`|
| `loop-consume-plan-after-20261115-v3` |10 / 35299|`0fb2088c0dd5937cca2707e3da55e084e8b5f964d5dfab1e8d8de1b21363d7ea`|

共130个 payload、106656375字节，包含全部源/补丁/计划/输入pin/object/client/编译链接输出/loader/JUnit/诊断IR/失败原件。固定 SDK/工具按完整路径和 SHA 引用，没有假称重复包含在每个客户端 payload。五个根 frozen/no_more_writes。证书为 `loop-consume-plan-campaign-20261115-v3/CERTIFICATE.json`，双端 SHA256 `9085a031028855801cbc2ded4733ecee2e153607be9924e53ce9698427ce1de0`。

本轮没有新增 Guest-native、Unicorn、No-Z3或完整CTest；也不重计先前 Shift/Mask/原生语义校准。没有新增排除或归一化。消费中途预算回滚、正式双配置与真实大样本后续阶段仍须独立验证，不以本结构回归代替。
