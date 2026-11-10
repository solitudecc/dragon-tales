# S2 Guest 内存 SSA

`LLVMLifter::runGuestMemorySSA()` 是一个显式调用的、保守的 LLVM 内存值转发组件。
它可先运行 mem2reg，再识别 Static/V2 函数中的 Guest memory base。事实记录精确的
地址、字节宽度和 SSA 值，只有证明安全时才改写 LOAD；默认 `optimize()` 顺序不变。

当前实现覆盖两层，并消费一类明确的调用效果：

- 同一成功检查链上的精确 STORE→LOAD 转发，以及没有中间写入的 LOAD 复用；
- 无回边的 CFG 边传播。汇合点要求每条有效入边都提供同一地址和宽度的事实；值相同
  时直接保留，值不同则在汇合块入口插入 LLVM PHI，再转发后续 LOAD。
- 小端常量 GEP 的字节级重建。已知 STORE 覆盖 LOAD 的全部字节时，按真实字节序拼回
  整数；只覆盖一部分的读仍保留原 LOAD。

没有 `readnone`/`readonly` 等 LLVM 内存效果证明的调用、未知别名、易失/原子访问和部分重叠写
都是保守屏障；带这些属性的调用只读或不访问内存，可以保留事实。自然循环只有在入口事实与所有
回边事实逐项相同后才允许把不变事实传出；循环内 LOAD 的替换会延迟到该检查通过之后。
对单一 header、单一 body、单一回边的规范两块循环，如果地址/宽度集合一致且回边值来自循环
外或常量，可在 header 建立 loop-carried PHI；其他形状仍清空相关事实。值变化、缺失事实、
宽度/地址冲突和无法证明的循环都不猜测一般循环不动点。`maxRewrites` 到达上限时返回
`complete=false`，不伪造收敛报告。

`MemoryForwardOptions` 的 `enableCrossBlock` 和 `enablePhi` 允许 C++ 调用者分别关闭
跨块传播或冲突值 PHI；C shim/Python 使用完整的默认 S2 行为，并返回
`phis_inserted`、`join_barriers`、`loop_barriers` 和 `byte_rebuilds` 统计。

## 已证明的内部调用（可选）

需要把静态内部调用当作内存 SSA 中的无写入边界时，C++ 调用者可以传入同一份
`Graph` 并设置 `MemoryForwardOptions::proveLeafCalls=true`，再调用
`LLVMLifter::runGuestMemorySSA(graph, options)` 或三参数的 detail 函数。静态提升器只给
能确定数值目标的内部调用附加 `dragon.guest.call.target` 元数据；摘要按目标地址查找，
不读取函数名、导入名或 LLVM linkage。

当前消费契约故意很窄：只有 `LeafCallProof::proven` 且没有任何
`stackWriteBytes` 或 `memoryConstants` 的证明，才允许 Guest facts 穿过调用。堆栈写集、
全局/堆常量、未知目标、超预算和其它失败证明继续作为 CALL barrier；因此不会把尚未建立
Guest 地址关系的摘要误当作无别名证明。`proveLeafCalls` 默认为关闭，原来的两参数
`detail::runGuestMemorySSA` 符号也保留，C shim/Python 的默认行为不变。

## 显式调用效果摘要（可选）

`dragon/lifters/llvm/CallEffectSummary.hpp` 提供 `attachCallEffectSummary()`，供可信的
C++ Pass 给单个 LLVM CALL 附加版本化摘要。摘要可以标记外部函数或 syscall、syscall number、
内存效果和有限的返回寄存器/errno 信息；`proven`、边界类型、范围和数量都会在写入 metadata
前检查。`MemoryForwardOptions::consumeCallEffectSummaries` 默认关闭，开启后只消费已证明的
`NoMemory`/`ReadOnly` 摘要；未知、畸形、带写集或 `KnownRanges` 尚未建立别名证明的摘要仍是
CALL barrier。syscall number `0` 是合法值，不能被当成缺失。

Guest 地址键现在按 LLVM `DataLayout` 累积嵌套/typed GEP 的真实字节偏移，并识别两臂相同的
PHI/select 地址；动态 GEP 只按精确表达式身份匹配，未知别名继续清空事实。

## 验证

S2 新增菱形、循环不变槽、规范循环 PHI 和完全覆盖窄读回归；旧实现对窄读没有转发。
S2/S3 聚焦组为 **9/9 测试、62 条断言**；主 CTest **836/836**，无 Z3 CTest **701/701**。
规范循环 PHI 阶段的已验证基线为 834/834 和 699/699。
Python shim 也返回 `available=True`、`changed=True`、`loads_forwarded=2`，并可读到三个
新增统计字段。

证据和可复现 patch 位于
`validation-artifacts/llvm-memory-forwarding-20261110/s2-bytes/`；VM 工作树使用
`~/dragon-tales-overlap`。这些结果证明模块生成、验证和本组件的局部语义，不构成一般堆
分析、未知外部调用摘要、字节级重组或完整循环内存分析的证明。

S3 调用效果证据位于 `validation-artifacts/llvm-memory-forwarding-20261110/s3-call-effects/`。
