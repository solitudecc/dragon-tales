# 按入边证明的状态检查特化

基线 `13f0563`，实现里程碑 `42639c6`。本轮扩展显式选择的 `dispatch-specialize`，不改变默认优化顺序、
Graph、机器指令语义或 Guest/C ABI。

## 解决的问题

共享状态 PHI 同时接收已知常量与未知值时，全局常量证明无法折叠条件。例如
`state = phi [50, initial], [unknown, other]` 后的 `state > 50`，只在 initial
入边上能证明为假。直接绕过共享块还会丢失该块计算的标志位、LOAD 和其它 live-out。

现在仅为已证明的入边复制共享条件块，将该次 PHI 选择替换为对应 incoming SSA 值，
保留计算和 simple LOAD，再把副本的条件分支改为已证明的目标。原块继续接收未知路径。
外层 predecessor 条件保持原样；SSAUpdater 修复块外和循环 PHI 的使用，包括失败出口
上的状态和常量 PHI incoming。自循环副本返回原块，不把下一次迭代误接到当前副本。

## 证明与保守边界

局部证明按真实整数位宽使用 APInt，支持整数算术、位操作、合法移位、整数转换、
有符号/无符号比较及已证明条件下的 select 所选 arm。需要实际使用当前块的 PHI
incoming，不能仅因一次执行记录或样本名字而特化。

条件证明拒绝 poison/undef、未知 freeze、越界移位及未证明的 poison flags，包括
nowrap/exact、trunc nuw/nsw、zext nneg、or disjoint 与 icmp samesign。与条件证明
无关的原有 flagged 计算可复制并保留语义，不为了简化分支而删除它们。

块必须由 simple LOAD 和纯标量计算组成。CALL、写入、同步、volatile/atomic、EH、
alloca、address-taken 块和重复 successor 保持保守。所选 incoming 必须在 predecessor
可用，不能直接取自原共享块的旧执行。条件求值一旦进入块外表达式，不再将其依赖中的
旧块 PHI 替换成本次 incoming；与证明无关、已在块外计算完成的 SSA 值可原样保留。

不假定 RBP 与入口 SP 的关系、Guest 表内容不变、分配成功、未知调用无副作用，
也不引入跨函数入口事实。

## 预算与统计

每次调用先快照原始 terminator，新副本不在本次继续特化；每个原条件最多特化一条入边。
公开的 `maxConditions` / `maxExpressionNodes` 继续适用。私有每模块限制为 2048 次
入边证明、65536 个表达式节点、64 个副本、16384 条副本指令和 262144 个 SSA 修复单元；
每块至多 256 条指令、512 条入边、128 个 live-out 和 8192 个外部使用。表达式至多
64 层，整数宽度超过 128 位保持 Unknown。预算预检在 CFG 修改前完成；工作量超限
报告 incomplete/budgetExhausted。

`branchesRewritten` 包含已折叠的副本，原共享条件可能仍在；`uniqueConditions` 包含
对应入边上下文的常量证明。不能把这两个计数直接解释成消除了多少原 dispatcher。

## 验证状态

独立原生夹具在首版生成 IR 时发现了实际崩溃：chosen successor 的 PHI incoming
若来自块外 SSA（例如入口读取的 Guest size），`MapValue` 对未映射的 local 返回 null。
`RF_IgnoreMissingLocals` 不能替代显式 fallback。修复改为在 VMap 中查找 region 定义，
其余 incoming 原样保留。此前 checked-edge GuestMemorySSA 也存在同一用法，已一并
修补并补独立回归；首版崩溃、断言与原始 IR 全部保留，不计为语义通过。

相同最终测试源码对冻结旧库为 **21 cases / 1659 assertions，13 cases / 329 assertions
失败**；修复候选与最终 main 构建为 **21 cases / 2218 assertions 全过**。新路径上的
外部 SSA 回归还实际使首版候选 SIGSEGV；旧内存路径的 entry LOAD 与 Argument 两种
变体均生成 null PHI incoming，单独测试为 **10 assertions / 2 failures**。测试没有在
invalid IR 后继续解引用，保留了 verifier 原始错误。

独立字节模型的 13 个模式、1232 个固定输入分别以 before/after、O0/O2 执行，修复库
**4928/4928** 次完整返回值、调用计数、512 字节寄存器、64 KiB Guest 和七字段 Exit
匹配；八种精度正例各复制一块，五种调用/内存/同步屏障完全不改写，三条未知入边均保留。
故障 IR 的 **312/312** 个指定差异与 **13555** 个单字段/长度控制均检出。

