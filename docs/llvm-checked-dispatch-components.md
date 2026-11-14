# Checked dispatch 的三个 C++ 组件

这些是供 C++ custom pass 显式组合的低级组件，接口继续位于
`dragon::llvmlifter::detail`。安装头文件为：

```cpp
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <dragon/lifters/llvm/DispatchDecoderSSA.hpp>
#include <dragon/lifters/llvm/DispatchCorridor.hpp>
```

CMake 把三个实现编入 `libdragon`，现有 include/dragon 目录安装规则同时安装头文件。
`lib/LLVM*.hpp` 只是源代码兼容转发。没有新增 LLVMLifter 成员、PassPipeline builtin、
C shim/Python 字段或默认优化步骤；原默认六步流水线保持不变。
Corridor 的强制提交失败接口仅留在私有头的测试宏下，不是 installed SDK 的声明或正式库
保证导出的 API。正式回归默认编译13个 Corridor case；宏开14例属于独立验证配置。

## 表上下文：先证明闭合，再消费

`runImageTableCheckedContext(module, request)` 接受现有独立 GuardedEntry 的 canonical
raw FunctionStart view。它要求仍能检查原寄存器 allocas、外部入口 switch，以及真实
可执行的 initial x86-64 SP 和完整表字节相等检查。应在普通 LLVM 优化之前调用。
`viewSymbol/tableOffset/targets` 都是候选描述，不是证书；ELF 的只读 section 属性、
manifest 或调用者提供的 targets 不能单独授权替换。

当前支持 little-endian、64位 pointer/index、绝对 u64 表和八字节缩放。候选最多512项。
组件联合证明 accepted 路径到首次 lookup 的 base，以及每个真实表目标到下一 lookup
或退出的 step：原 index bounds 成立、SP 保持所需关系、所有继续路径的写都保持表字节。
未知地址/alias/range、未证明调用或不支持身份拒绝闭合。只读权限不是 Guest 不可变假设。
不再返回 dispatcher 的终止效果仍按原顺序执行。

两参数接口保留 actual index switch 加完整原 target LOAD/switch fallback。
显式三参数 `consumeClosedCFG=true`，或同义 `runClosedImageTableContext`，仅在完整联合
闭合之后发布候选 CFG：actual target LOAD 和原 UnknownTarget 出口保留，已证明不可能的
旧 target-switch 回边可移除。原 bounds/error/flags/业务效果、ordinary root、entry guard
miss 的原执行继续存在。结果属于 **conditional checked analysis view**，不宣称普通根函数
已经无条件净化。

`ImageTableContextRequest` 的默认/硬上限为4096 blocks、65536 instructions、1000000 work；
只能下调。统计提供 `changed/closed/budgetExhausted`、guarded bytes、preserved stores、
constant values、work 和 message。以 `closed && changed` 判断成功消费，不能把单个计数、
`budgetExhausted=false` 或候选元数据当作证明。拒绝在 owned clones 内完成。

成功只发布所选 view 的 body，Module/Function/Argument 对象保持；旧 basic-block、
instruction/CFG 句柄失效。调用者在继续选取 LOAD、switch 或边之前必须重新获取它们。

## 解码 SSA：完整有限域，不猜条件真值

`normalizeDispatchDecoder(function, originalSwitch, options)` 把纯 scalar、常量侧的整数
decoder 分配到完整 PHI/select 图。动态 leaf 可依赖同一个既有 i1 或 AND1 SSA observation；
APInt 穷尽0/1两值，按原位宽检查原运算的 overflow/exact/disjoint/nneg/truncation flags。
不同 SSA observation 不能合并成同一个变量。未知 shape、多观察、灰环、坏 shift、
literal undef/poison、unsafe flags 或预算不足则拒绝，不发布 partial PHI。

新 select 重用原 observation，包括两值相同的情形也保留其 poison dependency。
这不证明 observation 的 definedness。LOAD、call、freeze、division 不被克隆成解码运算，
也不把未知 select 条件证明为真。PHI 必须覆盖每个实际 predecessor slot，包括重复边。
原操作、STORE 和其他 liveout 留存。

默认接口只替换选定 switch 的 condition。另一个显式接口
`normalizeDispatchDecoderUses` 消费**同一 decoder-root SSA value** 的全部原 instruction
uses；不按表达式相似性跨 LOAD 匹配。所有普通 uses/PHI incoming edge 都要满足 dominance，
最多512个原 uses并计入同一 work。失败恢复全部 uses 和新指令。

`DispatchDecoderSSAOptions` 默认/硬上限为256 nodes、128 PHIs、512新 instructions、
16384 work；表达式 depth32，decoder 的深度债务延续到 source 图。只能下调。
`changed/refused/budgetExhausted` 与 node/PHI/instruction/work/message 反映此次调用，
不意味着整个函数已经完成去平坦化。该组件不改 CFG 或 Function/Argument 身份。

## 保序走廊：在原 switch 位置消费状态

