# 去混淆能力补齐：设计交接

日期：2026-11-09。代码基线：`main@4cd41b7b6edc4c92051eb57c7a6d37e230d2334e`。

请基于现有代码和以下证据制定实施方案，先做设计，不修改生产代码。用户会将方案交回开发代理执行。
本文按能力缺口、设计问题和验证工具问题分类；不是全仓库缺陷总表。当前已验证范围内未发现新的
语义差异，但测试通过不代表整个框架已经无 bug，也不代表已经完成去混淆。

## 1. 用户目标与范围

重点是通过静态反汇编、IGNIL/LLVM 提升和可组合 Pass，理解混淆代码原理，简化常见控制流
平坦化、线性 MBA 和可证明的不透明谓词，产出更清晰的分析结果与 LLVM IR。

- 分析和去混淆过程不运行目标程序。Unicorn 与 LLVM 原生执行仅用于独立验证。
- 不要求生成独立的去混淆二进制或修补原可执行文件。
- VM、自修改等特殊保护由后续具体样本的自定义 Pass 处理，不作为本轮通用扩展目标。
- 复用现有 PassPipeline、有限值集、调用效果、MBA 和谓词组件，重点补齐分析与改写之间的缺口。

## 2. 当前已经具备的能力

项目：`solitudecc/dragon-tales`，C++20 / LLVM 21，具有 C shim 和 Python cffi 接口。

| 组件 | 当前状态 |
|---|---|
| 反汇编、CFG 与指令重叠 | 已有递归/线性策略及重叠解码修复；未知语义保留 UNDEF/unlifted 或显式退出 |
| 有限值集和内存分析 | 已有汇合、循环不动点、LOAD 前内存查询、条件入口事实及预算控制 |
| 跨调用内存效果 | 已有有界 x86 调用树、栈相关内存传递、确定循环填充/复制等能力 |
| 线性 MBA | 已有候选合成与完整字长 Z3 等价证明，证明成立才改写 LLVM IR |
| 整数谓词 | 已有局部 br/select 恒定条件证明，含部分乘法参与的恒定谓词 |
| PassPipeline | 已有 Graph/LLVM 两阶段注册、配置、缓存失效、有界重复执行和结构化报告 |
| 控制流清理 | 显式 `control-flow` 步骤组合 SCCP、DFAJumpThreading、JumpThreading、SimplifyCFG、InstCombine、DCE |
| 静态区域提升 | 新增 `liftStaticRegion` / `lift_static_region`，可在私有图中补建已解码块，按实际 target 执行，并保留未知目标出口 |

普通 `lift()` 仍使用原 Function 成员；新区域接口是显式覆盖扩展，不改变源 Graph 的边或函数成员。
`rebuild_decoded_blocks=True` 从机器指令重建，不继承调用者自定义的 IGNIL 改写。
现有默认六步优化顺序为 `mem2reg → instcombine → gvn → mba → predicates → dce`。

## 3. 最新验证事实与边界

- 主构建 CTest **800/800**，无 Z3 **668/668**。
- 静态区域 C++ 组件：两种构建均 **10 项 / 880 条断言**。
- 三架构小机器码：主/无 Z3 各 **240 次匹配**，分别为 96 次完整根返回、48 次区域外 Transfer、
  96 次内部 PC 拒绝；比较全部模型字段、64 KiB 内存及七字段 Exit。
- 小机器码用真实 Guest STORE 覆盖初始表项，再 LOAD/JMP；返回寄存器和内存负对照各检出
  **240/240 次指定单字段差异**。
- 真实 Tigress：两份冻结的 **C O0 ELF**，线性策略，LLVM product O0/O2，八组输入：
  旧库 C16 匹配、LLVM 0 完整 / 32 incomplete；新区域提升 C16 匹配、LLVM **32/32 完整根返回匹配**，
  比较全部模型字段与 **1 MiB 内存**。
- 真实 Guest `megaInit` 在验证中照常执行，没有将初始化后的堆快照注入静态分析。
- 源 Graph 的机器字节、语义操作数、markers、边和函数成员前后保持不变。
- 当前普通 lift 对旧库的 16 份指定 IR、8 份 Graph 快照逐字相同；额外 O2 文件仅 ModuleID
  中的真实输入路径不同，其后字节相同。原文件未归一化。
