# LLVM 静态反混淆：状态与调用契约

状态：**仓库所有者已确认（2026-11-03），三步均已完成（2026-11-04）**。
正式 lifter 支持静态模式、未知外部调用和内部函数声明/定义。
任务 8 的 StatefulV2 已完成，其协议保持现状。验证见
[静态模式实施记录](llvm-static-analysis-validation.md)。

## 目标和范围

把二进制转换成可优化的 LLVM IR，保留外部调用，再做常量传播、代数化简、
死代码和可证明分支的消除。分析与优化不执行目标程序，也不要求提供外部库函数体。
例如 `call unknown; test eax,eax` 必须保留调用后的两条可能路径；不能因为调用前
`eax=0` 就删除非零路径。与调用效果无关且可证明的冗余仍可优化。

未知调用默认可以改变所有已建模寄存器及 Guest 内存，因此依赖调用结果的条件可能
无法继续化简。后续可连接已 lift 的内部函数体或加入经过验证的效果摘要来提高精度。
导入名称本身不是效果摘要，不能据此假定返回值、无副作用或某套调用约定。

第一版交付静态 IR 及其优化结果。通用控制流反平坦化、虚拟机去虚拟化、恢复普通
C 原型、将优化结果写回原二进制，均需要额外分析或工程，不能算作本模式已实现的能力。

## 已确认的新增接口

| 位置 | 接口 |
|---|---|
| C++ | `ExecutionMode::StaticAnalysis = 2` |
| C | `DRAGON_LLVM_STATIC_ANALYSIS = 2` |
| Python | `LLVMExecutionMode.STATIC_ANALYSIS = 2` |
| 静态函数名称 | `dragon_static_<arch>_<hex>`，arch 为 `x86_32`、`x86_64` 或 `arm64` |
| 静态函数签名 | `void (ptr state, ptr memory, ptr runtime, ptr exit)` |
| 未知调用声明 | `declare void @dragon_static_external(ptr, ptr, ptr, ptr)` |

例如入口 `0x1000` 的 x64 函数为 `dragon_static_x86_64_1000`。
沿用现有模式选择接口；仍须在首次 lift 前选择，同一 module 不混用模式。
Legacy=0、StatefulV2=1 及默认值保持现状；`liftBlock` 继续只支持现有 trace 协议。

四个指针复用 [Execution.h](../include/dragon/lifters/llvm/Execution.h) 的布局及
[现有状态契约](llvm-execution-contract.md) 的缓冲区有效性、所有权、寄存器别名和对齐规则。
不新增 Exit reason，不改变 IGNIL 的 CALL/RET 抽象，不把启发式恢复的原型变成实际 LLVM 参数。

2026-11-05 新增显式入口策略：默认仍允许从登记 PC 恢复。静态模式首次 lift 前可选
`StaticEntryPolicy::FunctionStart`，生成独立的 `dragon_static_entry_*` 符号，只接受
对应函数起点；其他输入 PC 返回现有 Unsupported/Entry，保留 Guest 寄存器和内存。
内部跳转、循环及调用后续不受此入口检查限制。它不提供初始化或调用效果假设；
详见[限定入口分析](llvm-static-entry.md)。
`runtime` 在静态模式中保留作协议上下文；生成的 IR 不调用 `executeV2`，也不依靠分发预算推进分析。
**相同的四指针布局不代表静态函数可注册到 V2 执行器**；静态产物中的外部声明尚无执行实现。

## 调用与返回规则

1. 按现有 V2 的机器级规则先求 CALL 目标，再压入 x86 Guest 返回地址或更新 A64 X30。
   源目标不能因 SP/LR 的改变而重新求值。
2. 使用现有同步规则，将当前寄存器视图写入规范快照，PC 写成实际目标。
   初始化 Exit 为该次 `Transfer/Call`（包含 source、target 和 return_pc），避免读取旧 Exit。
3. 目标是已证实的内部函数入口时，调用对应四指针 `dragon_static_*` 函数。
   尚未 lift 的内部函数可先声明，后补定义；不能因处理顺序改变其签名。
   外部或间接未知目标调用 `dragon_static_external`。动态目标保存在 Guest PC 中，
   source/return_pc 保存在 Exit 中；绝不把 Guest 数值地址转成宿主函数指针。
4. 未知声明和调用点都不添加 `memory(none)`、只读、`willreturn`、`nounwind` 或
   “保持被调用者保存寄存器”等未经证明的保证。名称、槽位和调用位置只作诊断元数据。
   helper 表示一次未知被调过程的效果，可以不返回；如返回，必须提供有效状态和 Exit。
   其实现须遵守现有缓冲区和结构布局契约，不能破坏对象的生存期或改变快照映射。
5. helper 返回后，重新读取 Exit 和实际 Guest PC。只有 `Return/Return`、有效目标且
   目标等于本次 CALL 的确切后继地址时，进入调用后续块。其余结果作为显式退出传播；
   包括修改后的返回地址、Transfer、NeedTarget、Unknown、Unsupported 和 Budget。
   Exit 与实际 PC 不一致时输出 `Unsupported/InvalidExit`，不能假定正常返回。
