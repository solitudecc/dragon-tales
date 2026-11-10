# S2 Guest 内存 SSA

`LLVMLifter::runGuestMemorySSA()` 是一个显式调用的、保守的 LLVM 内存值转发组件。
它可先运行 mem2reg，再识别 Static/V2 函数中的 Guest memory base。事实记录精确的
地址、字节宽度和 SSA 值，只有证明安全时才改写 LOAD；默认 `optimize()` 顺序不变。

当前实现覆盖两层：

- 同一成功检查链上的精确 STORE→LOAD 转发，以及没有中间写入的 LOAD 复用；
- 无回边的 CFG 边传播。汇合点要求每条有效入边都提供同一地址和宽度的事实；值相同
  时直接保留，值不同则在汇合块入口插入 LLVM PHI，再转发后续 LOAD。

调用、未知别名、易失/原子访问和部分重叠写都是保守屏障。自然循环只有在入口事实与所有
回边事实逐项相同后才允许把不变事实传出；循环内 LOAD 的替换会延迟到该检查通过之后。
值变化、缺失事实、宽度/地址冲突和无法证明的循环都清空相关事实，不猜测一般循环不动点。
`maxRewrites` 到达上限时返回 `complete=false`，不伪造收敛报告。

`MemoryForwardOptions` 的 `enableCrossBlock` 和 `enablePhi` 允许 C++ 调用者分别关闭
跨块传播或冲突值 PHI；C shim/Python 使用完整的默认 S2 行为，并返回
`phis_inserted`、`join_barriers`、`loop_barriers` 统计。

## 验证

S2 新增菱形和循环不变槽回归，旧实现分别保留汇合 LOAD 和循环 LOAD；旧代码的循环回归有
2 个关键断言失败。当前聚焦组为 **6/6 测试、45 条断言**；主 CTest **833/833**，无 Z3
CTest **698/698**。菱形阶段的已合并基线为 832/832 和 697/697。
Python shim 也返回 `available=True`、`changed=True`、`loads_forwarded=2`，并可读到三个
新增统计字段。

证据和可复现 patch 位于
`validation-artifacts/llvm-memory-forwarding-20261110/s2-joins/`；VM 工作树使用
`~/dragon-tales-overlap`。这些结果证明模块生成、验证和本组件的局部语义，不构成一般堆
分析、未知外部调用摘要、字节级重组或完整循环内存分析的证明。
