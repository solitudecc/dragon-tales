# 分发条件证明预算的报告修复

基线 `3bc5f2e`，实现 `1f5284a`。`dispatch-specialize` 的全局 branch/switch 常量证明超过 `maxExpressionNodes` 后，原来仅返回未知；部分调用仍报告 `complete=true`、`budgetExhausted=0`。这会把中途停止的证明误报成完成。

修复在每个原条件的查询返回后检查实际节点计数，超限则设置 `complete=false` 并为该查询增加一次预算计数。原条件求值、遍历次序、入边 fallback、后续条件处理及改写规则完全不变，ABI 和默认预算不变。fallback 有自己的独立预算报告，不把两种查询混成一次。真正的未知输入、LOAD 或不支持表达式若没有触限，仍不记作预算耗尽。

`complete=true` 在这里表示有界分析正常完成，不表示所有 switch 都已证明或消除；应同时查看 `unresolved_switches`。流水线 `converged` 表示 IR 无进一步变化，也不是所有证明都完成。

## 回归与独立检查

- 同一旧 API 测试二进制在旧 main/no-Z3 各运行 5 cases / 158 assertions，实际失败 4 cases / 14 assertions；真正 Unknown 控制通过。
- 新两种构建各 5/158 全过；相关分发及流水线组各 99 cases / 5132 assertions。
- 完整 CTest：main 1035/1035，无 Z3 900/900。
- 独立 producer 使用同一可执行程序切换旧/新库，两种构建各有 21 个具名夹具，42 份 before/after IR 逐字节一致；8 个场景的预算状态被纠正，其余非状态字段全部相同。
- 每种构建的审计均检出 63 个状态损坏控制及 21 个有效 IR 返回值损坏控制。IR 控制调用正常审计的同一字节比较入口。旧 `--require-fixed` 真实调用新版期望验证器并失败，不以硬编码退出代替检查。

精度边界包括三节点二元表达式、两节点 freeze、共享 SSA 重复访问、默认 128 节点限制、131 节点深链、未知输入加深右子树，以及失败后仍处理后续可证明条件。闭合无入口 PHI 循环仅检查 IR 分析鲁棒性，没有虚构可执行输入。

本轮是状态报告修复，没有新增 CPU/Unicorn 原生语义执行、真实 ELF 完整重放或全语料库重跑；逐字节相同的输出直接验证本轮不改变这些夹具的 IR。完整测试仍按原套件执行，不将以前的原生比较重复计入本轮。没有新增排除、跳过或归一化。

## 真实样本剩余前提

[逐表项诊断](guest-table-target-diagnosis-20261111.md)检查已归档 IR 和 ELF，确认 f-cff O0 的 16 个不同索引、f-combo O0 的 5 个索引仍读取调用者提供的 Guest 字节。状态 14/34 的地址证明已经完成；剩余内容不能直接取 ELF 文件值。不同合法 Guest 表字节可以选择不同已有目标或 Transfer/Branch 默认出口。

原真实配方实际包含 `mem2reg, instcombine, gvn, guest-memory, dispatch-specialize, control-flow, dce`，没有执行 MBA、predicates 或 dispatch-deflatten。此前该配方中的 `switches_rewritten=0` 只描述已运行的分发特化步骤，不能扩展成所有优化组合的测量结论。

下一组件应生成独立的、有明确入口前提的分析视图：绑定精确入口/区域、初始 Guest base、寄存器字节和内存字节事实，并保存前提来源。入口事实只播种一次；沿实际 CFG 遇到可能覆盖写、未知调用、不同 base 和无法证明的循环就失效。表初值本身不足以证明与动态栈写不交，不能默加 SP 或 noalias 假设。该接口尚未实现，本轮不声称新增表目标解析或去平坦化收益。

## 参考实现方向

后续内存组件优先对照 [angr reaching-definitions](https://github.com/angr/angr/tree/master/angr/analyses/reaching_definitions) 的定义/使用和内存状态组织，以及 [LLVM MemorySSA](https://llvm.org/docs/MemorySSA.html) 的可能覆盖关系和查询失效机制。它们不自动提供 Dragon Guest 入口事实，不能直接照搬镜像读取策略。

MBA 候选生成可对照 [SiMBA](https://github.com/DenuvoSoftwareSolutions/SiMBA) 和 [GAMBA](https://github.com/DenuvoSoftwareSolutions/GAMBA)，表达式简化接口可对照 [Triton](https://github.com/JonathanSalwan/Triton)，规则与配置组织可对照 [D-810](https://github.com/joydo/d810)。这里只列后续算法研究方向，没有引入这些项目的代码或依赖；候选写回仍须通过本项目的位宽、definedness 与等价证明。

## 证据位置

根材料在 VM `~/vmc-runs/dispatch-proof-budget-20261111/`，Mac 副本在 `/Users/c/Desktop/deob/validation-artifacts/dispatch-proof-budget-20261111/`。393 份构建源码逐项绑定，两个独立验证源另行冻结；日志包括旧失败、新成功、完整 CTest、故障控制和每份 IR。

双端 metadata 包核验 630 份 payload，SHA-256 `bb2c6bc80d89ebeda67bc64098ca7d7edd269d8331640fe8aedeaef2895c8e6a`，压缩大小 7379098 字节。Mac 名为 `metadata.tar.gz`，VM 邻接名为 `dispatch-proof-budget-20261111-metadata.tar.gz`。真实表诊断包也完成双端完整文件集合及逐文件哈希核验。

运行库分为四份无损压缩件，以 `RUNTIME-PARTS.json` 绑定原始/压缩哈希；旧库引用上一阶段四份已归档压缩件，见 `BASELINE-RUNTIME-REFERENCES.json`。源码/日志 metadata 包不重复包含运行库。为避免新链接保留多余旧库，仅在重新核验压缩/展开哈希和既有 Mac 验证记录后删除上一阶段两份 runtime-v1 硬链接，实际 build 文件保留，恢复说明和退休记录已保存。

真实表诊断另存 `guest-table-target-diagnosis-3bc5f2e.tar.gz`，SHA-256 `386e0d3767279f0a17dfe77446a48ba0fc2fcbec3bbace7f778dbfd90b53fba8`，含 17 份 payload 和一个清单。它复用旧证据，未新增真实执行计数。
