# 复杂混合混淆验证记录

日期：2026-11-10。所有分析在 Ubuntu VM 的 `~/dragon-tales-overlap` 上执行，Dragon
只做静态反汇编、IGNIL/LLVM 提升和 Pass；原始程序、Tigress 程序和 Unicorn 只用于独立
语义对照。

## 综合大函数

综合 `kernel` 同时包含循环、嵌套分支、函数指针表、Guest 内存读写、MBA、opaque 谓词、
Flatten、间接分发、递归 helper、`memcpy`/`memset` 和 64 位乘法。使用 Tigress 2.1
revision 2128，seed `20261110`，8 个边界输入，O0/O2，两种反汇编策略，四种组合：
`f-mba`、`f-opaque`、`f-cff`、`f-combo`。

两种策略各 32 个 case：

- C/Unicorn 对照 `64/64` 匹配；
- CFG 错误目标 `0`，诊断完整；
- LLVM `product-O2` 的 `64/64` 均为 `incomplete`，没有错误恢复；
- `f-combo-O0` 最大约 `24675` 条机器指令，仍能完整记录执行到的状态；
- `f-combo` 的未解码 PC 和未知分发边被保留为缺口，不算成功。

随后对同一批 16 个 ELF 显式运行
`mem2reg → instcombine → gvn → mba → predicates → dispatch-specialize →
dispatch-deflatten → control-flow → dce`，16/16 个 LLVM pipeline 报告和 IR verify
通过。MBA/谓词阶段在可证明位置发生改写，例如 `f-mba-O0` 的 IR 行数由 `14021` 降到
`3094`，`f-combo-O2` 由 `4109` 降到 `1775`。这些复杂样本的 dispatcher 状态仍然是
未知或多值，因此两个分发 Pass 都报告 unresolved、改写边为零；这是正确的保守结果。

同一批 16 个 ELF 单独运行 Guest memory SSA，16/16 次分析完成且没有错误改写；复杂
函数中的动态指针和调用使其报告 `alias_barriers`，而不是猜测 Guest 槽关系。

基础函数语料另用 seed `1901/1902` 测试 arithmetic 和 flatten-switch，线性/递归各一组：
每组 `128` 次 C 对照全部匹配，`768` 次 LLVM 比较中 `384` 次完整匹配、`384` 次明确
`incomplete`，`different=0`。

独立 MBA 语料 `144` modules/`1152` 次 native trials 全部匹配，条件谓词语料 `24`
modules/`192` 次全部匹配。Tigress 的 opaque 初始化改用受控 list/arena 变体后，opaque
`192/192` 匹配，combined `192/192` 保守 `incomplete`。原始 array opaque 变体有 Tigress
2.1 自身的零除数生成缺陷，触发 Unicorn `idiv` 异常的样本被单独记录，未计入 Dragon 失败。

## 外部函数和未知调用

真实 PLT/GOT 外部函数语料包含 writer、只读返回、函数指针、callback、varargs、浮点、
tail call 和 noreturn API。plain O2/PLT 共 `480` 次静态执行：

- `352` 次精确 API 边界匹配；
- `128` 次完整 root 返回匹配；
- Host 对照 `224/224`，C 对照 `128/128`；
- 错误内存属性、错误入口、unlifted 和错误导入来源均为 `0`。

Flatten 后的外部语料 `64/64` 次执行都在到达 API 前停在未解决分发，统一记为控制流
`incomplete`，不把它误报成未知调用处理成功。

更大的 PLT/GOT 外部矩阵共 `2304` 次执行，`1472` 次 API 边界匹配、`832` 次完整返回，
不同、错误和未执行均为 `0`。外部 API 与 Flatten 的专项 `512/512` 次均为保守
`incomplete`；其中 `4` 个导入 provenance 缺口属于尚未到达导入边界的控制流覆盖问题。

专项回归覆盖：未知外部 writer、显式只读外部调用、未知间接调用、未知 syscall 原型，
共 `4` cases/`16` assertions 全通过。结论是：

- 未知调用清空受影响的 Guest-memory facts，并保留 CALL barrier；
- 只有显式只读属性或已证明摘要允许跨调用转发；
- 未知 syscall 不执行内核模型，分析精确停在系统转换前边界。

## syscall 边界

`x86 int 0x80`、`sysenter`、`syscall`，以及 AArch64 `svc`、`hvc`、`smc`、`eret` 等
系统转换共 `128` 次边界对照：完整 Guest 状态、PC/SP、内存和 7 个 Exit 字段全部匹配，
`actual_root_returns_observed=0`，说明没有错误地把内核效果当成普通函数返回。

当前可证明的通用处理是“保留边界并报告 incomplete”。要跨过已知 syscall，需要增加可选
的效果摘要，键应包含架构、OS/ABI 和 syscall number，至少描述返回寄存器、读写 buffer、
errno/flags、可能阻塞或异常；参数未知、指针可能别名、版本不明或效果不完整时仍回到
barrier。不能按导入名或一次运行结果猜测系统调用效果。

## 产物

- 综合函数：`/home/cc/vmc-runs/complex-tigress-20261110/`
- 显式 Dragon Pass pipeline：`/home/cc/vmc-runs/custom-pipeline-20261110/`
- 基础 arithmetic/Flatten：`/home/cc/vmc-runs/obf-focus-20261110/`
- 外部调用：`/home/cc/vmc-runs/unknown-calls-20261110/` 和
  `/home/cc/vmc-runs/external-unknown-20261110/plain/`
- syscall 边界：`/home/cc/vmc-runs/system-transition-unknown-20261110/`

这些结果证明了静态分析的保守边界和已支持能力，不表示堆链表、VM、自修改代码或任意
非线性 MBA 已经通用解决。