- 本轮未将区域能力扩展验证到 C O2 二进制、递归策略、新程序/新种子或全部 69 程序语料。
- 真实样本的分发 switch 和回边仍在。**完整返回匹配证明执行覆盖及已测输入的语义，不证明去平坦化完成。**

`f-mba` 的正常原程序 switch 与 `f-combo` 的混淆分发器必须区分。新区域 IR 中的 26/102 个
覆盖 case 是候选机器块数量，不是已证明的真实状态数量或跳转表完整目标集。

## 4. 具体待解决问题

### 4.1 P0：Guest 内存中的状态变量没有转成可优化的 SSA

真实 IR 的主要形状可概括为以下伪代码，并非逐字摘录的可编译 LLVM IR：

```text
state  = LOAD(guest_memory, rbp - slot_offset)
target = LOAD(guest_memory, table_base + state * entry_size)
SWITCH target -> covered_machine_blocks, default Transfer
```

`mem2reg` 可以提升 LLVM alloca，但这里是 Guest 平坦内存中的读写。状态藏在 LOAD/STORE
中，优化器难以识别“这个块结束后一定进入哪个状态”。

需要设计：栈槽识别、读写关联、部分字节覆盖、别名判断、跨块合并与失效规则。不能未经证明
假设输入指针与 Guest 栈不别名，也不能把不同 LOAD 或不同 UNDEF 当成同一个值。

### 4.2 P0：跳转表内容与初始化前提缺少通用证明

表地址和索引范围不等于表项内容证明。Guest STORE、可能别名的写入及调用都可能改变内容。
新区域提升只按真实 target 匹配候选块；默认 Graph 的相应间接边仍可保持 Unknown。

需要设计：如何证明各表项内容、初始化事实从何而来、写表/未知调用如何使事实失效，以及
不能证明时如何保留完整默认行为。

已有 `readKnownMemory` 是调用者对精确 LOAD 位点作出的可信断言，不是框架自动证明器。
普通 `readMemory`、ELF 初始字节和只读 section 标记均不自动构成该断言。

### 4.3 P0：Graph 分析与 LLVM 改写之间缺少通用事实桥接

已有分析可以给出有限值集、内存事实和有界调用效果；已恢复的跳转边也会进入 LLVM。
但值约束、内存关系和调用效果尚无通用通道自动成为 LLVM 可消费的已证明事实。

私有区域重建会切分块并重建 IGNIL，原图的 block/opIndex、Temp 身份与 LLVM 指令不一定
继续对应。原局部图上的结论不能直接约束扩展后的所有路径。

需要设计：事实表示、来源与入口契约、准确的 IGNIL→LLVM 位点映射、传递与复核接口、
CFG/IR 变化后的缓存失效。条件入口事实的 `assumptionsValid` 仅表示结构合法，不表示前提已证明。

### 4.4 P0：真正的去平坦化改写尚未实现

现有 `control-flow` 能处理简单 SSA 状态机。真实样本 selector 仍是 Guest LOAD，尚未变成
可预测的状态 PHI。仅追加标准 LLVM Pass 没有消除当前真实分发器。

需要设计：分发器识别、状态变量恢复、每个块的下一状态求解、条件状态转换，以及将
“块 → 分发器 → 块”改成直接控制流，正确修复 PHI、循环和副作用。

保留原程序真实 switch 和随输入变化的分支；unknown、缺失目标和未证明默认路径不能变成
`unreachable`。不能以 switch 总数归零作为验收。

### 4.5 P1：不透明谓词缺少跨块、路径与初始化内存相关证明

局部代数谓词折叠已经实现，包括部分含乘法的恒定条件。当前组件不利用前驱守卫或路径条件，
PHI 和不同 LOAD 通常作为独立未知输入，依赖调用后事实的条件也可能保持未知。

当前组合样本还包含依赖 `megaInit` 链表初始化的条件。测试中执行过初始化不等于静态前提成立；
`FunctionStart` 只限定入口 PC，并不限定其输入内存已经初始化。

需要分别规划：普通代数谓词、路径相关谓词、初始化内存相关谓词。若要去掉当前样本中的链表
条件，应明确有界初始化/堆关系的建模范围与入口上下文，无需扩成任意全程序堆分析。

