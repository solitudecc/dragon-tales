# Guest 内存 SSA 与流水线顺序

历史流水线诊断基线 `ca245bc`。下面的 raw/post 诊断只读取封存 IR 和源码；后续
内建入口与循环归属修复单独记录，不追溯改写历史验证结果。原始 raw 精度失败应继续
保留，不能通过重命名 post 流水线将其改为成功。

## 为什么 raw 缺少可选的状态 PHI

`scripts/validation/guest_memory_checked_edges_real.py` 的历史 raw 配方依次运行：

```text
GuestSSA(run_mem2reg=False)
mem2reg → instcombine → gvn → mba → predicates
dispatch-specialize → dispatch-deflatten → control-flow → dce
```

post 配方仅在 GVN 之后增加一次 `GuestSSA(run_mem2reg=False)`。两个配方的第一轮
GuestSSA 都在寄存器 alloca 尚未提升时执行，不是默认公共 API 的使用方式。

冻结 f-cff O0 的 raw/post `before-memory-stage-0.ll` 和
`after-memory-stage-0.ll` 四份文件逐字节相同，SHA-256 全为：

```text
a9001c948bbd137491e89cf845751a5c2aadd4386f186e158b36147f4091bb20
```

此时 `bb_66234` 仍执行 `%52 = load i64, ptr %RBP`，`memory.valid7` 读取 Guest
状态后又写回 RAX/AL/EAX 等 alloca。第一轮没有转发，完整状态语义并未出错。
之后 mem2reg/InstCombine/GVN 暴露了 `%RBP.1` PHI 和更直接的地址关系，但 raw
配方没有再次运行 GuestSSA。

post 在这些关系出现之后运行 GuestSSA，证明 `memory.valid5` 的 STORE50 经检查
链到达状态 LOAD。它生成 `bb_66234.guest.edge` 和 `memory.valid7.guest.edge`，
副本中省掉已证明的 LOAD，同时保留两层 bounds 和失败出口。SSA 修复在
`memory.valid9` 生成：

```llvm
%guest.edge.live459 = phi i64 [ 50, %memory.valid7.guest.edge ],
                            [ %430, %memory.valid7 ]
```

这个 PHI 来自逐边内存转发后的合流修复。它不是原始机器代码必须含有的状态变量声明，
也不是单独执行 mem2reg 就必然生成的状态 PHI。raw 到达 dispatch-specialize 时，
`memory.valid9` 仍只有原 `memory.valid7` 入边，状态仍由 Guest LOAD 提供，没有
可选的常量 50 入边。post 此处有 13 个修复 PHI，其中包含上面的状态 PHI。

因此这里已证实的是**流水线未在规范化之后重新消费内存关系**，不是已经发现新的
生产语义错误，也不能据此断言必须新增一套从 raw alloca 恢复状态的分析器。

## 公共入口与已实现的显式流水线

`MemoryForwardOptions::runMem2Reg` 默认 true。
`LLVMLifter::runGuestMemorySSA` 在调用内部实现前，按该选项调用 `runMem2Reg()`。
`detail::runGuestMemorySSA` 本身不执行 mem2reg；直接调用 detail 或显式传 false
时，调用者负责准备输入。只有 mem2reg 是否已经足以达到每个真实样本的收益，
本次只读诊断没有另做执行验证，不能从 post 的结果推定。

在基线 `ca245bc`，PassPipeline 有 mem2reg、instcombine、gvn 和 dispatch 的内建
入口，却没有 Guest SSA 内建入口；历史脚本为此自行注册 callback。后续实现
`a7ed9bd` 已增加名为 `guest-memory` 的内建入口，保留旧插件名称
`guest-memory-ssa` 的注册空间。调用者可以显式组合以下顺序：

```text
mem2reg → instcombine → gvn → guest-memory(run_mem2reg=0)
→ mba → predicates → dispatch-specialize(allow_plain_stores=1)
→ dispatch-deflatten → control-flow → dce
```

该入口复用 `LLVMLifter::runGuestMemorySSA`，默认 `run_mem2reg=1`，将可用性、
完成状态、改写状态和全部原生统计传给流水线。配置只接受公布的 uint32 整数参数，
布尔开关只接受 0/1；依赖 Graph 的 `prove_leaf_calls` 和 `max_call_*` 不属于这个
无 Graph 入口。`consume_call_effect_summaries=1` 仍只消费已验证的调用效果元数据。
`Result.changed` 包括实际 IR 变化，可能因 mem2reg 而不同于 GuestSSA 自身的
`changed` 计数，不能将二者混为同一个指标。具体用法见 [Pass 流水线](pass-pipeline.md)。

