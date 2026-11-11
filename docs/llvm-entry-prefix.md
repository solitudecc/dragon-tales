# 有界首次入口前缀：`entry-prefix`

`entry-prefix` 是显式启用的 LLVM pass。它将 [guarded entry view](llvm-guarded-entry.md)
成功检查后的首次机器执行前缀与原循环入口分开，使入口值不会立即和回入值合并为未知 PHI。
后续 LLVM 优化因此可能证明栈写地址、排除对已检查表项的覆盖，并消除这个前缀中的真实分发目标 LOAD。

它仍是静态分析和 IR 变换：构建、复制和优化不运行目标程序。生成的 IR 保留运行时检查、
普通回退和原循环。它不是完整去平坦化，不证明整张表永久不变，也不把首次入口的 SP
推广为整个函数或递归调用的 SP。

## 使用方式

先创建 raw guarded view，再显式运行一次前缀 pass，之后执行所需的普通优化。
默认 `optimize()` / `PassPipeline.DEFAULT_LLVM` 没有加入这个步骤。

C++ 可直接调用：

```cpp
#include <dragon/lifters/llvm/LLVMLifter.hpp>

// view 是 createGuardedEntryView 返回的独立 LLVMLifter。
auto stats = view->runEntryPrefix({32, 2048});
if (!stats.complete) {
  // 保留 stats.message、rejected 和 budgetExhausted 供分析报告使用。
}
view->runMem2Reg();
view->runInstCombine();
view->runGVN();
```

C 和 Python 通过现有 [PassPipeline](pass-pipeline.md) 的 builtin 注册接口使用它，
无需新增 C 参数结构。Python 示例：

```python
with source.guarded_entry_view(
    facts, context_id="entry_view", provenance="runtime-checked candidate bytes"
) as view:
    with dragon.PassPipeline() as prefix:
        prefix.add("entry-prefix", max_blocks=32, max_instructions=2048)
        report = prefix.run(view, max_iterations=1, max_pass_runs=1)
    counters = report["runs"][0]["counters"]

    with dragon.PassPipeline([
        "mem2reg", "instcombine", "gvn", "control-flow", "dce"
    ]) as cleanup:
        cleanup_report = cleanup.run(view)

    entry_symbol = view.guarded_entry_manifest()["view_symbol"]
    result_ir = view.ir()
```

调用者仍需显式选择 `view_symbol` 执行或导出。该步骤没有把普通函数入口改成 guarded
入口，内部调用和递归仍保留原符号。示例展示 API 编排，不代表任意样本都会得到同样精度。

适用模块为非空 `StaticAnalysis` 模块。通常应在 mem2reg 和 CFG 清理前运行，使用
`createGuardedEntryView` 保留的真实 `guarded.accept` 无条件边；若前置优化删除了这个
候选块，本 pass 不会猜测另一个入口或重新插入假设。块名只定位候选，不是内存内容、
支配关系或路径条件的证明。

## 预算与单次执行

| 参数 | 默认 | 合法范围 |
|---|---:|---|
| `max_blocks` / `EntryPrefixOptions::maxBlocks` | 32 | 整数 0–4096 |
| `max_instructions` / `EntryPrefixOptions::maxInstructions` | 2048 | 整数 0–65536 |

两个参数是**本次 pass 调用在整个模块累计成功复制的预算**，按函数顺序共享，不为每个
函数重新分配。每个候选尝试使用剩余预算。0 禁止新的复制；存在待处理候选时会报告预算不足。
C/Python builtin 拒绝布尔值、字符串、负数、超上限和未知选项。

`cloned_instructions` 统计原区域复制指令与新增直接出口 PHI 的总预算成本；根 PHI 后来可能被入口值替换，
因此该数不是最终 IR 指令净增长，也不包含事务暂存的整个函数体。为限制事务成本，单个
源函数还受 65,536 块和 1,000,000 条指令的固定上限保护。

本次调用只冻结原 `guarded.accept` 候选，不将新块继续加入候选队列。若 seed 的目标已经
只有该 seed 一个 predecessor，则跳过；这既覆盖已经隔离的入口，也覆盖原本就无需隔离
的单入口块。重复调用不会沿刚生成的入口副本继续增长复制链。

这些预算独立于 `dispatch-specialize` 的原六块限制，也不会增加外部流水线的迭代数。
不需要通过增加旧 pass 的预算来开启本组件。

## 保留的执行行为

每条边以原来源块、successor 序号和原目标为身份。根 PHI 使用真实 seed 入边的值；
支配 seed 的非 constant 外部 SSA 值也可保留。内部 PHI 按真正的克隆入边筛选，出口 PHI
按实际边数补入本次克隆值，包含重复 switch successor 的情况。

区域内的 LOAD、freeze、STORE 和未知条件保持执行次序。DFS 发现的回入边接回原块，
不再执行首次入口事实；后续迭代继续原代码。包含调用的块保持在原区域，前缀不会越过未知
调用获取新的常量事实。后续内存优化仍须按调用和可能别名写入的实际效果失效。

Switch 是正常分发边界。只有直达 RET、所有原 incoming 都来自同一个 switch 来源块、
且满足安全准入的 terminal leaf 可以计入同一预算。它保留未知 selector、错误出口、
寄存器与 Exit 的全部写入；不是删除 default 路径。其他分发目标和回入边继续指向原块。

整个过程不添加 Guest STORE、`noalias`、表 immutable 标记或新的 `llvm.assume`。
描述器大小重新读取和无法证明的 bounds 仍保持真实运行时检查。

