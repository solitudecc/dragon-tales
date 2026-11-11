# 剩余 Guest 表 LOAD 的内容证明评估

基线：`9f9e5f9`。本次为源码与既有冻结证据的只读审查，没有重新执行真实样本，也没有实现新的入口假设。

## 结论

已知表项地址不等于已知表项值。当前 StaticAnalysis + FunctionStart 只限制入口 PC，Guest 内存仍任意。已有真实调查记录四张 CFF 表在已测程序路径中没有 Guest STORE 初始化；同一槽可合法含目标 A、目标 B 或未知地址。因此当前完整状态基线不能无条件消除这些 LOAD 和分发。

如果后续发现真实初始化 STORE，应继续在实际 LLVM CFG 上证明 reaching write、路径完整性和未被破坏，复用 GuestMemorySSA。否则需要另名的显式上下文产物；不能自动将 ELF 字节、`.rodata`、image reader 或一次回放升级为不变内容。

真实证据依据：`docs/same-function-cff-validation-20261111.md` 的表内容调查，以及 `docs/future-region-independent-validation-20261111.md` 的已知 state50 / Guest index70096 分析。此处没有声称新增样本验证。

## 当前可复用的 API 与限制

| 代码位置 | 当前机制 | 能证明什么 / 不能证明什么 |
|---|---|---|
| `include/dragon/lifters/llvm/LLVMLifter.hpp` | `StaticEntryPolicy::FunctionStart` | 仅入口 PC；明确不假定内存 |
| `include/dragon/analysis/FiniteValues.hpp`、`lib/analysis/FiniteValues.cpp` | `finiteMemoryValues` | 精确 IGNIL LOAD 位点前的真实 STORE 事实 |
| 同上 | `FiniteEntryState`、`finiteMemoryValuesWithInitialState` | 有界的条件分析；`assumptionsValid` 仅结构合法，不是前提为真 |
| `python/dragon.py` | `Graph.finite_memory_values_with_initial_state` | 已暴露只读条件报告；不安装 Graph 边或 LLVM 假设 |
| `include/dragon/disassembler/Disassembler.hpp`、`lib/analysis/TableContents.cpp` | `readKnownMemory(site,address,bytes)` | 可信提供者声明每次到达该 LOAD 时的内容；比入口字节强，必须覆盖后续 CFG 扩展 |
| `include/dragon/lifters/llvm/CallEffectSummary.hpp`、`lib/CallEffectSummary.cpp` | `attachCallEffectSummary` | 一个调用正常返回后的完整写足迹和常量；不能变成 callee 入口前提 |
| `lib/GuestMemorySSA.cpp` | `invalidateSummaryWrites` / byte 对应实现 | 消费已验证摘要中的 GuestMemory 写后常量；默认关闭摘要消费 |
| `lib/GuestMemorySSA.cpp` | Graph-aware `proveLeafCalls` | 目前只保留已证明完全不写内存的内部调用；不是自动初始化器摘要生成器 |
| `lib/LLVMLiftProvenance.hpp` | `GuestAccessSite`、`MemoryCheck`、`EdgeSite` | 可标识原操作、真实 LOAD、bounds 成败边与实际 LLVM terminator；身份信息本身不是语义证明 |

现有 LLVM 公共接口没有入口内存初始化/约束消费者。`Execution.h` 的 GuestMemory 是调用者持有的运行时映射描述，不是优化器的内容证书。

## 两个必须避免的错误桥接

1. **入口字节不能当全程 immutable。** 即使显式提供入口表字节，任意 SP/RBP 的栈 STORE、可能别名的输出 STORE、未知调用或同步仍可能修改表。入口 SP 未受限时，`[RBP-offset]` 可能正好覆盖表槽。不得因命名为“栈”或“表”就假定不相交。
2. **IGNIL Graph 的路径集可能小于 decoded-region LLVM。** `LLVMPreparedRegion.cpp` 冻结并补齐选定块，但不为所有动态目标补 Graph 边；`LLVMLifter.cpp` 的 decoded-region lowering 则允许实际 selector 跳到 `bbMap` 中每个已覆盖块。直接将 Graph 条件分析的常量替换 LLVM LOAD，可能漏掉实际 LLVM 回入路径上的覆盖写。须在实际 LLVM CFG 上传播，或先证明两者 CFG 闭包相符。`OpOrigin` 身份一致不足以授权替换。

例如原 Graph 将动态跳转看作终止，而 lifted switch 实际能进入一个改写表槽后回跳的块：仅在 Graph 首次到达 LOAD 时成立的常量，不保证第二次到达仍成立。

## 建议的最小机制

第一优先仍是无额外前提的当前函数内 STORE/LOAD 关系修复。针对实际剩余表内容，若确需上下文能力，应新增独立的有界入口上下文组件，不改变现有 `guest-memory` 默认或原函数入口契约：

1. 输入采用 `FiniteEntryState` 同样的有限寄存器字节/Guest 字节语义，但明确绑定函数入口、独立 context ID 和事实提供者；验证不重叠、不溢出、预算、位宽与所有权。命名可以复用，不能直接把条件报告当证明。
2. 生成另名的上下文函数/模块，并随产物保留完整前提清单；无条件函数保持原状。若要求沿用通用调用 ABI，则必须提供无新增故障/副作用的实际检查及失败回退，而非只添加 metadata 后声称全输入等价。
3. 在实际 LLVM 入口上一次性建立事实，绑定对应入口 Guest base 的 SSA 身份。不要给每次后续 Guest base LOAD 自动重新播种；循环回边必须与事实合并，不可重置入口事实。
4. 复用 `AddressKey`、word/byte facts、别名失效和循环证明来传播。初始化事实本身不证明后续 size 检查成功；不得删原故障出口。未知调用、未知写、部分重叠、descriptor/base 关系不明都须失效或保留原 LOAD。
5. 只对有完整当前位点证明的 LOAD 替换，随后用现有 `dispatch-specialize` 和标准 LLVM CFG 清理。只得到索引、复制分支或增加一个 target 比较，不记作表内容证明或完整去平坦化。

这需要新的显式上下文接口和独立验证，不建议把入口字节装成“假调用”的 CallEffectSummary，也不建议直接对原函数插入无条件 `llvm.assume`。如果只为本轮无条件基线收益，不应启动该组件：真实缺失的初始化事实不会由增加迭代预算产生。

## 最小验收边界

- 原无条件入口在任意表内容 A/B/default、任意合法栈位置下保持完整状态行为。
- 上下文产物只在明确列出的前提域比较；若有检查包装层，则包装层必须在全部原输入域与基线一致，错误表内容走原逻辑。
- 实际 STORE 证明、同值与异值汇合、循环二次覆盖、栈/表重叠、部分写、未知调用、已证明调用写后常量分别覆盖。
- LOAD 的 bounds 失败、Guest base 变动、额外独立入口、CFG 新暴露回入和预算耗尽均不得留下错误常量。
- 故意删除 STORE、使用入口旧值替代循环新 LOAD、将调用 postcondition 错当入口事实等 controls 必须被完整寄存器/Guest/Exit 对拍检出。

该建议不涉及 VM、堆链表分析、文件只读段不变性或自动全程序初始化建模。
