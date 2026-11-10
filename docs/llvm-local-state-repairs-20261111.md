# 同函数状态传递与内存覆盖修补

本阶段基线为 `26e1fce`，继续处理同一机器函数中的状态写入、条件分发和 Guest 内存。
没有增加函数名、混淆器特征或镜像表内容假设，也没有启用跨调用入口收窄。

## 实际修改

1. **查找到达状态 LOAD 的 STORE。** 原先要求 STORE 紧邻前驱终结指令；现在可在同块内
   越过保留原位的纯计算、普通 LOAD，以及有明确 readonly/readnone 属性的调用。
   最近的其它写入、未知调用、部分写、volatile/atomic、fence、convergent、returns-twice
   和副作用汇编都会阻止扫描。地址、类型及宽度必须完全匹配，不推测不别名关系。
2. **保留条件选择的两个真实目标。** 对 `state = select cond, A, B`，分别证明两臂目标，
   并证明 `cond` 非 undef/poison 后，可把无条件回分发器改成复用同一个 `cond` 的直连分支。
   已证常量条件只分析实际选中的臂。外层 guard、目标 PHI、分发块 live-out 和未知入边
   继续保留。扫描与表达式证明共享每条边的预算，超限报告 incomplete。
3. **补扫不可约循环中的可达块。** 原调度器可能在两个循环入口互相等待；原有循环证明和
   延迟改写全部完成后，对未访问块分别从空事实开始扫描。支持本块精确 STORE→LOAD 和
   完整字节重建，不跨块导入/导出事实，不构造 PHI，不声称完成一般 SCC 不动点。
4. **拒绝带 poison 风险的常量证明。** 两个分发 Pass 都拒绝把 LLVM 21 的
   `trunc nuw/nsw`、`zext nneg`、`or disjoint` 当作普通整数运算。`freeze` 后的位可能
   是任意稳定值，不能猜成唯一目标；原有冻结布尔条件仍可用于实际条件分支。

详见 [去平坦化合同](llvm-deflattening.md)、[到达 STORE 回归](reaching-store-regressions.md)
和 [不可约循环覆盖](guest-memory-irreducible-coverage.md)。Graph、IGNIL、Guest/C ABI 和
默认流水线没有变化；跨调用兼容计数保持零。

## 旧代码失败控制

这些测试均能在基线生产代码上编译并实际失败，未以缺少新 API 的编译错误代替回归。

| 测试组 | 旧代码实际结果 |
| --- | --- |
| 到达 STORE 的尾部与屏障 | 3 cases / 256 assertions，15 failures |
| 条件状态与预算基础组 | 8 cases / 649 assertions，71 failures |
| 不可约循环内存覆盖 | 5 cases / 105 assertions，22 failures |
| 冻结的带标志 cast | 2 cases / 102 assertions，32 failures |
| 冻结的 disjoint OR | 2 cases / 67 assertions，10 failures；普通 OR 控制通过 |

旧失败使部分 REQUIRE 后续检查无法执行，因此不能直接比较旧/新断言总数。
原始失败输出保存在 VM `~/vmc-runs/llvm-local-state-20261111/baseline-logs/`。

## 最终集成验证

主构建和无 Z3 构建的新增组均为 **20 cases / 1211 assertions**，相关分发、调用屏障和
GuestMemorySSA 组均为 **54 cases / 1502 assertions**，全部通过。主 CTest 为
**887/887**，无 Z3 为 **752/752**。

三个独立验证器都链接同一份冻结集成库，未用单独替换的组件对象代替最终库：

| 原生验证 | 正常结果 | 故障控制 |
| --- | --- | --- |
| 到达 STORE：18 模式、O0/O2 | 72 模块 / 864 次完整状态匹配 | alias 控制检出 4 次、条件控制检出 10 次；867 个比较器控制通过 |
| 条件状态：16 个 fixture、O0/O2 | 1024 次匹配 | 1024 个控制观测中 4/4 指定错误被检出 |
| 有限不可约循环：11 模式 | 528 输入 / 1056 次匹配 | 返回值、内存单字段控制各检出 1056 次 |

到达 STORE 比较返回值、调用次数、全部 512 字节寄存器及 64 KiB Guest 内存；条件状态
比较返回值、两个状态字、两个侧内存字和调用次数；不可约循环比较返回值及全部 64 KiB。
三组正常差异、跳过、排除、归一化均为零。并发、poison 任意值和环境语义由保守结构
回归限定，没有把顺序原生执行称为这些语义的证明。

