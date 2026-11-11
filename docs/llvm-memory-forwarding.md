# S2 Guest 内存 SSA

`LLVMLifter::runGuestMemorySSA()` 是一个显式调用的、保守的 LLVM 内存值转发组件。
它可先运行 mem2reg，再识别 Static/V2 函数中的 Guest memory base。事实记录精确的
地址、字节宽度和 SSA 值，只有证明安全时才改写 LOAD；默认 `optimize()` 顺序不变。

当前实现覆盖以下能力，并可消费明确的调用效果：

- 同一成功检查链上的精确 STORE→LOAD 转发，以及没有中间写入的 LOAD 复用；
- 无回边的 CFG 边传播。汇合点要求每条有效入边都提供同一地址和宽度的事实；值相同
  时直接保留，值不同则在汇合块入口插入 LLVM PHI，再转发后续 LOAD。
- 小端常量 GEP 的字节级重建。已知 STORE 覆盖 LOAD 的全部字节时，按真实字节序拼回
  整数；只覆盖一部分的读仍保留原 LOAD。
- 不可约循环中未被原调度器访问的可达块会从空事实开始补扫，不跨块导入/导出事实。
- 对只有一条已证明入边的共享检查路径，受限复制检查块，在副本中转发精确 STORE 值，
  保留其它入边、原 LOAD 和所有检查/失败出口，并修复外部 PHI 与业务 live-out。

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

按入边复制需要上述两个选项都启用；它在原事实传播完成后运行，不向旧循环算法导出
新不变量。地址只接受受限的 64 位 byte-GEP 和明确的逐边 SSA 等价，不假设镜像不变。
此路径有独立的代码增长和证明预算，并可能增加 LLVM 块与 PHI；触及上限报告
`complete=false`。建议在 mem2reg/InstCombine/GVN 后显式运行。当前合同和最新验证见
[按具体入边恢复 Guest 内存值](llvm-memory-checked-edges.md)，下文数字保留历史阶段。

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
前检查。`MemoryForwardOptions::consumeCallEffectSummaries` 默认关闭，开启后消费已证明的
`NoMemory`/`ReadOnly` 摘要，以及只含 `GuestMemory` 写范围的 `KnownRanges` 摘要。后者先
失效声明范围内的事实，再安装声明为 `complete`、不超过 64 位的 little-endian 常量写；不重叠
的 Guest 事实继续保留。`GuestState`、栈、绝对地址、不完整常量、畸形摘要和未知调用仍是
CALL barrier。syscall number `0` 是合法值，不能被当成缺失。

Guest 地址键现在按 LLVM `DataLayout` 累积嵌套/typed GEP 的真实字节偏移，并识别两臂相同的
PHI/select 地址；动态 GEP 只按精确表达式身份匹配，未知别名继续清空事实。

## 验证

S2 新增菱形、循环不变槽、规范循环 PHI 和完全覆盖窄读回归；旧实现对窄读没有转发。
跨调用摘要新增完整常量写、非重叠保留和不完整写屏障回归。GuestMemorySSA 聚焦组为
**13/13 测试、112 条断言**。原直接 STORE 阶段的基线为主 CTest **857/857**、
无 Z3 CTest **722/722**；同函数逐边恢复和入口隔离的新增验证另见下文。
Python shim 也返回 `available=True`、`changed=True`、`loads_forwarded=2`，并可读到三个
新增统计字段。

证据和可复现 patch 位于
`validation-artifacts/llvm-memory-forwarding-20261110/s2-bytes/`；VM 工作树使用
`~/dragon-tales-overlap`。这些结果证明模块生成、验证和本组件的局部语义，不构成一般堆
分析、未知外部调用摘要、字节级重组或完整循环内存分析的证明。

S3 调用效果证据位于 `validation-artifacts/llvm-memory-forwarding-20261110/s3-call-effects/`。
原通用去平坦化对调用摘要和调用点 STORE 的入口消费已隔离：调用后写入效果并不是
callee 入口事实，也不能证明 callee 内部某个后续 LOAD 的值。现有相关测试继续覆盖这个
拒绝边界。[同函数 CALL 回归](call-state-barriers.md)则验证未知 CALL 后重写状态、保留
未知边并独立恢复已证边；写后摘要在本内存组件中的合法消费仍然保留。

最终整合为主 CTest **867/867**、无 Z3 **732/732**；同函数分发/调用聚焦组均为
**20 cases / 403 assertions**，CALL 原生对照 **336/336**。完整的旧失败控制、效果范围和
未覆盖项见[本轮修补记录](same-function-dispatch-fixes-20261111.md)。
