# 已解码区域的静态 LLVM 提升

2026-11-09，基线 `d200f05`。本组件为分析混淆代码补齐 LLVM 执行覆盖，不发布新的
Graph 间接边，也不把 image 表项当作运行时常量。后续仍需将可证明的分发状态关系转成
SSA，才能进一步消除真实平坦化分发器。

## 实际缺口

前一阶段两份 Tigress O0 ELF 在 `0x10167`、`0x10adc` 的间接 JMP 处退出。
表内容 Unknown 时保留退出是正确行为；但线性反汇编列表中已经存在后面的机器指令。
进一步核对发现，listing 中的 decoded 指令还不一定有 BasicBlock/IGNIL：

| 内核 | 已解码指令 | 原有效块覆盖的指令 | 无有效块覆盖的指令 |
|---|---:|---:|---:|
| f-mba | 360 | 242 | 118 |
| f-combo | 5935 | 330 | 5605 |

`0x10169`、`0x101ac`、`0x1154c` 都没有原有效块；它们可由“机器终结指令之后的
下一条已解码指令”静态选出，无需观察 CPU 目标或读取跳转表来证明目标。
普通 `Function` 仍只包含按已知 Graph 边遍历到的成员，不能因此宣称这些额外块可达。

## 显式覆盖契约

新增 C++ `LLVMLifter::liftStaticRegion(root, graph, additionalBlockStarts, options)`，
仅接受 `StaticAnalysis + FunctionStart`。它保留原函数成员，并允许实际内部跳转进入
额外选择的块。外部输入 PC 仍只接受自然函数入口；不是新的任意 PC 恢复模式。

间接 JMP 按 **真实运行时 target** 生成 switch，列出本次覆盖的合法块起点。
未命中的目标保持原 `Transfer/Branch` 退出、精确 Guest PC 和副作用；不生成代表未知的
`unreachable`。直接 JMP/CJMP 到新增块也可以继续执行，区域外仍退出。
CALL 保留机器返回槽、独立静态 callee 和调用后核验/重载；RET 保留原静态返回协议。
因此本视图扩大了普通 `lift()` 的内部执行覆盖，不能声称两种视图的停止边界完全相同。

默认选项使用现有 IGNIL，新增地址必须有真实首指令、有效 BasicBlock 和 IGNIL。
`rebuildDecodedBlocks=true` 则在**私有 Graph 副本**注册已解码指令起点、切分块，使用现有
机器 lifter 重建所选路径，并补齐真实顺序 fallthrough。它使用机器指令，不继承调用者
自定义的 IGNIL 改写；原 Graph、函数成员、出边、flags 和借用视图均不改变。
没有 MCInst 语义来源的手工 Instruction 会拒绝；未知机器语义仍是显式 Unsupported。

选择的是块起点，不是地址沙箱。一个块的顺序指令和原函数成员可能超出选择这些起点时
使用的 ELF symbol 范围。接口也不自动解码缺失字节或追踪所有后继。
递归扫描未解码的路径仍须先获得静态解码覆盖；不能把本轮线性结果外推到递归策略。

硬上限为 4096 个块、累计 65536 个间接分发 case；只能下调，不能截断后宣称覆盖完整。
机器重建额外限制累计 65536 条指令。选址/预算失败在模块改写前拒绝；晚失败删除不完整
函数体，保留原声明的属性、参数名和既有使用。Graph clone 和已有块遍历仍受整个输入图
大小影响，这些上限不是整模块 CPU/内存限额。

生成的函数保留 `dragon.static-entry-policy=function-start`，并记录
`dragon.static-coverage=decoded-region`、`dragon.coverage-blocks` 和
`dragon.coverage-source=graph-ignil/decoded-machine`。不改 Guest ABI 或静态 callee 名称。
普通 `lift()`、默认六步优化、Graph 目标完整性判定保持原有路径。

后续 S1a 将两种区域准备路径统一到内部拥有只读 Graph 的 `PreparedRegion`，严格 IGNIL 路径
也会生成快照；普通 `lift()` 不经过它。操作来源按准备实例/块/op 序号区分，但尚不包含 LLVM
访问映射或内存值证明。范围和验证见[实施进度](deobfuscation-progress-20261109.md)。

## 使用

```python
with dragon.LLVMLifter(image.architecture,
        execution_mode=dragon.LLVMExecutionMode.STATIC_ANALYSIS,
        static_entry_policy=dragon.LLVMStaticEntryPolicy.FUNCTION_START) as lifter:
    fn = graph.get_function(entry)
    # starts 来自静态反汇编：原块、直接分支目标、终结指令之后的指令等。
    assert lifter.lift_static_region(fn, graph, starts,
        rebuild_decoded_blocks=True, max_blocks=4096, max_dispatch_cases=65536)
    with dragon.PassPipeline([
        'mem2reg', 'instcombine', 'gvn', 'mba', 'predicates', 'control-flow', 'dce'
    ]) as pipeline:
        report = pipeline.run(lifter)
        if not report['ok']:
            raise RuntimeError(report['error'])
    ir = lifter.ir()
```

