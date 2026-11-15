# 有界 PHI 常量图与条件 LOAD 锚

私有低级组件 `lib/LLVMPhiConstant.{hpp,cpp}` 不改 IR。默认
`PhiConstantGraph(Function&)` 只接受同一 Function、同一整数类型（最多64位）的
PHI 和同一个字面量 `ConstantInt`。全部实际 predecessor slots 必须完整覆盖，
包括重复边及不可达前驱；每个依赖节点都须沿 PHI 依赖到达真实常量锚。
无锚子环、不同常量、LOAD、Freeze、参数和算术叶均拒绝。

显式条件构造器接受 `const std::map<Value*, ConstantInt*>& H`，只扩展一种叶：
当前 Function 中 simple、integer、无 poison-generating annotations 的 LOAD，
且该 **exact LOAD SSA** 在 H 中有非空、完全同类型的常量值。不会按地址把另一次
fresh LOAD 当作同一观察；H 中的 PHI、Freeze、参数或算术键不会绕过完整图检查。
volatile/atomic/带相关 metadata 的 LOAD 仍拒绝。

条件构造器返回的是“**在 H 下为 C**”，不是独立值事实或内存证书。它不会证明 H，
不会 RAUW、剪 CFG、发布 definedness/noalias，也不会读取 Guest 的真实内容。
消费者必须独立关闭 H 才能发布任何变换。Loop Image 的预期消费者是在只读的 C
hypothesis 下验证原 preserving 集中全部实际写同时保护 table 与 cell；不能只比较
最终内存或在 hypothesis 下先删掉中间 clobber。成功后再替换已证 C LOAD 的 uses，
保留实际 LOAD，并用空 H 的新对象进行 literal-only 复验。这些是消费者义务，
不是本组件的输出。

H 必须比分析对象活得更久并保持不变。Function/CFG/SSA/H 任一改变都须重建对象；
不同 cell candidate 使用不同对象。完整图闭合后才统一缓存全部 PHI→C；普通拒绝只
缓存当前 root，预算拒绝不发布部分缓存。strict constructor 及 `PhiConstantResult`
布局保留；H 指针追加在旧三个字段之后。新 SDK 的直接 class clients 必须用新头重编。
新头的 conditional constructor 链接旧43f `get` 只会得到旧 strict 拒绝，不应被标作
旧执行语义错误；公开 `runLoopImageTableContext` 客户端不实例化本 class。

每 query 最多512 PHI、65536 work，caller 仍须把实际 work 扣入原总账。零预算不能
命中缓存。迭代图遍历没有提升旧 scalar range 的32 depth/128 nodes，也不增加 Root
共享1M限制。work 计量是明确的节点、边、slot、队列、查询与缓存发布操作，不声称
约束所有 C++ 标准库内部 CPU 指令。

原 strict5 cases/208 assertions 已在43f实际 SDK 通过，且该测试正文8550B完整保留。
追加3 cases 检查条件 LOAD 互环、strict refusal、未列 fresh SSA、非法/缺失 H、
volatile/atomic/metadata、H 生命周期隔离与预算不泄漏。main conditional SDK
`b8a1814a…` 已通过8 cases/303 assertions，相关 Loop5/204、PhiLow9/281、Path3/72、
旧 Image14/2273 全过。使用同一新 Graph TestTU.o、Catch 与冻结43f SDK 的 before
单客户端，旧 strict5/208仍过；新 conditional3 cases 中恰好两个阳性 REQUIRE 失败、
一个负例 case 通过（exit2、JUnit无error），actual LD/import 证明不是 ABI 崩溃。
绑定记录为 `loop-image-conditional-phi-20261115-v1/BEFORE-GRAPH-BINDING.json`，
SHA `549f27ebe373c674cdf272c2fa780b885316a6ff34dcde1168a9b751076d8817`。

真实 canonical raw 的 A/B 各一次 actual API 仍拒绝、整 IR byte-exact。B 已越过原
RSI PHI cap，但在新点 `memory.valid57.dispatch.edge308` 的 `%dispatch.old287`
达到 depth33（nodes15、work930761）；这不是成功闭合或 native 收益。
RESULT SHA `a2ff6be48f6ed36b9b83edc39242c94ebdc9279840a27e820e7b9bcf1f4ef378`。
No-Z3/full/native 尚未执行，不能借用先前组件的次数。
