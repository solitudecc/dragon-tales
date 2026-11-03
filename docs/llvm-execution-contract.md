# LLVM 函数执行契约（任务 8）

状态：2026-11-03 所有者要求继续下一步，按已提交方案开始实施。第 2 步只兑现基础 V2
状态/分发出口，CALL/RET 机器效果仍明确 Unsupported；见本文第 9 节及分步验证记录。
第 1 步基线为 `0aa4b73`，当时只增加文档和测试，没有修改公共接口或降级实现。
本文描述六步完成后的目标。中间提交只宣布已经兑现的能力；尚未实现的机器效果必须明确
拒绝/退出 Unsupported，阶段测试须说明停止边界或使用了哪些既有抽象兼容项，不能冒充
最终 V2 全函数执行通过。

后续步骤以[六步计划](llvm-control-flow.md)为准，每步独立分支、提交、合入并汇报。

## 1. 要解决的问题及当前接口

当前 `LLVMLifter::lift()` 生成 `void sub_<地址>(ptr mem, ptr regs)`，入口从寄存器字节区
加载局部 alloca，在 RET 写回；直接 CALL 却生成 `void fn_<地址>(ptr mem)`，调用前后没有
寄存器同步。跨函数 JMP/CJMP 和 switch default 可以进入 `unreachable`。

这些是本次需要修复的问题，不是新增的刻意设计差异。`unreachable` 只能用于已经证明不可能的
路径，不能表示未知目标、缺少实现、超出函数范围或运行预算耗尽。

## 2. 执行方案（2026-11-03 起按此实施）

| 项目 | 建议 | 兼容边界 / 实施步骤 |
|---|---|---|
| 启用方式 | 增加显式的 `StatefulV2` LLVM 执行模式 | 默认保留旧接口可准确表达的子集；新选项及 C/Python 镜像在批准后实现 |
| LLVM 入口 | `void dragon_v2_<arch>_<地址>(state, memory, runtime, exit)`，四个指针参数 | 新模式专用名字与签名；原接口名字/参数保留；旧接口无法安全表达的调用或跨界退出明确拒绝 |
| 被调函数 | 所有被调的 lifted 函数使用相同 V2 签名 | 不再把 Guest 地址直接转换成任意宿主函数指针；第 2/3/5 步落实 |
| 未知外部调用 | 退出并请求实现；提供实现后才能继续 | 默认不假设 ABI，不自动恢复“callee-saved”寄存器，不把导入名当作宿主 libc 实现 |
| Guest CALL/RET | 在 V2 LLVM 层落实机器效果；Legacy IGNIL 的抽象保持 | 第 4 步；默认 trace/toy-vm 行为不自动切换 |
| A64 RET 的目标 | 建议通过 RET 的 aux 补充架构寄存器编号，仍不增加 src 操作数 | 建议 aux=0 表示旧的默认 X30，aux=1..32 表示 X0..X31；X31 解释为 XZR。这是元数据接口扩展，必须批准后再改 |

模式、符号名与逻辑类型按本表实施；具体 C 布局见第 9 节。后续若需要超出本文的接口或设计
变动，会再次先说明并等待同意。兼容策略：Legacy 接口继续支持准确可表达的函数/trace；发现 CALL 或无法表示的
跨界出口时，应返回明确的 lift 失败，不能继续生成已知误编译的 IR。toy-vm 若依赖这些情况，
必须先验证其实际使用方式，再选择明确的模型或获批准的迁移方案，不为保留测试而返回错误结果。

不提出隐式 ABI 优化模式：如将来需要 SysV/Win64 假设，必须单独 opt-in、单独验证。

## 3. GuestState 的布局与所有权

目标逻辑结构（第 1 步未发布 ABI；第 2 步的具体 C 布局见第 9 节）：

```text
GuestStateV2 = { abi_version, architecture, regs_size, regs_bytes }
GuestMemoryV2 = 执行期间稳定的 Guest 地址映射及其数据
ExecContextV2 = 目标注册表、外部模型、运行预算、执行器私有信息
ExecExitV2 = { reason, transfer_kind, source_pc,
               next_pc, next_pc_valid, return_pc, detail }
```

- 调用方持有并分配 state、memory、runtime、exit；生命周期覆盖整次执行和所有嵌套分发。
  被调方可修改 Guest 寄存器和映射内存，不可释放或移动其存储。