受影响的 checked-edge 兼容矩阵 **2176/2176** 匹配，故障对照检出 **88/88** 个指定
差异。新增外部 Argument/entry LOAD PHI 子集 **128/128** 匹配，故障对照 **32/32**
指定差异检出；旧同子集在产出 IR 后实际 verifier 失败，尚未执行 native。正常比较没有
排除、跳过、归一化或删减字段。详细输入与原始证据见
[独立验证记录](state-guard-independent-validation-20261111.md)。

冻结 f-cff O2 的独立源码探针从 2 次分支改写增至 3 次，新增的是已知状态 50 的
检查入边；mem2reg-only 探针为 0→1，其余六份真实 IR 没有新增特化。
实际 CFF 和原程序的目标 switch 仍在；这不是完整去平坦化或表内容恢复。

最终 main/no-Z3 聚焦均为 **21 / 2218** 全过，完整 CTest 分别为 **937/937** 与
**802/802**，含已有 UBSan targets。两种真实流水线各重新执行 **C16/LLVM16** 完整
根匹配，Graph 不变；实际 `dispatch-specialize` 阶段的 f-cff O2 从旧 0 次改写变为
新 1 次，不能与上述不同阶段的源码探针混计。其它三个整模块仍触及原有 256 条条件预算，
报告 INCOMPLETE；GuestSSA 原顺序两个模块、post-GVN 四个模块仍报告 incomplete。
执行完整与分析完整分别记录，表读取及 default 出口保留。

测量 gate 另经 1 个正例、4 个指定假收益对照检验；它核对实际 branch ICmp 对应的
状态 PHI 及正确 successor，不把其它寄存器恰好等于 50 计作恢复了状态关系。
对已保存真实 IR 的严格 gate/预算复审执行次数为零，不增加真实重放计数。

本轮不改变默认优化、Graph 或机器 lifter；未新增单指令/IGNIL campaign，也未重复
69 输入语料库。没有声称任意 CFG、任意表内容或整个优化器已经得到语义证明。

## 冻结与归档

main 最终构建与独立 native 使用同一冻结库：
`~/vmc-runs/state-guard-root-20261111/candidate-runtime-v2/`。

- backend：`4b20733fb35f70101f549c7e8ca6aa1249d500a6055d408d519d17c4fd675d03`。
- shim：`81e02399abf9bdbb5dee66da782ad940cbadb1568557dac3159a18e399226753`。

main 最后重构建触发过 Ninja 日志恢复；最终完整重编后，两库哈希与冻结 v2 逐字相同，
全部最终聚焦/CTest 通过。原始重复构建日志保留，不将中间构建计作额外测试覆盖。
失败候选 v1 backend 仅 gzip 压缩，原始哈希和压缩文件哈希保留，需要复现时可解压恢复。

组件源码探针归档双端 SHA-256 为
`169456b7830211babef140f778c121dbf52b0473397c93812993270aaeb71fbe`；
单元旧/新失败控制归档为
`ee09b919a0a5da5c6a8b89da34a50a47f95afc7fbe572d4f734db8df07386584`。
本地目录分别为 `/Users/c/Desktop/deob/validation-artifacts/state-guard-impl-20261111/`
与 `/Users/c/Desktop/deob/validation-artifacts/state-guard-unit-tests-20261111/`。
独立对拍完整包为 VM `~/vmc-runs/state-guard-reference-20261111-evidence.tar.gz`，
本地 `/Users/c/Desktop/deob/validation-artifacts/state-guard-reference-20261111/`
下的同名文件；双端 SHA-256 为
`34c23462796874e580f7a2f3046bd9f77cd9e3a9bb1c2dd48e124fcd7aae8c5d`，
989 个内部文件逐项核验通过。根代理另外确认其中 8 份实际 native 源码快照与最终仓库匹配。

最终构建、聚焦、完整 CTest、测量自检和源码/库绑定日志包为 VM
`~/vmc-runs/state-guard-root-20261111/final-validation-logs.tar.gz`，本地
`/Users/c/Desktop/deob/validation-artifacts/state-guard-root-20261111/` 下的同名文件。
双端 SHA-256 为 `76daf19f67b31356f7b267acc8419f2784b5945c828b57dbd921c2b8d35d3829`，
23 个内部证据文件逐项核验通过；16 份最终生产、测试与验证源码在 Mac/VM 哈希一致。
main runtime 的 `final-runtime` 为冻结 v2 目录的符号链接，no-Z3 两库另以 gzip 保存在
同根的 `final-runtime-noz3/`，均有原始与压缩哈希。未复制为额外语义样本。
