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

## 验证

S2 新增菱形、循环不变槽、规范循环 PHI 和完全覆盖窄读回归；旧实现对窄读没有转发。
当前聚焦组为 **8/8 测试、58 条断言**；主 CTest **835/835**，无 Z3 CTest **700/700**。
规范循环 PHI 阶段的已验证基线为 834/834 和 699/699。
Python shim 也返回 `available=True`、`changed=True`、`loads_forwarded=2`，并可读到三个
新增统计字段。

证据和可复现 patch 位于
`validation-artifacts/llvm-memory-forwarding-20261110/s2-bytes/`；VM 工作树使用
`~/dragon-tales-overlap`。这些结果证明模块生成、验证和本组件的局部语义，不构成一般堆
分析、未知外部调用摘要、字节级重组或完整循环内存分析的证明。
