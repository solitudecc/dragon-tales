# Guest 内存 SSA 与流水线顺序

诊断基线 `ca245bc`。本记录只读取封存 IR 和源码，没有重新执行目标、运行优化器或
扩大证明条件。原始 raw 精度失败应继续保留，不能通过重命名 post 流水线将其改为成功。

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

## 公共入口与最小集成方向

`MemoryForwardOptions::runMem2Reg` 默认 true。
`LLVMLifter::runGuestMemorySSA` 在调用内部实现前，按该选项调用 `runMem2Reg()`。
`detail::runGuestMemorySSA` 本身不执行 mem2reg；直接调用 detail 或显式传 false
时，调用者负责准备输入。只有 mem2reg 是否已经足以达到每个真实样本的收益，
本次只读诊断没有另做执行验证，不能从 post 的结果推定。

在基线 `ca245bc`，PassPipeline 有 mem2reg、instcombine、gvn 和 dispatch 的内建
入口，却没有 Guest SSA 内建入口；历史脚本为此自行注册 callback。最小集成方向
是提供显式 Guest SSA 入口，并让用户能够直接组合已有的已验证顺序：

```text
mem2reg → instcombine → gvn → GuestSSA
→ mba → predicates → dispatch-specialize(allow_plain_stores=1)
→ dispatch-deflatten → control-flow → dce
```

这是建议的显式配方，新增内建入口的实现、选项验证、计数/Incomplete 传递和最终
验证由对应实现记录说明。本诊断不修改默认 optimize 顺序，不授予初始化内存或表项
不可变假设。多轮迭代仍需调用次数和工作量上限，不能把单轮预算重置包装成无界分析。

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
新目标执行数和新分析运行数均为零。

已封存的 post 结果包含实际 index70032 target LOAD，原 66 cases/default 和 LOAD
仍保留；CFF switch 1→2 是上下文副本。raw 的严格索引门槛仍返回 1。两组原有
C16/LLVM16 完整根匹配和错误控制不因本次只读诊断新增计数。分发表内容证明、全部
状态恢复和一般循环内存不动点均不能由这项流水线修补宣告完成。
