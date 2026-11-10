# 可证明的 LLVM 去平坦化

`dispatch-deflatten` 是一个显式选择的 LLVM Pass。它识别只含 PHI/load 和 switch 的分发块，
支持状态 PHI，也支持每条前驱边在分发块前立即写入同一状态槽的 load selector，并对每条入边证明状态值为一个有限的常量集合。
当集合中的所有值按照 LLVM switch 的完整语义（显式 case 或 default）都映射到同一个目标块
时，Pass 把原入边直接改到该目标块；集合超出 `max_values` 或映射到多个目标时保留原边。

改写有严格边界：未知或不同宽度的状态值、重复入边、分发块中的其它指令、目标块中的 PHI、
预算耗尽以及无法证明的表达式都会保留原分发 CFG。目标块暂不接受 PHI，是因为新增前驱时
必须同时构造对应的 SSA incoming value；拒绝比猜测安全。未知别名、一般堆状态、无法证明的
状态写入和未知调用仍保留原 dispatcher。该 Pass 不读取镜像、函数名或 Tigress/OLLVM 特征，
也不宣称能处理间接内存读取或 VM 分发器。

分发块中向后续块传递业务值的 PHI 也必须保留：直接绕过该块会丢失对应入边的值，
即使 LLVM verify 恰好仍然通过，也可能算出错误结果。load-backed selector 只接受普通的
非 volatile、非 atomic LOAD。入边只改写普通 branch/switch；`indirectbr` 的目的列表
和实际计算地址不能通过修改其中一个来同时更新。

## 跨调用 GuestState 专门化

开启默认的 `consumeCallEffectSummaries` 后，Pass 还可以处理一类封闭的跨函数入口：目标函数必须
是本地链接、所有直接调用点都可枚举；每个调用点要么附带 `CallEffectSummary v2`，声明完整的
`KnownRanges` 写集并对与 dispatcher LOAD 完全重合的 `GuestState` 槽提供一个 `complete` 常量，
要么在调用点所在块中有一个可证明覆盖该槽的直接常量 STORE。所有调用点的常量必须映射到同一个
LLVM switch 目标，之后才把 callee 入口的 switch 改成直连。外部可见函数、未知/间接调用者、
不完整摘要、部分重叠写和映射到不同目标的调用点都保持 unresolved。

这是一种调用图闭合条件下的全局专门化，不是把一个调用点的动态值传播给共享 callee。需要不同
调用点分别进入不同目标时，应保留通用 callee，或由可信插件先生成调用点 clone；当前 Pass 不
自动 clone，也不推断函数原型、系统调用效果或堆对象内容。

它和 `dispatch-specialize` 的关系是：后者折叠当前 IR 中唯一常量的 branch/switch selector；
前者沿着状态 PHI 的每条已证明入边把“块 → 分发器 → 块”改成直连边。两者都默认关闭，
可以在提升、`mem2reg` 和常量传播之后显式加入流水线。Pass 报告会区分访问、改写边数和
未证明的分发器；成功只表示变换完成，不表示整个函数已去平坦化。
