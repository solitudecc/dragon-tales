# 按具体入边恢复 Guest 内存值

基线为 `50f698c`。`runGuestMemorySSA()` 在原有精确转发、自然循环检查、延迟改写及
不可约循环本块补扫之后，增加受限的入边特化。它处理这种情况：只有某一前驱能证明
写入状态，LOAD 却位于多个前驱共享的内存检查之后。只在这个前驱上复制短检查路径，
把副本中的 LOAD 替成已证明值，保留原路径服务其它入边。

这不是把不完整内存事实合并成循环不变量，也不全局替换共享 LOAD。原 bounds 条件、
普通读取、失败出口及其全部状态都保留；不会因为 STORE 已执行就绕过后续检查。
Graph、IGNIL、Guest/C ABI、默认 `optimize()` 和内存/镜像输入合同没有变化。

## 使用与报告

能力由已有的 `guest-memory-ssa` Pass 使用，无新公共 API。需要
`enableCrossBlock=true` 与 `enablePhi=true`，仍受 `maxRewrites` 限制。
建议先把寄存器提升到 SSA，并运行已有的表达式整理：

```cpp
lifter.runMem2Reg();
lifter.runInstCombine();
lifter.runGVN();
auto report = lifter.runGuestMemorySSA({65536, false});
```

`runGuestMemorySSA()` 的默认 `runMem2Reg=true` 仍有效；上例 false 只是避免重复提升。
对显式 PassPipeline，`mem2reg → instcombine → gvn → guest-memory-ssa` 可作为一段
可选流水线。没有调整历史默认优化顺序，也没有把验证器的额外 Pass 自动加入产品。

`loadsForwarded` 包含副本中消除的 LOAD；共享原 LOAD 可以同时存在。
`blocksVisited` 包含新复制块，`phisInserted` 包含修复 live-out 所需的 PHI。
复制和 PHI 可能暂时增加 IR 大小，不能只凭指令数判断去混淆收益。
预算不足保留未处理路径，报告 `complete=false / budgetExhausted`；已完成的安全改写
可以保留。完整根函数执行成功和分析预算完整是两件不同的事。

## 接受条件

- 目标是普通整数 LOAD（8 到 128 位且完整字节），地址为同一 SSA Guest base 上的
  直接 `getelementptr i8`、单个 i64 索引、地址空间 0；不借用旧的宿主整数地址折叠或
  不别名猜测。带 GEP no-wrap/inbounds 标志的形状保守拒绝。
- 从 LOAD 块沿唯一前驱回到首个多入边汇合，得到一条无环短路径。路径仅允许普通
  branch、PHI、纯标量指令和 simple LOAD；调用、写入、同步、volatile/atomic、
  EH、alloca、address-taken 块及不能安全修复的类型/元数据使用均拒绝。
- 选择一个普通 branch 的唯一入边，反向找其块内最近的精确 STORE。中间任何其它
  写入、调用或同步都阻止证明；存值须已知非 undef/poison，宽度相同。不同 Guest-base
  LOAD 不是同一个地址空间的证明。
- 优先使用 LLVM PHI 地址翻译。对它不支持的 Sub，只做有界、无 IR 插入的 Add/Sub
  结构比较：沿指定入边替换 PHI，相同操作码及操作数递归匹配，不做全局仿射、MBA 或
  栈帧关系推断。显式/派生 undef、poison、产生 poison 的标志均拒绝。
- freeze 是稳定 SSA 边界，不剥离其内部。两份不同的 freeze 不能当作相同值；新执行的
  region 内 freeze/LOAD 不能沿用旧循环执行的位。来自 region 的旧索引、被选 PHI arm
  或存值也保守拒绝。未选中的未知 PHI arm 不妨碍独立的已知边证明。

只有全部条件成立才复制路径。新出口给已有目标 PHI 补齐 incoming，随后用 SSAUpdater
修复所有外部使用，包括原失败块、普通业务 live-out 和循环 PHI 入边。原 PHI 对象在
修复期间不自动删除。每次成功后重建 DominatorTree；原 LOAD 候选只快照一次，副本
不在同次运行中递归扩张，每个原 LOAD 最多特化一条入边。