`threadDispatchCorridor(function, originalCandidates, options)` 接受调用者在本次首次修改
之前冻结的原 incoming edge 和 CFG path。每项的 source、successor slot、path、PHI、
状态域与全部 side exits 都由核心重新检查。候选提供者不是权威；自动 SCC/PHI/CFG 候选
枚举目前仍只在验证 probe，不因一个样本通过而成为核心事实。

一次调用在单一 owned module trial 中按原 Function block/slot 顺序处理所有原候选，
最后整体发布一次。新副本不是候选。每个 path 最多6块/256原 instructions；中间保留
实际 STORE、GuestSize LOAD、counter/bounds/flags 及所有 fault/finish side exits 的顺序。
不假定 GuestSize 恒定，不提前读取或删除它，也不把未证明的 call/volatile/atomic/fence
当作普通 scalar 运算复制。

历史与 fresh 按**每个原 Use 的执行位置**区分。支配 seed 的旧后段 header 值经
single-input snapshot 进入此次复制；不能用全局 VMap 误接到稍后执行的新 header。
数值证明同样区分 Current/Historical 视图：历史 numeric PHI 保守拒绝，AND1/i1 只枚举
完整有限域而不展开其内部旧数值。SSAUpdater 修复全部跨块 ordinary/PHI uses，外部 exit
PHI 根据新 terminator 的实际 successor slots 建立，不能沿用旧 switch 的槽数。

仅在原 switch 执行位置消费原 selector。真正已定义的单值可直连；二值可用一次原 selector
比较去两个目标，不提前在 atom 处建立分支，不新增 freeze/assume。这样保留 poison 的
原控制消费时点和前面的错误退出。不能因为相等 select arms 就删除未知 poison 依赖。

默认/硬上限为512候选、64改写、64副本块、16384复制 instructions、262144 SSA repair
cells、1000000 work；单次预付同时约束128 liveout和8192外部 uses。只能下调，snapshot/
桥块/复制与 repair 分别计费。`complete=false` 可伴随独立原边的部分成功；changed、
rewritten edges 或副本数不是完整 CFF 验收。应在真实业务可达 SCC 检查原 dispatcher
是否退出循环，不把 switch 改名/降级当作完成。

不支持的 alias/值关系、历史 numeric 依赖、调用和特殊身份保持原路径。非法试验或提交
整体回滚；Module/Function/Arguments 与 ordinary roots保持。成功后所有旧 block/
instruction 句柄过期，下一阶段必须重新获取。

## 组合与验证范围

调用者可依次显式创建 checked view、消费表闭合、用已安装
[GuestCellSSA](llvm-guest-cell-ssa.md) 取得最近状态 STORE 的完整 SSA、分配 scalar decoder，
再使用 LLVM/control-flow passes 和保序走廊。每个 body 发布后重新选择原始候选。
任一拒绝都不能改用 manifest、表 hint 或输入样本来补成“事实”。

原型已在 frozen manual O2 的 guarded 分析视图得到8条原回流边全部处理，核心紧后和最终
actual SCC 的循环分发 switch 均为0。原16输入的 before/after LLVM完整状态复验随后通过；
没有因此重计旧 C/Unicorn references。这是限定已测域的结果，不是任意黑盒混淆保证。
原始失败、事务/历史值缺陷的 controlled 对照和阶段产物均保留。

SDK 接入保留全部公开声明、字段顺序、默认值和原回归正文／断言；Decoder 与 Corridor
核心逐字节保留。独立事务检查另修正 Image 发布：恢复 `CloneFunctionInto` 新建的空
`llvm.dbg.cu`，临时承载保留 calling convention／attributes，并拒绝不能保留的局部
metadata／foreign BlockAddress 身份。表不变式与业务变换逻辑保持原样。

新增同 API 回归对旧私有 Image 核心实跑1例／74断言、8断言失败，证实 named metadata
污染与 fastcc musttail 的临时承载拒绝；不将其称为旧生产库曾错误执行目标。修后私有
宏测试7例／939断言全通过，含两个阶段的受控回滚。正式默认 Image6例、Decoder9例、
Corridor13例，共28例；强制失败 hook 只在私有 TEST 宏下，未加入 installed header。

正式两套库均已通过28用例/1701断言和 SDK client验证，历史完整 CTest1163/1163与
1028/1028通过，并单列补跑未在缓存发现列表中的 predicate/C4/Cell 用例。实际库的
native及真实样本验证结果见[正式接入报告](checked-dispatch-integration-20261114.md)，
与先前 prototype/helper-object 的计数分开。4场景入口/逻辑GuestSize边界组的24份正常
状态比较、8次指定wrong-IR检测及72个比较器控制均通过；完整三层档案已双端逐成员校验。

这些组件不处理通用 VM、自修改/JIT、一般堆分析或任意 external-call 效果，也不生成独立
去混淆二进制。遇到特殊混淆可通过新的 custom pass 在已有可证明边界内扩展。
