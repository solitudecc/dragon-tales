# LLVM 控制流清理

后续[已解码区域提升](llvm-static-region.md)已使本文两份线性 Tigress 样本取得 32 次完整
Guest 返回对照；它扩展显式 LLVM 覆盖，Graph 表内容证明仍保持 Unknown，真实分发器尚未消除。
下文原阶段的不完整计数是历史结果，保持独立。

本阶段以基线 `4487600` 为起点。目标是用框架分析混淆原理，并输出更清晰的 LLVM IR：
优先处理常见控制流平坦化、线性 MBA 和能证明的不透明谓词。独立可执行文件重建不在范围内；
特殊保护留给遇到具体样本时注册的自定义 Pass。

已有的 MBA 与谓词步骤会实际改写 IR，分别要求完整字长等价证明和恒定条件证明。
仅恢复 Graph 跳转边还不能消除 LLVM 中的状态分发。本组件增加显式 `control-flow` 步骤，
组合 LLVM 21 的 SCCP、DFAJumpThreading、JumpThreading、SimplifyCFG、InstCombine、DCE。
这是标准 LLVM 变换的编排，没有增加 Dragon 自定义的推测性边改写。

## 使用

```python
with dragon.PassPipeline([
    'mem2reg', 'instcombine', 'gvn', 'mba', 'predicates', 'control-flow', 'dce'
]) as pipeline:
    report = pipeline.run(lifter, max_iterations=3, max_pass_runs=24)
    if not report['ok']:
        raise RuntimeError(report['error'])
    cleaned_ir = lifter.ir()
```

优先使用 StaticAnalysis + FunctionStart 分析自然函数入口。Resumable 与 StatefulV2
仍保留各自入口、预算、内存检查和退出契约；本步骤不会替换或弱化这些契约。
入口策略必须在提升前设置，不能由优化器在看到状态机后自动推断。

C++ 可调用 `LLVMLifter::runControlFlowSimplification()`，或通过
`PassPipeline::add("control-flow")` 编排。C/Python 直接使用已有管线接口选择同名步骤，
不需要新回调。无 Z3 构建也能执行此步骤；MBA 和 SMT 谓词证明仍按原有不可用契约处理。
无参数，未知参数拒绝。历史 `optimize()` 的六步顺序与返回值保持兼容。

每次调用报告模块范围内的 `blocks_before/after`、`instructions_before/after`、
`conditional_branches_before/after`、`switches_before/after`。这些包括 Guest 协议与
内存检查，不能直接解释成“去掉了多少混淆”。穿线可能复制块，规模也可能增大。
管线限制执行轮次和步骤数；LLVM 内部成本阈值不是整模块内存或时间上限。

## 适用条件与边界

若状态由常量、PHI、select 和控制流决定，DFA 穿线可将可预测的
`case → dispatcher → case` 改为直接路径，并保留输入决定的真实循环和分支。
[LLVM 21 实现](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.8/llvm/lib/Transforms/Scalar/DFAJumpThreading.cpp)
要求 switch selector 本身是合适的 PHI，分析与复制也受启发式限制。
普通 JumpThreading 补充处理特定前驱上已知的条件。

下列情况不能据此宣称完成去平坦化：

- 机器间接跳转提升成 `switch(actual_target)`，selector 仍是 Guest 表 LOAD 或地址表达式。
  Graph 已知候选目标不等于表内容不可变；本组件不会把这些读取强改为常量。
- 状态来自未知调用、可能别名的写入或外部入口。优化必须保留这些输入允许的行为和默认边。
- Tigress 的堆链表条件依赖 `megaInit`。本组件不假设初始化、注入快照或删除未知调用效果。

前期 CVP/SCCP/普通 JumpThreading 实验只有少量 CFG 清理，见
[MBA 文档](llvm-mba-simplification.md)。FunctionStart 改善入口前提，但并没有解决上述内存问题。
`ok`、`converged` 和 LLVM verify 均不表示混淆已去除或语义已得到独立验证。

## 验证

4 项 C++ 回归在 `origin/main` 只加测试后全部失败（4 条断言失败），最终测试程序配合
冻结旧库重跑也得到同样的 4 项失败。旧六步保留测试状态机的 switch；新步骤消除已知
PHI/select 状态分发，保留真实循环、外部初态、定义明确的 default、volatile 写入和调用后读取。
默认执行为 43 条断言，导出原生夹具时另有 4 条文件写入检查。

