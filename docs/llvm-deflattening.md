# 可证明的 LLVM 去平坦化

`dispatch-deflatten` 是一个显式选择的 LLVM Pass。它识别只含 PHI 和 switch 的分发块，
要求 switch selector 是该块的状态 PHI，并对每条入边证明状态值为一个有限的常量集合。
当集合中的所有值按照 LLVM switch 的完整语义（显式 case 或 default）都映射到同一个目标块
时，Pass 把原入边直接改到该目标块；集合超出 `max_values` 或映射到多个目标时保留原边。

改写有严格边界：未知或不同宽度的状态值、重复入边、分发块中的其它指令、目标块中的 PHI、
预算耗尽以及无法证明的表达式都会保留原分发 CFG。目标块暂不接受 PHI，是因为新增前驱时
必须同时构造对应的 SSA incoming value；拒绝比猜测安全。该 Pass 不读取镜像、函数名或
Tigress/OLLVM 特征，也不宣称能处理堆状态、间接内存读取或 VM 分发器。

它和 `dispatch-specialize` 的关系是：后者折叠当前 IR 中唯一常量的 branch/switch selector；
前者沿着状态 PHI 的每条已证明入边把“块 → 分发器 → 块”改成直连边。两者都默认关闭，
可以在提升、`mem2reg` 和常量传播之后显式加入流水线。Pass 报告会区分访问、改写边数和
未证明的分发器；成功只表示变换完成，不表示整个函数已去平坦化。