- 寄存器区与 Guest 内存是两个不同的对象，Guest 地址不能指向寄存器快照。
- **现有寄存器偏移不变**；大小根据 `registerFile()` 的最大 extent 检查，当前 x86 家族
  为 488 字节、AArch64 为 788 字节。保留字节不用于偷偷传递运行时控制信息。
- x86/i386：GPR 槽距 8；RIP 槽为 128；CF/PF/AF/ZF/SF/OF/DF 为 136..142；
  XMM 低/高半部分别为 143+8*i / 271+8*i；x87 为 400+10*i 的物理 80 位槽，
  TOP/TAG/CW/SW 为 480/482/484/486。i386 只解释有效的 32 位 GPR 和 8 个 XMM，
  64 位槽的高 32 位要求入口归零，不作为 i386 架构结果比较。
- A64：X0..X30 为 0..240，XZR=248、SP=256、PC=264；V0..V31 为 272+16*i；
  NZCV 的四个字节为 784..787。XZR 恒为 0。
- Flags 仍使用 0/1 的单字节表示；x86 的架构未定义结果不猜测其值。
  原始 80 位 x87 和 128 位 SIMD 不经过 double 或宿主 ABI 参数转换。
- 32 位写入零扩展、部分寄存器合并、A64 标量 V 高位清零、外部快照 alignment=1
  等既有规则继续有效。

## 4. PC、SP 和内存

PC 的唯一真值是寄存器区对应的 PC 槽，不再另设一个可独立更新的 state.pc。
`exit.next_pc` 只是退出记录；有效时必须等于已写回的 Guest PC。

- 在可以返回控制权的边界，PC 表示**下一条要执行的 Guest 指令**。
  `source_pc` 记录导致退出的机器指令地址；CALL 另记录 fall-through 为 `return_pc`。
- x86 RIP 相对寻址继续读取“本指令结束地址”，不能把函数起点或旧快照 PC 当成基址。
- SP 是 Guest 地址，只指向 Guest 内存。宿主的 C/LLVM 调用栈与 Guest 栈相互独立。
- 内存按已有的小端 LOAD/STORE 规则访问；同一个 Guest 地址在调用双方指向同一批字节。
  允许有效映射下的非对齐访问，不能添加没有依据的对齐、只读或 noalias 假设。
- 本任务不引入 MMU、OS 信号、中断、并发或自修改代码的仿真。有效映射与执行期间代码
  不变是本版执行前提；框架遇到越界、故障或预算耗尽必须报执行失败，不记为正常返回。
  未知映射/不支持的执行条件不得用 `unreachable` 或假定访问成功来表示。

## 5. 同步不变量

寄存器字节区是跨函数边界的唯一权威状态。函数内部允许缓存 alloca/SSA，但：

1. 调用、跨函数转移、返回或交还运行时前，写回所有已发生的 Guest 寄存器效果。
2. 被调方必须读到调用点状态，而非函数入口旧快照；参数包括 SP、Flags、SIMD 和 x87。
3. 继续执行调用后的代码前，重新读取被调方可能修改的全部寄存器，旧 SSA 值不得继续代表
   这些寄存器。默认未知调用可改变所有寄存器与可写内存。
4. 被调 V2 函数返回宿主只表示交还控制。仅当退出为 Guest Return 且 next_pc 等于调用
   的 fall-through 时，调用方才能刷新缓存后继续；其他退出原样传播，不能用旧 caller
   缓存再次写回并覆盖被调方状态。被改写的返回目标不能被忽略。
5. 同一 guest register 的不同 view 在同步前后保持字节一致。不能只刷新 RAX、只刷新
   caller-saved，或遗漏 80/128 位字段。
6. 对未知调用不附加未经证明的 `readnone`、`readonly`、`argmemonly`、`noalias` 等属性。
7. 暂无模型的 unlifted/opaque 效果必须显式拒绝或交还控制；不能把未知数据传播成 LLVM UB。
   在不支持指令前退出时，保留此前已完成指令的状态，并报告该指令地址。

## 6. 退出及跨函数分发

建议退出原因：

| reason | 含义 |
|---|---|
| Return | 已完成 Guest RET 的机器效果，next_pc 是其目标；不自动等同于返回宿主 |
| Transfer | 已确定目标，需要跳出当前编译区域；transfer_kind 区分 Call / Branch / Return |
| NeedTarget | 目标数值已知，但注册表中没有对应实现 |
| UnknownTarget | 目标值无法可靠确定；next_pc_valid=false，不捏造 0 地址 |
| Unsupported | 缺少指令或外部行为模型，不能安全继续 |
| Budget | 运行预算耗尽；不是完成，也不是不可达 |