条件状态组的实际统计从旧 `visited=16 / rewritten=1 / edges=1 / unresolved=16` 变成
新 `16 / 13 / 14 / 3`。两个带外层 guard 的条件选择和未知 CALL 后未重写的状态仍未证明。
不可约循环组为 `blocksVisited=44 / loadsForwarded=4 / byteRebuilds=2 / phisInserted=0`。

## 真实 Tigress 重放及剩余问题

使用此前冻结的 `f-cff`、`f-combo`，各 O0/O2，输入为 0、1、0x80000000、0xffffffff。
最终库完成 **C 16/16、LLVM 16/16** 完整根函数对照；比较 76 个寄存器/标志字段、
1 MiB Guest 内存和七字段退出结果。寄存器和内存故障控制各 16/16 被检出。
Graph 和输入哈希保持不变。这是重复验证同四个 ELF，不计为新的样本覆盖。

四份最终 IR 中实际 Guest LOAD 目标 switch 都是 **2→2**（CFF 分发和程序原有 switch）。
RIP 入口路由 **1→0**，这是已有限制入口策略带来的框架路由清理，不能算作消除了 CFF。
表读取、运行时目标检查和默认退出均保留，没有把 `.rodata` 当成不变性证明。
四组实际 `dispatch-deflatten.edges_rewritten` 都为零。

另在八份冻结真实 IR 上复验，扫描覆盖增加，但 `loadsForwarded/byteRebuilds/phisInserted`
仍全部为零，输出逐字节不变。主要缺口是成功检查把 Guest 访问拆到不同 LLVM 块，以及
动态 SP/RBP 地址、汇合状态与表读取之间缺少足够的路径证明。本次本块补扫没有解决它们。
后续需沿具体成功边建立地址和值关系，保留失败边；不能全局假定 RBP=入口 SP−8，不能
因测试输入都返回就删除未知目标出口。

本轮没有修改机器指令 lifter，也没有重跑完整单指令或 69 输入语料库；验证集中在实际
改变的 LLVM 变换、两种构建和上述冻结 ELF。全量 CTest、IR verifier 和有限原生矩阵
都不是整个优化器或任意程序正确性的证明。

## 下一步候选方案（尚未实现）

只补扫唯一前驱链通常会重复 GVN 已完成的工作。真实 f-cff O0 的状态 LOAD 在四入边
汇合之后；其中一条边可把 `RBP−120` 翻译成该边的 `RSP−128`，对应此前 STORE 50，
另外三条边不能因此获得这个值。下一阶段应建立具体入边上的局部证明：

1. 通过 PHI 翻译得到该边上的地址，仅接受同 SSA Guest base、64 位 byte-GEP、同宽且
   无覆盖写/调用/同步屏障的到达 STORE，失败或预算不足保持原图。
2. 只为这一条边克隆有界的内存检查及访问路径，在成功路径替换已证明 LOAD；保留原
   bounds 条件、失败出口的完整状态和七字段 Exit，不绕过检查。
3. 保留原汇合与 LOAD 服务其它未知入边；逐项修复克隆路径的 live-out/PHI，无法完整
   修复则放弃。不能把边上的证明用于全局 RAUW 或循环不变量。
4. 回归至少覆盖一已知/三未知入边、检查成功与失败、不同指针别名、覆盖写、未知调用、
   循环回入、live-out 值和克隆预算，再对同一冻结 ELF 进行完整状态及错误控制重放。

这是下一阶段的设计方向，尚不计为已具备的去平坦化能力。

## 证据位置

- 总体、全量日志、真实 ELF 重放：VM `~/vmc-runs/llvm-local-state-20261111/`。
- 到达 STORE：VM `~/vmc-runs/reaching-store-final-20261111/`。
- 不可约循环：VM `~/vmc-runs/guest-memory-irreducible-20261111/`，本地同名证据目录在
  `/Users/c/Desktop/deob/validation-artifacts/`。
- 总体本地摘要：`/Users/c/Desktop/deob/validation-artifacts/llvm-local-state-20261111/`。
- 旧失败、最终构建/CTest 和原生报告摘要包：该目录下 `validation-logs.tar.gz`，两端
  SHA-256 为 `70c86a54dad865a37b60b5525165c447d646844e9e8172a565678628ce99e702`。
- 冻结 backend SHA-256：`7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799`。
- 冻结 shim SHA-256：`cb01b3cb7723d69e1902fc89e393c62729c01e3fdc25cfd94aeef5827e11e855`。