C 接口是 `dragon_llvm_lifter_lift_static_region`，接收地址数组、两个上限和
0/1 重建标记。Python 拒绝非法上限、非 uint64 地址和非 bool 重建开关；读取地址生成器
最多 `max_blocks + 1` 项。C/C++ 返回 false/0 时必须检查结果。
支持无 Z3；额外覆盖不依赖求解器。现有 MBA/谓词步骤仍遵守其各自的可用性与证明契约。

## 验证

| 验证 | 结果 |
|---|---|
| C++ 组件 | 10 项 / 880 条断言，主构建与无 Z3 均通过 |
| 完整 CTest | 主构建 800/800，无 Z3 668/668 |
| C/Python | 三架构路由、模式、硬上限、生成器界限、私有重建与 Graph 保持检查通过 |
| 三架构小机器码 | 主/无 Z3 各 240 次匹配：96 次完整根返回、48 次区域外 Transfer、96 次内部 PC 拒绝 |
| 小机器码负对照 | 仅破坏返回寄存器/内存，各检出 240/240 次指定单字段差异 |
| 同字节旧库小机器码 | 0 完整根返回、96 incomplete；另 48 次 Transfer 与 96 次拒绝入口匹配，原退出码 1 保留 |
| 真实 Tigress，线性 | 两 ELF、product O0/O2、8 输入：C16 匹配，LLVM32 完整根返回匹配；wrong/incomplete/unlifted/executed-undecoded 均 0 |
| 同字节旧库 Tigress | C16 匹配，LLVM0 完整 / 32 incomplete，原退出码 1 保留 |

小机器码在 Guest STORE 前将表项初始化为另一个目标，再由输入寄存器 STORE、LOAD/JMP，
同一编译模块分别执行两个内部目标和一个外部目标。沿用 Unicorn 的全部模型字段、64 KiB、
PC/SP、七字段 Exit；内部 PC 拒绝使用原“不执行 Guest”契约。无新增排除、跳过或归一化。
Tigress 沿用原 C 规格、动态 CFG 和全部模型字段/1 MiB 检查，真实 Guest `megaInit` 执行，
没有注入初始堆快照。每次提升前后核对原 Graph 的机器字节、语义操作数、markers、边与成员。

新 API 的仅测试构建在旧头文件上因缺少 API 无法编译，原日志保留；不能将其称为旧库
执行语义错误。另用真实旧库取得上表 before/after 数字。首轮晚失败测试误用
`Graph::addIGNILBlock` 的 emplace，未替换已有 IR；改为原位替换并断言真实 Temp999 源后
复跑，未放宽失败检查。重建回归还覆盖孤立指令、块内切分/旧前缀失效、缺 MCInst、
零 IR unlifted STORE、寄存器/向量收集、JMP/CALL 区别及前向声明恢复。

当前普通 lift 与旧库的四模块各阶段 16 份 IR 逐字相同；8 份 Graph 快照也相同。
额外 O2 两文件仅首行 ModuleID 的真实输入路径不同，其后字节相同；原文件未归一化。
本轮未重跑 69 程序 corpus 或单指令活动，覆盖的是新 opt-in 路径及公共路径兼容性检查。
这些结果不是任意输入、所有程序或完整优化器正确性的证明。

额外的无 Z3 Tigress 尝试在原 harness 的 SymbolicExecutor 诊断阶段因该可选符号不可用
退出，尚未开始 C/Unicorn/native/LLVM 检查；失败日志保留，没有修改或跳过诊断后报成功。
无 Z3 本组件的语义证据为上述三架构 240 次对照。

## 真实收益与下一步

32 次真实完整返回说明先前退出后的代码现在有了可执行、可优化的 LLVM 表达。
这还不是去平坦化：严格 LLVM 指令计数下，MBA 内核六步后 3908→控制流步骤后 3870，
combo 为 17176→17141；switch 和回边保持不变。额外 LLVM O2 后分别 3877/14721 条指令，
仍有 1/2 个 switch，每个包含 26/102 个覆盖 case。case 行不计作 LLVM 指令，不能混用
原 harness 的缩进行数统计。本轮没有新增 MBA 或不透明谓词证明算法。

下一组件聚焦 Guest 栈状态/表读取的可证明关系，在保留实际读取、别名写、未知调用和
默认路径语义的前提下将状态关系暴露给 SSA。验收要看到真实分发路径减少，而不仅是
寄存器、内存检查或前缀清理。特殊保护仍由具体样本的自定义 Pass 处理。

完整证据在 VM `~/vmc-runs/llvm-static-region-20261109/`；Mac 小汇总在
`/Users/c/Desktop/deob/validation-artifacts/llvm-static-region-20261109/`。原生与失败原件保留。