- 建议预算单位为基本块入口/跨函数分发次数，预算检查发生在可恢复边界；宿主进程超时
  是测试执行失败，不等于一个已经实现的 Guest Budget 出口。
- 每种退出都先物化状态；成功与请求处理是不同结果，调用方不能把 NeedTarget 当作执行成功。
- UnknownTarget 在无法求值的控制指令执行前退出：PC 保留该指令地址，尚未提交该条 CALL
  的栈/LR 效果。它与已完成转移效果、只缺目标实现的 NeedTarget 不同；不能把未知目标写成 0。
- 外部 JMP 不自动认定为遵守 ABI 的尾调用；它也可能进入另一个函数内部或共享块。
  分发按 Guest 地址及有效入口映射进行，不强迫目标从某个函数起点重新执行。
- CJMP 的两个方向分别处理。一个方向跨界，不能删除另一个本地可达方向。
- 已解析 jump-table switch 的 default 仍走运行时分发/请求处理；分析列出的目标不意味着
  其他运行时值是 UB。
- 注册表把 Guest 地址映射到 V2 实现；内部函数、外部模型、间接目标遵守同一状态契约。
  `importSlots()` 的名称只作为标识/诊断元数据，不把 `puts` 等名字直接绑定成宿主 ABI 调用。
- 运行时可以采用分发循环。Guest Return 只有在目标是调用方指定的根退出地址时才结束整次
  根函数执行；其他目标仍需处理，不能任意丢弃返回目标。

## 7. CALL/RET 的机器效果与恢复点（第 4 步）

- CALL/BLR 的目标先从旧状态求值，再更新 SP/LR；例如 CALL RSP、CALL [RSP]、BLR X30
  不能读取已经更新过的 SP/X30 作为目标。
- x86-64 CALL：将 next instruction 的地址写入 Guest `[RSP-8]`，更新 RSP，再进入目标。
  i386 对应 4 字节和 ESP 地址宽度。RET 从 Guest 栈读取目标，弹出返回地址，并加 RET.aux
  的 imm16 字节数。不能只调整宿主栈，也不能忽略被改写的返回地址。
- A64 BL/BLR：X30=return_pc；RET 使用其指定寄存器，而非一律硬编码 X30。
- 若 Guest CALL 效果已完成而目标缺少实现，退出状态的 PC 已指向被调目标，Guest 栈/LR
  也已经更新；恢复时不能再次压入同一个返回地址。
- 本计划的 x86 范围为当前支持的 near CALL/RET；far CALL/RETF/IRET 等缺少状态模型时
  保持明确不支持。
- 原 IGNIL CALL/RET 抽象及其 SymbolicExecutor 行为保留；V2 所需的 return_pc、RET 源寄存器
  必须来自可靠的指令信息/批准的元数据，不能从下一块地址猜测。
- `liftBlock()` 默认仍是既有 trace 接口，toy-vm 不自动改为 V2。若要增加 V2 trace 入口，
  必须另列接口方案并先获批准。

## 8. 第 1 步测试框架的范围（不是 V2 实现）

`function_llvm_diff.py` 运行完整小函数：保留分支、回边、CALL 和函数外目标；同一输入在
Unicorn 和编译后的 LLVM 代码中执行。数据与栈均使用同一 Guest 地址的独立副本，外部目标
在两边采用同样的桩行为。用例 A、B 使用计划文档的原始字节。

本步 adapter 只适配现有两指针入口和单指针 `fn_<地址>`。Native 桩通过测试进程的
`active_regs` 指针写寄存器快照，这是暴露旧接口缺口的**测试桥**，不是新增生产 ABI。
待 V2 契约批准后再增加对应 adapter，不能在本步修改降级结果来“修好”用例。

当前比较包括 16 个 GPR、7 个建模 Flags 中定义明确的部分、全部 16 个 XMM、原始 x87
及控制状态、全部映射字节和桩调用次数。仅对已有 IGNIL 抽象作以下明确兼容：

- 不比较最终 RIP：旧 RET 无目标输出；仍要求 Unicorn 实际到达根退出哨兵。
- 仅抵消外层普通 RET 的 8 字节 RSP 弹栈，不掩盖其他 SP 变化。
- 仅排除 CALL 实际写入的 8 字节返回地址槽；其他内存全部比较。
- TEST/XOR 的 AF 按手册未定义排除；其他 Flags 不扩大排除范围。