### 4.6 P1：线性 MBA 的区域提取和覆盖有限

已有真值签名候选合成及完整字长 UNSAT 等价检查。当前主要限制包括跨 PHI/转换关系、
不同 LOAD 之间的关系、较大表达式、最多三个独立 SSA 输入及模块共享的查询预算。
默认最多 16 次查询/模块；不能通过删除证明检查或盲目放大预算解决覆盖问题。

需要设计：更合适的区域提取、预算分配、与状态/谓词/控制流步骤的迭代顺序，以及区分
“不支持”“证明失败”“预算耗尽”的报告。通用非线性 MBA 应单独限定范围。

### 4.7 P1：跨调用与循环内存效果仍有覆盖边界，向 LLVM 的应用也不完整

已有能力包括有界 x86 调用树、栈相关内存传递，以及确定控制流下的填充/复制循环。
当前并非“还没有调用摘要”。一般堆对象、未知外部调用、符号循环次数和复杂别名仍可使
事实退回 Unknown；已证明效果也尚未完整连接 LLVM 优化。

需要复用既有摘要，明确具体扩展域与消费者。函数名、导入名及某次动态结果都不是效果证明。
正常返回时的效果证明也不等于函数必定终止、无故障或满足真实 OS 页权限。

### 4.8 P1：需要明确“干净 IR”的观察契约

当前可执行 IR 保留完整 Guest 模型状态、内存和退出协议。寄存器、标志位计算及内存检查
中有许多属于可观察输出，不能为了接近原 C 函数而任意删除。

需要决定：是否在保留完整状态 IR 的同时增加面向阅读的切片/投影视图。如果只关心返回值
和指定内存输出，必须明确该视图的契约，不能混用完整状态等价与限定输出等价。

### 4.9 P1：区域选择、Pass 顺序和成本控制仍需产品化

区域接口要求显式提供起点；当前真实验证使用符号范围与静态 leader 规则。无符号程序的
自动选区、递归扫描缺失代码的补充解码尚需设计与验证。

`rebuild_decoded_blocks=True` 会从机器指令重建，可能覆盖先前自定义 IGNIL 改写的收益。
接口选择的是起点，不是严格地址沙箱，一个完整顺序块可能超出选点时使用的 symbol 范围。

每函数克隆整张 Graph、每个间接 JMP 连接覆盖块集合，是代码结构上已知的扩展成本；
大输入上的实际规模影响仍需测量。现有块/case 上限不是整个过程的 CPU/内存硬上限。

需要规定：自动选区、显式配置与默认兼容、Graph/IGNIL/LLVM Pass 顺序、重建与已有变换的
关系、事实缓存失效及资源统计。

### 4.10 P2：验证范围和无 Z3 测试工具仍有缺口

最新真实增益只覆盖两份冻结的 C O0 ELF、线性策略和八组输入。还需增加新程序、新种子、
C O2 二进制、无符号样本及适用的递归策略验证，分别报告语义正确性和去混淆结构收益。
旧 69 程序语料的 IR verify 不能替代当前新 Pass 的去混淆验收或全程序语义证明。

无 Z3 的 Tigress harness 存在一个已复现工具问题：原诊断直接构造 `SymbolicExecutor`，
因 `dragon_symexec_create` 不可用而中止。该额外队列尚未进入 C/Unicorn/native/LLVM 比较，
未将原计划 16 次比较报作成功。无 Z3 组件可用性已有三架构 240 次语义对照。

需要明确可选诊断不可用时的行为；完整语义检查及错误分类必须保持，不能静默跳过失败后
将整个队列计为通过。

## 5. 方案应交付什么

1. 按依赖划分实施阶段，列出每阶段的可观察成果、源码修改点与非目标。
2. 明确输入/入口/内存/输出契约，区分无条件证明与附带前提的专门化结果。
3. 设计 Pass 划分、顺序、重复执行条件，以及 Graph/IGNIL/LLVM 间的事实接口和生命周期。
4. 给出允许改写的充分条件，覆盖别名、部分写入、未知调用、循环、PHI 与默认目标。
5. 给出预算、失效、Unknown/Unavailable/Incomplete 和回退策略。
6. 给出逐阶段回归、旧代码失败证据、Unicorn/原生差分、负对照和真实结构收益的验收方案。
7. 明确哪些能力直接复用，哪些要新增；已有组件无需重新从零规划。