| 验证 | 结果与范围 |
|---|---|
| 完整 CTest | 主构建 789/789；无 Z3 657/657 |
| 状态机 LLVM 原生执行 | `control_flow_machine_diff.py`：before/after × O0/O2 共 384 次匹配，其中优化后 192 次；8 个模块、外部状态及 12 个整数边界输入，比较返回值和完整 64 KiB 内存 |
| 状态机负对照 | 仅改返回值或仅改内存，各检出 384/384 次单字段差异 |
| 三架构 Guest 兼容性 | `control_flow_diff.py`：46 个现有夹具、184 个模块、3776 次匹配；其中 1040 次完整根返回、480 次声明边界、2256 次拒绝入口 |
| Guest 负对照 | 只改寄存器或内存，各检出 224/224 次单字段差异；包含正常和拒绝入口 |
| 精确 V2 预算 | 三架构、预算 0/1/2/3、O0/O2 共 192 次匹配：96 次完整返回、96 次预算边界；剩余预算单字段负对照检出 192/192 |
| 无 Z3 原生执行 | 三架构自然入口夹具 336 次匹配，其中 144 次完整根返回、192 次拒绝入口 |

所有上述执行检查均无新增排除、跳过或归一化。Guest 检查沿用 Unicorn 全模型字段、
完整 64 KiB 内存、退出记录和调用检查；拒绝入口和零预算使用明确的“不执行 Guest”契约。
状态机的独立算术规格不替代 Unicorn 指令参考。新编排器的纯自检还核对预算协议和失败报告，
不计作原生执行。首次完整 CTest 因既有 API 测试精确名称清单未包括新步骤而失败；
更新为新的精确集合、补上 C/Python 原生入口检查后，完整复跑通过，原日志保留。

两份历史 FunctionStart Tigress O0 内核追加同样步骤的只读 pilot：f-mba 指令
3603→3565，f-combo 15613→15589。switch 条件与回边均未变化；f-mba 的 6-case
是原程序 switch，f-combo 的 39-case 才是平坦化分发器。后者仍是 Guest LOAD selector，
因此这里只能归为轻量清理，不能把历史 IR verify 或 pilot 计为语义成功。

实现提交 `1a24a21` 后，当前库完成两份同字节 O0 ELF、两个策略、product O0/O2、8 输入的
重放：C 32 次匹配，LLVM 0 次完整返回 / 64 次 incomplete / 0 次不同 / 0 次其他错误，
两次原 oracle 退出码 1 原样保留。实际 Guest `megaInit` 照常执行，无初始化快照注入。
8 次模块 Pass 调用成功，但当前默认 Graph 没有证明这几处表内容、没有发布间接边；
fresh IR 的相关 switch 在优化前已经不存在。因此不能把 `switches_after=0` 当作消除了分发。

| 两策略相同的首个停止点 | 退出 |
|---|---|
| f-mba `0x10167`，`ff e0` / `jmpq *%rax` | Transfer/Branch，下一 PC `0x101ac` |
| f-combo `0x10adc`，`ff e0` / `jmpq *%rax` | Transfer/Branch，下一 PC `0x1154c` |

CFG wrong 和 unlifted 均为 0；线性策略执行但未解码的地址数为 0，递归策略在两 ELF 上分别
为 132 和 5704，保持显式缺口。以上是不完整分析的观测，不是完整函数语义通过。

基线双库、IR 前后版本、输入/输出和结果在 VM
`~/vmc-runs/llvm-control-flow-20261109/`。本次没有新单指令/IGNIL 活动，也没有重新计算
69 程序 corpus：指令提升、Graph 和默认六步未变，新增步骤通过显式选择执行，覆盖的是
上述改变的优化路径。现有 corpus 的 IR 合法性也不能用来证明本步骤完成了常见程序去混淆。

## 后续方向

下一组件先从上述两处停止点提取最小回归，追踪前驱 STORE/状态运算、表 LOAD 与间接跳转的依赖，
复用已有有限值集和内存效果分析。只有在全部入路径及 IR 输入契约下仍成立的事实才能用于改写，
不能仅凭 ELF 字节、权限标记或一次 Guest 初始快照假设内容恒定。随后将可证明的分发值关系
保留到 SSA，让控制流步骤能够消费；未知路径仍须保留真实退出和默认行为。

这一组件的验收要同时看到真实内核分发路径的恢复/直连，以及完整 Guest 返回匹配；
只减少前缀块数不算完成。实现不能根据上述地址或函数名写特例。MBA、谓词与控制流收益分别记录，
最终用不同程序和混淆种子核对收益与语义，保持特殊保护的插件扩展边界。
