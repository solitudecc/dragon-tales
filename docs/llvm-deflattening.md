# 可证明的 LLVM 去平坦化

`dispatch-deflatten` 是一个显式选择的 LLVM Pass。它识别只含 PHI/load 和 switch 的分发块，
支持状态 PHI，也支持每条前驱边在分发块前立即写入同一状态槽的 load selector，并对每条入边证明状态值为一个有限的常量集合。
当集合中的所有值按照 LLVM switch 的完整语义（显式 case 或 default）都映射到同一个目标块
时，Pass 把原入边直接改到该目标块；集合超出 `max_values` 或映射到多个目标时保留原边。
证明逐条入边消费：某条路径的 CALL 结果或 state 未知时，该边继续经过原分发器；其它
入边的完整证明仍可用于直连。报告中的 `dispatchers_rewritten` 和 `unresolved_dispatchers`
因此可以同时非零，表示部分入边已恢复，仍有未证明路径。

改写有严格边界：未知或不同宽度的状态值、重复入边、目标块中的 PHI 和无法证明的表达式
保留对应入边；不支持的分发块指令和结构预算超限则保留整个分发块。目标块暂不接受 PHI，是因为新增前驱时
必须同时构造对应的 SSA incoming value；拒绝比猜测安全。未知别名、一般堆状态、无法证明的
状态写入和未知调用仍保留原 dispatcher。该 Pass 不读取镜像、函数名或 Tigress/OLLVM 特征，
也不宣称能处理间接内存读取或 VM 分发器。

分发块中向后续块传递业务值的 PHI 也必须保留：直接绕过该块会丢失对应入边的值，
即使 LLVM verify 恰好仍然通过，也可能算出错误结果。load-backed selector 只接受普通的
非 volatile、非 atomic LOAD。入边只改写普通 branch/switch；`indirectbr` 的目的列表
和实际计算地址不能通过修改其中一个来同时更新。

## 调用效果与入口事实

本 Pass 聚焦同一函数的基本块。`CallEffectSummary` 描述调用正常返回后的读写效果，
不能直接证明 callee 入口或 callee 内部某个 dispatcher 的输入值。此前实验性的跨调用
入口消费还混淆了不同嵌套指针的地址空间，并没有追踪 callee 内部的覆盖写；该自动路径
已从通用 Pass 中移除。`GuestMemorySSA` 仍可在正常返回后消费合法的 GuestMemory 写后摘要。

为保持源码兼容，`consumeCallEffectSummaries` 与 `maxCrossCallSites` 字段暂时保留，
前者默认 false；显式置 true 也不会重新启用已移除的路径。三个 `cross_call_*` 报告计数
继续保留并为零。罕见的子函数改写 state 情况应通过单独 Pass 建立入口上下文、地址关系、
正常返回效果与覆盖写证明，再接入通用 SSA/去平坦化组件；原通用函数入口不得被猜测收窄。

它和 `dispatch-specialize` 的关系是：后者折叠当前 IR 中唯一常量的 branch/switch selector；
前者沿着状态 PHI 的每条已证明入边把“块 → 分发器 → 块”改成直连边。两者都默认关闭，
可以在提升、`mem2reg` 和常量传播之后显式加入流水线。Pass 报告会区分访问、改写边数和
未证明的分发器；成功只表示变换完成，不表示整个函数已去平坦化。