默认 optimize 的六步顺序保持不变；此配方没有授予初始化内存或表项不可变假设。
多轮迭代仍需调用次数和工作量上限，不能把单轮预算重置包装成无界分析。

## 嵌套循环成员漏标与修复边界

内建入口与历史 callback 的对照发现部分统计受宿主对象布局影响。进一步检查定位到
GuestMemorySSA 的普通事实路径和字节事实路径使用了相同的循环闭包错误：全局
`loopBlocks` 同时充当各条 backedge 的 visited 集合。考虑以下 CFG：

```text
E → O → P → I → B → I
    ↑       ↓
    L ←─────┘
O → Exit
```

`B → I` 是内循环 backedge，`L → O` 是外循环 backedge。若先处理内循环，
全局集合已含 I/B；外循环从 L 反向走到 I 时误以为已经完成遍历，遗漏 P。
先处理外循环则不会遗漏。遍历顺序来自指针键容器，不能以“排序后计数稳定”替代
正确的循环闭包。多 latch 也必须分别完成闭包，自环必须在自己的 header 停止。

修复在两处都为每条 backedge 建立独立 visited，预置 header 作为停止点，并将
本次遍历到的块合入全局 `loopBlocks`。没有扩大内存证明、改变循环 invariant
规则或增加迭代预算。普通路径按正确成员集合延迟循环内改写并复验 invariant；
字节路径继续禁止事实跨越其保守契约中的循环边。

这不仅影响显示计数：遗漏 P 会影响立即/延迟转发及循环出口识别；遗漏相邻的 P/Q
还会使字节事实穿过本应停止的边。因此不能只从比较器里去掉 `loop_barriers`。
另一方面，现有回归中旧版的块内 STORE→LOAD 转发本身可能语义正确；修复恢复的是
一致的循环归属与保守准入，不能将布局相关的额外转发直接描述成已证实的错误执行。

新增 `TestLLVMGuestLoopMembership.cpp` 覆盖 108 组嵌套循环宿主分配布局（含多个
latch）、自环，以及 32 组 P 写两个 i32、Q 读 i64 的字节路径布局。字节夹具关闭
PHI/checked-copy 入口，单独检验跨块字节事实边界。相同最终三个测试在旧 B244
运行 1018 次断言、失败 432 次；修复后相关组 82 个测试/3516 次断言通过。
这些数字是聚焦结构验证；独立 native 的执行、错误控制、完整 CTest 和真实样本结果
须使用对应最终验证记录，不由结构测试代替。尤其字节夹具不自动计入其他 native
矩阵的覆盖范围。

该修复也不承诺所有诊断统计从此与布局无关：join 候选仍可能取自指针排序的首条
入边，`join_barriers`/`partial_overlaps` 必须分别核查。历史严格统计比较的失败
应保留，不能因 IR 逐字节一致就将失败改记为通过。

## 证据与当前边界

只读输入来自双端封存的
`store-hub-reference-final-v3-20261111.tar.gz`，SHA-256：

```text
a795157ea1dfafad4690e9b55f470efafb152d0335fd9d603559c5a0447a04eb
```

对应 tar member 前缀是
`store-hub-reference-20261111/candidate-real-{raw,post}-v3/oracle/f-cff/O0/s0-product-O0/`。
分别读取 before/after-memory-stage-0、post 的 before/after-memory-stage-1、
before/after-store-context 和 pipeline-report 即可复核，不需恢复整个归档。
本地摘取和来源 manifest、逐块诊断 JSON 位于 `/tmp/dragon-raw-pipeline-diagnosis/`，
该历史摘取诊断的新目标执行数和新分析运行数均为零。

已封存的 post 结果包含实际 index70032 target LOAD，原 66 cases/default 和 LOAD
仍保留；CFF switch 1→2 是上下文副本。raw 的严格索引门槛仍返回 1。两组原有
C16/LLVM16 完整根匹配和错误控制不因本次只读诊断新增计数。分发表内容证明、全部
状态恢复和一般循环内存不动点均不能由这项流水线修补宣告完成。