## 直接出口 SSA 值传递

若某个值在复制块 `D` 中定义，而普通外部使用全部局限在其直接出口 `Out`，且 `Out`
的唯一原 predecessor **块**正是 `D`，可以在 `Out` 插入一个 PHI。原边传原定义，
本次克隆边传本次映射值；重复 successor 按实际边数分别提供同值 incoming。原有 PHI
入边仍按既有规则修复。

此规则允许把执行前计算的地址或 LOAD/freeze 值传到原调用块，调用、volatile 访问及
后续写入都仍在原处执行。它只传递已经计算的 SSA 值，不证明调用无副作用，也不允许把
调用前的 Guest 内存事实传播到调用后。

修复限定在该直接出口内：多来源汇合、普通使用分散到多个出口、跨越中间块的传播及
原区域历史使用不因这个扩展而获准。每个候选有固定上限：64 个值、512 个普通 use 和
512 个 PHI incoming 槽。每个新 PHI 还计入同一 `max_instructions` 剩余预算；不足时在
修改前拒绝并报告预算事件。成功的预算截断前缀仍可能报告 `Incomplete`。

## 事务、拒绝与句柄

每个候选先在同一 Module 内的临时 Function 中复制、变换并验证。函数引用保持指向
原 Function，临时函数不会成为递归或外部调用的目标。提交时映射回原参数对象并移动
已验证的函数体；原函数体暂存到回滚对象，提交验证失败时恢复。

成功提交保留原 Module、Function、Argument 身份以及原函数属性、全局函数指针和调用引用。
函数体的 BasicBlock/Instruction 身份不保留，使用者必须废弃相关句柄和缓存的 CFG 分析。
PassPipeline 会按变换步骤使分析缓存失效。

拒绝是**逐函数事务**：该函数的原 IR 保持不变；同一次调用中先前成功提交的其他函数
不会因此回滚。所以允许出现 `changed=true` 且 `complete=false`，表示有安全的局部结果，
同时有未完成候选。

以下形态保持保守拒绝，而不是生成近似语义：

- 无法修复的普通 non-PHI 跨区域使用，或外部 SSA 不支配 seed。
- 复制区域在 seed 前已经执行过的历史形态；不把旧执行值当作 fresh LOAD/freeze。
- 原函数中的 address-taken block、EH 或不支持的 debug 身份。
- 区域 alloca、token、unsupported terminator，或无法合法映射的 PHI/边界。
- 不满足独占来源条件的 switch RET 出口不能纳入 closure；若由此留下无法修复的
  non-PHI live-out，则拒绝候选。不能为了保留收益绕过这项检查。
- 输入 IR 无效、候选验证失败或预算不足。

整个模块在变换前验证，候选及提交后函数再次验证。verify 只证明 IR 结构合法，不能代替
独立语义对照。该组件不承诺对任意用户改写的 LLVM 模块提供完整等价证书。

## 报告解释

| counter | 含义 |
|---|---|
| `available` | 是否处于支持的非空 StaticAnalysis 模块 |
| `candidate_edges` | 本次冻结的 `guarded.accept` 候选数，包含随后拒绝或跳过的候选 |
| `peeled_edges` | 实际成功提交的入口前缀数 |
| `cloned_blocks` / `cloned_instructions` | 成功提交的累计复制预算成本 |
| `original_exits` | 保留为原块出口的实际边数 |
| `internal_edges` / `backedges` / `terminal_edges` | 内部边、保留的 DFS 回入边、纳入的 terminal leaf 边数 |
| `rejected` | 未提交的拒绝候选数 |
| `budget_exhausted` | 预算不足事件数 |
| `skipped_single_entry` | 目标只有 seed 一个 predecessor 的正常跳过数 |
| `complete` / `changed` | 本次是否无拒绝/预算不足，以及是否提交了变换 |

任何候选拒绝或预算不足都使 `complete=false`，builtin 返回 `Incomplete` 并在 `message`
记录诊断；它不是成功消除混淆。无候选或单入口跳过是正常不变结果。
`complete=true` 也只说明本次有界组件处理完成，不能理解为整个 CFG、表内容或去平坦化完成。

## 当前验证范围

本阶段 main/no-Z3 各通过聚焦回归 **5 cases / 107 assertions**、Python 路由/严格参数/
低预算拒绝/重复调用检查，以及真实 API 接入的独立原生对照 **1824 次完整状态匹配、
198 个指定错误控制检出**。完整 CTest 分别 **1051/1051、916/916**。
聚焦回归覆盖函数/模块/参数身份、全局引用与普通元数据、
PHI/重复出口、fresh 值、预算和拒绝时原 IR 不变。

生产 pass 加旧三轮配方的四 ELF 真实回放为 **C16/LLVM16** 完整状态匹配，
144 个错误控制全检出。f-cff O0 在 32/2048 下复制 21 块、748 条指令，后续优化
消除了 slot 70032 的首次实际 selector LOAD/switch，保留动态 bounds 并直达原
`bb_67010`。入口字节仍须检查，原循环/分发器仍保留；combo O0 前缀预算不足，
combo O2 保守拒绝。旧内存/分发分析的 incomplete 仍如实报告。

完整阶段验证及归档见[真实验证](entry-prefix-real-validation-20261111.md)、
[生产集成验证](entry-prefix-validation-20261111.md)和
[独立原生验证](entry-prefix-production-native-20261111.md)。