这些兼容项逐次计数，不是 V2 允许忽略的效果；第 4 步 V2 对拍必须取消前三项并比较真实
PC/SP/返回地址写入。本版仅覆盖 x86-64、有限小函数和模拟桩；i386、A64、间接目标、未知
外部效果、嵌套调用、RET imm16、RET Xn 和真实函数语料会在对应后续步骤先加失败用例。

本版还未验证被调方读取 caller 刚写入的参数状态、非 ABI 寄存器破坏和向量/x87 返回值；
第 3 步必须先补相应失败用例。现阶段 SIMD/x87 只验证未触碰状态的保留。

本步只请求确认契约，不声称 A、B 已修复。严格对拍默认对现有 A/B 返回非零；正对照必须
通过，故意破坏 Native 输出的负对照必须检出。IR verify 通过不算语义通过。


## 9. 第 2 步已经实现的 V2 基础

公共类型的唯一来源是 `include/dragon/lifters/llvm/Execution.h`。C++ 别名和执行器在
`Execution.hpp`，C shim 包含同一 C 头文件，Python cffi 同时读取两份头文件。没有复制一套
可能偏移不一致的 C/Python 类型。当前 ABI 面向 64 位小端宿主：

| 类型 | 字段（按声明顺序） |
|---|---|
| GuestStateV2 | u64 abi_version=2, architecture（Architecture 的数值）, regs_size；u8* regs_bytes |
| GuestMemoryV2 | u8* data；u64 size；当前是 Guest 地址 0..size 的连续映射 |
| ExecContextV2 | TargetEntryV2* entries；u64 entry_count, budget；void* user_data |
| TargetEntryV2 | u64 guest_address；四指针 TargetV2 implementation |
| ExecExitV2 | 七个 u64：reason, transfer_kind, source_pc, next_pc, next_pc_valid, return_pc, detail |

退出原因和 transfer_kind 的具体数值以该头文件为准。return_pc 仅在已执行的 CALL
转移记录中有效；Unsupported CALL 尚未执行，不提供返回地址。C++ LLVMLifter 对象增加了
内部状态字段，C++ 使用者须以新头文件重新编译；原 C 工厂签名及不透明句柄接口保留。
每个已编译实现的架构必须与 state.architecture 及其来源 Graph 一致。调用方提供有效、互不重叠的状态/内存/
运行时对象；模型只修改 Guest 寄存器、映射数据和自己的 user_data，不修改状态/映射头、
注册表或增加运行预算。注册表中的每个地址须确实对应实现支持的入口；不存在的实现返回
NeedTarget，不猜测相邻函数入口。当前 lifted 实现支持该函数的起始地址；后续增加恢复入口
时会明确列出。V2 模块和 Legacy 模块不能混用。

C++ 通过 `setExecutionMode(ExecutionMode::StatefulV2)` 在首次 lift 前 opt-in，C 使用
`dragon_llvm_lifter_set_execution_mode`，Python 使用
`LLVMLifter(..., execution_mode=LLVMExecutionMode.STATEFUL_V2)`。设置非法模式或在非空
模块切换模式返回失败；旧构造方式及 Legacy LLVM 名字/签名保留。

`executeV2` / `dragon_llvm_execute_v2` 从寄存器区 PC 查注册表，调用统一的四指针实现。
每次分发、Guest 基本块入口和 IGNIL 局部循环入口各消耗一个预算单位；退出时 budget=0
明确报告 Budget。合法可执行路径不会因为没有模型而变成 `unreachable`。调用方可在
NeedTarget 后补充实现并以当前 PC 继续，不重新执行已完成的分支。

第 2 步的 CALL/RET 在该指令执行前退出 Unsupported；trap、unlifted/opaque 在最早缺少
语义的指令前停止；字节流结束则停在最后一条已完成指令之后。合法的架构未定义数据在
V2 使用独立 freeze 值，标志字节限制为 0/1，不把未知效果交给 LLVM UB。

映射边界检查使用完整访问宽度，且所有 Guest/快照访问 alignment=1。越界报告 Unsupported
加 Memory detail；由于本任务不模拟架构故障，不承诺该错误的精确异常状态，也不能将其
当作可恢复的 Guest fault。有效映射仍是执行前提。Unsupported 指令、NeedTarget 和 Budget
则按各自声明的停止边界保留状态。

`function_dispatch_diff.py` 将相同机器代码执行到这些明确边界，比较 PC/SP/所有已建模
寄存器/全部映射内存和退出记录；不使用第 1 步的 Legacy PC/SP/CALL 栈归一。统计名为
prefix_match，不能解释为完整 Guest CALL/RET 已实现或 A 已修复。