## 资源边界

预检在修改 CFG 之前完成。当前私有上限为每路径 6 块、256 指令，每个前驱回扫 256
指令，每个汇合 512 入边，每条路径修复最多 128 live-out、8192 外部使用。模块共享 512 LOAD 候选、
2048 入边尝试、64 次特化、16384 复制指令和 262144 个保守 SSA 修复工作单元。
单地址证明最多 256 节点、64 层深度；模块累计最多 65536 个地址证明节点。
修复工作按 `liveouts × (已有函数块数 + 本次复制块数)` 保守估计，不在复制后才检查。
遇到不支持的形状保持未知，真正触及资源上限则显式报告 incomplete。

## 验证与实际收益

新增的最终同源结构回归在旧库实际失败 **153 条断言**，中间 v1 失败 **50 条**，
最终集成 v2 **16 cases / 873 assertions** 全过。Add/Sub、未知入边、外层 guard、
外部常量 PHI、循环重入、旧执行索引、freeze、屏障及共享预算均有回归。非确定性的
undef/poison/freeze 合同只做结构检查，没有伪造固定原生期望值。

独立原生模型覆盖 544 个输入、O0/O2 的 before/after 共 **2176 次正常比较**，零差异。
比较全部 512 字节寄存器、64 KiB Guest 内存、七个 Exit 字段、返回值和调用次数；
192 个错误 IR 观测精确检出 **88/88** 指定错误，另 **5987** 个比较器控制通过。
四个核心正例和循环例确实改变已知边并转发，未知三边与原 LOAD 都保留。

此前的有限不可约循环原生矩阵也在最终库重跑：**1056 次**返回值及完整 64 KiB 内存
匹配，返回值/内存控制各 **1056 次**检出；无差异、跳过、排除或归一化。
其中新入边特化增加一次合法转发，原 LOAD 仍服务其余路径。对应旧结构回归已加强为
同时检查关闭入边特化时不导入不完整循环事实，以及默认选项下只复制具体已知边。

主/no-Z3 聚焦组均为 **16 cases / 873 assertions**，相关组件组均为
**70 cases / 2393 assertions**，全量 CTest 为 **903/903** 和 **768/768**，全部通过。
验证细节、真实样本流水线与失败中间产物见
[结构回归记录](guest-memory-checked-edges-regressions.md)及
[独立原生与真实 ELF 验证器](guest-memory-checked-edges-validation.md)。

对四个冻结 Tigress ELF，原显式 raw 顺序与 GVN 后额外运行 SSA 的顺序分别做完整
C16/LLVM16 重放，全部匹配。相同后 GVN 顺序下，真实新转发为 f-cff O0 **0→1**、
f-combo O0 **0→4**，两个 O2 为零；八份固定 IR 均验证通过，六份仍报告预算 incomplete。
实际 CFF 和程序原有 switch 均 **1→1**，表 LOAD、目标检查和默认退出保留。
这些是具体状态读取恢复，不能称为完成真实 CFF 去平坦化。

本轮没有改变机器指令 lifter，也没有新增单指令/IGNIL 对拍或重跑 69 输入整语料库。
这里的证据限定于改变的可选 LLVM 变换、完整组件回归和同一四个冻结 ELF，不能作为
任意循环、一般堆关系或整程序语义证明。

集成构建、旧失败和不可约循环复验记录在 VM
`~/vmc-runs/checked-edge-memory-20261111/`，本地摘要目录为
`/Users/c/Desktop/deob/validation-artifacts/checked-edge-memory-20261111/`。
两端 `final-validation-logs.tar.gz` 的 SHA-256 为
`5ada40acc0ad85918dc85af614125a0a25b7ce2879fd73578d32aaa0984f4ff8`。
独立原生/真实重放及结构回归的双端归档路径和哈希分别记在上面的两份验证文档中。
最终 backend 为 `bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003`。