主要依赖链为：状态/内存事实 → 可消费的 SSA 表达 → 分发器改写。
阶段优先级可以调整，但不能用输入采样代替全输入证明，也不能把函数成功返回或 IR 变小
单独当成去混淆完成。

## 6. 建议阅读与源码入口

先读 [AGENTS.md](../AGENTS.md) 与 [correctness.md](correctness.md)，再读：

- [已解码区域提升](llvm-static-region.md)：最新契约、数据、限制和真实 IR 计数。
- [控制流清理](llvm-control-flow-cleanup.md)：标准 LLVM 步骤与实际收益边界。
- [跳转表内容证明](table-content-proof.md)：可信读取、查询位点、推断边撤回与复验。
- [条件入口事实](finite-initial-state.md)：调用者假设与已证明事实的区别。
- [有限分发](finite-dispatch.md)、[跨调用内存与循环](call-memory-loops.md)、
  [有限写集](finite-write-footprints.md)：已有分析域及限制。
- [MBA](llvm-mba-simplification.md)、[谓词折叠](llvm-predicate-folding.md)、
  [PassPipeline](pass-pipeline.md)。这些文档包含历史阶段结果，应以各自基线区分。

| 功能 | 主要源码 |
|---|---|
| LLVM 提升/区域覆盖/控制流 | `lib/LLVMLifter.cpp`；`include/dragon/lifters/llvm/LLVMLifter.hpp` |
| 管线调度与注册 | `lib/PassPipeline.cpp`；`include/dragon/passes/PassPipeline.hpp` |
| 有限值集、条件入口、内存查询 | `lib/analysis/FiniteValues.cpp`；`include/dragon/analysis/FiniteValues.hpp` |
| 分发与表内容 | `lib/FiniteDispatch.cpp`；`lib/analysis/TableContents.cpp` |
| MBA / 谓词 | `lib/LLVMMBASimplification.cpp`；`lib/LLVMPredicateFolding.cpp` |
| C / Python 接口 | `c_shim/dragon_c.h`、`c_shim/dragon_c.cpp`、`python/dragon.py` |
| 最新组件回归 | `tests/lifters/TestLLVMStaticRegion.cpp`；`tests/python/test_static_region.py` |
| 最新真实与小机器码验证 | `scripts/validation/static_region_replay.py`；`scripts/validation/static_region_diff.py` |

## 7. 环境与证据定位

- 本地仓库：`/Users/c/Desktop/deob/dragon-tales`。
- 构建/验证使用 Ubuntu aarch64 VM：`ssh cc@192.168.10.169`，LLVM 21.1.8、Z3 4.13.3、
  Unicorn 2.1.4；近期配置 8 GB RAM。
- **仅使用 owned worktree**：`/home/cc/dragon-tales-overlap`；构建目录 `build` / `build-noz3`。
  `/home/cc/dragon-tales` 是原 dirty checkout，保留原样。
- 最新验证使用 `cmake --build build -j1`、`ctest --test-dir build -j2`；本交接阶段不要求重新运行。
- VM 证据：`/home/cc/vmc-runs/llvm-static-region-20261109/`。
- Mac 证据：`/Users/c/Desktop/deob/validation-artifacts/llvm-static-region-20261109/`。
- 总索引：`final-artifact-index.json`；实现/接口冻结：`final/freeze.json`；
  真实样本汇总：`final-replay-summary-v1.json`；小机器码汇总：`native-metadata-v1/report-v2.json`。
- 冻结真实输入：`/home/cc/vmc-runs/pass-pipeline-20261106/tigress-s0/` 下
  `f-mba/O0/fixture.elf`、`f-combo/O0/fixture.elf`，对应源文件在各 profile 的 `transformed.c`。

普通视图此前首停点是 `0x10167 → 0x101ac`、`0x10adc → 0x1154c`。
新区域视图已经可以穿过这些位置并取得上述完整返回；不要再将其误报为新区域视图当前的停止点。

实际测试构建来源记录为 `d200f05 + patch`，其源码字节与 `4cd41b7` 对应；之后仅同步
Git HEAD，没有冒充在提交后重新构建。后续执行仍按仓库规则逐阶段验证、提交，保留失败原件。