6. 进入后续块时从快照重新加载全部使用到的寄存器视图，并清除调用前临时值。
   旧 alloca/SSA 值不能跨调用继续使用；GPR 子寄存器、Flags、向量和 x87 都适用。
   内存也不得沿用调用前的加载值。

内部函数体使用同样规则；优化器只有在看到实现并证明效果后才能缩小影响范围。
“未知声明”不需要实现就能 verify 和优化；只有执行或链接成完整可执行文件时才需要实现。
LLVM 缺省内存效果允许读写，依据是
[LLVM LangRef 的 memory 属性规则](https://llvm.org/docs/LangRef.html#function-attributes)。

JMP/CJMP 的本地边照常保留，跨界边和跳转表 default 使用现有显式退出，不能生成
代表“未知目标”的 `unreachable`。RET 保留现有 V2 的真实 Guest 出栈/X30 规则。
未知机器指令、far/system/authenticated 转移等现有 Unsupported 范围不在本模式中近似执行。
这会保留保守边界，不能声称已恢复所有跨函数路径。

## 提案阶段最小验证（历史记录，尚非正式降级验证）

脚本：[static_call_contract_probe.py](../scripts/validation/static_call_contract_probe.py)。
它手写 LLVM IR，不调用 lifter 生成 IR；函数内清零字段、调用一个只有声明的函数、
重新读取字段并据此决定是否写内存。verify 与 O2 优化阶段均不提供函数体。
之后才链接独立 C 桩，原生执行并对比 C 的逐字节非零判断。
脚本里的三个原始缓冲区参数仅用于此验证，不是上表的正式四指针签名。

覆盖 11 个字段：EAX、CF、R15、XMM15H、FPU0 raw80、FPSW、SP、PC、普通内存、
Exit.reason、Exit.next_pc；每项输入 0、1、全 1、最高位，分别测试 O0/O2。
这里测试的是原始位向量的副作用可见性，CF 等字段的任意位型不代表合法机器状态，
也不是浮点指令测试。没有新增跳过或掩码。

| 变体 | 原生比较 | 一致 | 抓到差异 | 跳过 |
|---|---:|---:|---:|---:|
| 正确声明，调用后重载 | 88 | 88 | 0 | 0 |
| 故意沿用清零的旧缓存 | 88 | 22 | 66 | 0 |
| 故意错误声明 `memory(none)` | 88 | 55 | 33 | 0 |

后两项是必须抓出错误的负对照，不能计入功能成功数。零输入不会暴露错误；
错误属性在此次 O0 测试中未被利用，44/44 一致，O2 为 11 一致、33 差异。
错误属性变体故意违反 LLVM 契约，仅展示错误保证如何损害优化结果，不作为合法语义。
脚本逐条检查每个变体、优化级别和边界输入的预期差异，不接受任意数量的失败。

33 份原始 IR 和 33 份 O2 IR 共 **66 次 verify 通过**。
O2 后正确变体的 **11/11** 个外部调用和 **11/11** 个条件分支保留；
两种错误变体共 **22/22** 个条件分支被折叠。提案时正式 API 对三种架构的 mode=2
均拒绝（3/3），确认新模式尚未实现。

环境：Ubuntu aarch64，LLVM 21.1.8，GCC 15.2.0；原始 IR、优化 IR、C 桩、原生文件和
逐项 JSON 保存在 VM `~/dt-agent-all/validation-results/static-contract-probe-final/`，
日志为同级 `static-contract-probe-final.log`。复现：

```sh
cd ~/dt-agent-all
TMPDIR="$HOME/dt-agent-all/validation-tmp" \
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
~/ucdev/bin/python3 scripts/validation/static_call_contract_probe.py \
  --out validation-results/static-contract-probe-final
```

这轮没有改生产代码，未重跑 CTest、Unicorn、真实块、语料库、未 lift 统计或跳转表验证；
上述结果只证明 LLVM 副作用契约，不能替代机器级 CALL/RET、控制流和真实样本验收。
脚本最初把错误属性的 O0 也预期为差异，因实际仅 O2 暴露错误而退出失败；
现按未优化/优化 IR 的行为分别逐条断言，保留全部 264 次比较，没有删输入或排除字段。

## 已确认的实施顺序

1. 加入显式静态模式和未知调用降级，先加入当前版本必失败的 C++/C/Python 回归；
   对 A/B 及调用后 GPR、Flags、向量、x87、内存变化做 O0/O2 函数级对拍。
2. 加入内部函数调用、未提供内部函数体、间接调用和导入元数据；覆盖正常返回、
   改写返回地址、不返回及跨界边。测试用桩只是独立参考夹具，不进入产品库。
3. 接入优化输出并做三架构/真实语料回归，分别汇报语义比较、合法性、无法化简及
   Unsupported 数量。不能把 verify 成功当成反混淆成功。

每一步独立分支、提交、合入 main 并报告数字。后续若要增加效果摘要、真实宿主 ABI
或普通 C 函数签名，再单独提交具体方案确认。
