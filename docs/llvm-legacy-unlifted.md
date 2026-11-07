# Legacy 整函数提升拒绝 unlifted

本阶段基线为 `a19af98`。默认 Legacy 接口此前可能把带 `unlifted` marker 的函数
生成为可执行 LLVM IR，甚至在不支持的 STORE 没有产生任何 IGNIL 操作时仍返回成功。
这会把“不知道该指令做了什么”变成“该指令没有效果”。

修复是在创建 LLVM 函数之前检查该函数的所有成员块：只要存在 marker，C++ 返回
`false`、C 返回 `0`、Python 返回 `False`，模块保持原样。此项是提升接受契约的
安全修正，不是恢复了该指令语义，也不计为去混淆成功。

同源测试已在旧实现上实际失败；修后主 CTest 653/653、无 Z3 521/521 通过，原生与
接受性独立审计完成。执行证据使用冻结的源码和候选快照；最终档案闭合与 Git 发布状态
另由外部 receipt 记录。

## 拒绝契约

`LLVMLifter::lift(Function, Graph)` 的 Legacy 预检同时要求：

- 函数有效、各成员块有 IGNIL；
- 每个成员块的 `unlifted()` 为空；
- 继续满足既有 Legacy CALL、动态/越界转移等拒绝规则。

marker 检查按函数成员块进行，不仅检查入口块，不依赖该指令是否有 IGNIL 操作，
也不把暂时看似不可达的成员块自动忽略。纯内存 STORE 可以没有任何寄存器定义和
IGNIL op；如果只扫描 op，就看不到丢失的内存效果。

该拒绝发生在 LLVM 函数创建、寄存器分配及相关模块写入之前。空模块不留下部分符号，
已有其他函数或同地址定义的模块也不变化；`ir()` 的字节内容保持一致。
调用者可在同一个 lifter 上处理后续合法函数，不应遭遇失败所留下的半成品或污染。
这里保证的是 **marker 预检拒绝** 的不变性，不把它扩写成所有未知失败路径均有事务回滚。

合法的 `UNDEF` 不是拒绝依据。例如受支持的 MUL 可以为架构未定义的标志产生 UNDEF，
而没有 unlifted marker；它仍可提升。NOOP 也可以零 op 而保持完整支持。不能用
“含 UNDEF”或“指令数大于 op 数”替代 marker 检查。

## 对调用者的影响

默认执行模式仍是 Legacy，公共函数签名、C/Python 返回类型、Guest ABI 和默认模式均
没有更改。行为变化是：以前可能生成部分 IR 的输入现在明确返回失败。调用者必须检查
返回值；模块里还能看到先前其他函数的 IR，不代表这次请求也被接受。

```python
with dragon.LLVMLifter(image.architecture) as lifter:
    if not lifter.lift(function, graph):
        raise RuntimeError("Legacy lift refused; inspect member-block unlifted markers")
    ir = lifter.ir()
```

StatefulV2 和 StaticAnalysis 没有因本修复改为拒绝整个标记函数。它们保留原有的显式
Unsupported 指令边界；提升成功或 `opt -passes=verify` 通过，均不表示未知指令已经实现，
也不表示能够从该边界继续到正常根返回。不能通过改用这些模式，把部分提升称为完整执行。

Legacy trace 接口此前已经拒绝 marker；本阶段补齐的是 Graph/Function 整函数入口。
Graph 恢复、反汇编、IGNIL 指令语义、切片、有限值证明和去混淆 pass 没有因此新增能力。

公开契约说明位于 C++ `LLVMLifter.hpp`、C `dragon_c.h` 和 Python `LLVMLifter.lift`
docstring；[LLVM 入门](manual/06-moving-to-llvm-ir.md)、
[真实程序示例](manual/07-a-real-world-example.md)和 [C API](c_api.md)也说明检查返回值。
真实程序一章保留的旧缺 STORE IR 被明确标为历史错误证据，不再建议忽略缺口继续使用。

## 回归与旧实现失败证据

同源测试先对冻结旧实现运行，前提及正确正例通过，结果如下：

| 入口 | 旧实现测试量 | 实际失败 |
|---|---:|---:|
| C++ | 7 用例 / 426 断言 | 4 个用例失败、70 个失败断言 |
| C | 1 用例 / 18 断言 | 4 个失败断言 |
| Python | 4 个 unittest 方法 | 16 次失败，来自 marker 拒绝及模块不变性场景 |

这些是旧实现的失败对照，不是修后通过统计。现有 API 足以编译/运行测试，没有以
“旧版缺新 API”代替行为反例；合法 UNDEF、正常函数及 V2/Static 接受正例在旧库也通过。

覆盖包括真实的 MOVBE 内存 STORE（该指令零 IGNIL op）、FS/GS 标量或 SSE 内存效果、
非入口成员块、零 op/空 IR 的 marker，以及同实例反复失败、已有定义和后续合法提升。
Python 同时核对 `False` 与直接 C 调用的 `0`，以文本 IR 的 UTF-8 字节验证不变；
它的 V2/Static 用例只验证接受契约，不能冒充原生运行边界对拍。
C++ 用例另外核对显式 Unsupported 的退出字段，不把边界写成 `unreachable`。

修后最终 C++ 7 用例 / 426 断言、C 1 用例 / 18 断言全部通过；Python 四个方法也随
完整 CTest 通过。主库 653/653、无 Z3 521/521 完整测试通过；冻结脚本的末次专项
复跑为 2/2，分别覆盖 inventory 的 11 份坏报告拒绝 / 1 份合法报告接受，以及 native
harness 的 9 项契约自检。前一阶段的 CTest、原生根返回、语料或 Tigress 数字均不借作
本阶段成功记录。

## 原生执行与边界验证

`native-final-audit.json` 独立闭合本阶段 22 个 job。不同接口的观察边界如下，不能合并
成一个“全部完整根返回”的数字：

| 验证范围 | 实际结果 | 计数单位 |
|---|---|---|
| 旧 Legacy | 128 次 CPU 观察：64 个真实 MOVBE 反例、64 个支持指令控制匹配 | 反例具有错误 EAX/RAX 和恰好 4 个 DATA 字节，保留非零退出，不能当生产通过 |
| 新 Legacy 接受契约 | 16 个模块：8 个 marker 拒绝且模块不变、8 个支持函数接受 | 拒绝按模块计，不乘 trial 数冒充 CPU 比较 |
| 新 Legacy 执行 | 64 次 pre-RET ABI 观察匹配 | 参考停在 RET 之前，PC 在 Legacy ABI 范围外；SP 及其余全部建模字段和 64 KiB 内存参与比较，没有 RET-pop 归一化 |
| 新主 V2 / Static Resumable / product FunctionStart | 每种 128 次：64 个完整根返回、64 个精确 Unsupported 边界 | 主库三类合计 384 次完整 Guest 状态对拍 |
| 无 Z3 product | 128 次：64 个完整根返回、64 个精确 Unsupported 边界 | 无 Z3 单类验证，不称全部模式均无 Z3 重跑 |

新库合计 512 次完整 Guest 状态对拍，其中 **256 个完整根返回、256 个指令前
Unsupported 边界**；加上 Legacy 的 64 次 ABI 观察，共 576 个正常匹配。
后一个总数没有统一的根返回语义。V2/Static 对拍保留全部建模寄存器、PC/SP、
64 KiB 内存及七字段退出协议；Unsupported 只验证该指令前状态已正确提交，不证明
后续未知效果或完整函数完成。旧库对应的三种非 Legacy 模式另有 384 次匹配，作为
本阶段实际执行的兼容对照。

结果、内存负对照各检出 96/96 个精确单字段差异，共 192 次，覆盖 Legacy 和 product
的各自观察路径。对照只改比较副本；原始 native snapshot 保持不变。Legacy 不比较 PC
是明确的 ABI 观察范围，不能写成“不排除任何字段的完整 Guest 根返回”。

非 Legacy 的 88 份 LLVM 文件也分开核验：64 份 `raw.ll` / `unoptimized.ll` 字节完全相同；
24 份 `optimized.ll` 只有第一行 ModuleID 注释中的 old/main 绝对输入路径不同。
独立审计要求该行恰好包含两个预期路径，并要求余下所有字节完全相同。不能声称
88 份文件逐字一致，也没有对 Guest 状态做归一化。
首次要求所有文件逐字相同的审计失败日志和 `native-run` 的 `complete=False` 原记录
均保留；后续 `native-final-audit.json` 按上述明确差异完成闭环，不覆盖早期记录。

## 有限真实输入的 Legacy 接受性

本阶段另对锁定的九个二进制、两种遍历策略采集旧/新主库报告：Lua i386/x64 PE、
Lua AArch64 ELF，以及六个冻结 Tigress ELF。每版 18 个图，全部候选函数均尝试提升，
没有只抽样前若干函数。

| 接受性结果 | 函数记录数 |
|---|---:|
| 候选总数 | 6245 = 6237 valid + 8 invalid |
| 共同接受且文本 IR 的 SHA-256 相同 | 755 |
| 旧接受、新因 marker 拒绝 | 67 |
| 旧新均已拒绝 | 5423，其中含 8 个 invalid |
| Legacy 接受总数（旧 → 新） | 822 → 755 |

这些是按 `(输入,策略,入口)` 的记录，不是 6245 个独立源函数。5423 个既有拒绝不能
全部归因于新增 marker 检查；没有更细诊断时不猜测其拒绝原因。

`legacy_unlifted_inventory.py` 只检查接受性和共同接受的文本 IR 身份，不做 LLVM verify
或 CPU 对拍。独立输入 manifest 固定九项；报告必须完整，精确覆盖每输入 × 两策略及
每图声明的 entry 集合，重复、遗漏和非法类型均拒绝。旧新分进程核对源码身份、
实际两 DSO 的完整路径/maps/SHA，并在输入处理后重核；11 份坏报告负对照均被检出。
图地址、计数、函数成员及 marker 字段用于覆盖核对，不应据此声称重新验证了完整
反汇编/CFG/IGNIL 全指纹或全程序语义。

本轮没有重跑上一阶段的 69 输入全语料 V2 验证，也没有重跑 Tigress CPU 矩阵：
生产改动仅位于 Legacy 整函数入口的 guard，不修改上游 Graph/IGNIL 语义；已有模式的
独立原生对照和有限接受性采集用于本阶段验证。九个输入中包含 Tigress ELF，不等于
重新完成了 Tigress 去混淆或 Guest 初始化/内存语义测试。

## 验证边界与后续工作

这项检查能阻止调用者执行已知不完整的 Legacy 函数，但不能发现 lifter 从未登记的漏标，
不能证明所有支持的指令语义正确，也不能把 IR 合法性当作 CPU 等价证明。
诚实登记 UNDEF/unlifted、独立参考差分及完整函数验证仍分别需要。

默认 Legacy 的接受集合会收窄，即使某个 marker 所在成员块在特定输入下没有执行，
也不能据此把整函数标为完整。若要保留部分路径分析，应使用明确消费 Unsupported 边界的
客户端，或先独立证明并安全移除相关路径；本阶段不提供这样的路径证明或自动清理。

表内容来源、alias/不可变证书、初始化堆分析、任意内部调用摘要、去平坦化及二进制重写
仍是独立工作。此拒绝修复不解决 image 跳转表可被 Guest STORE 改写的问题，也不恢复
此前 TLS marker 后撤回的目标集合。

只读调用点盘点发现 `scripts/validation/fuzz_loader.py` 的嵌入子进程仍忽略返回值。
该脚本统计的是抗崩溃尝试，`OK` 不表示所有请求已成功提升；本阶段没有把它的计数
改为语义成功指标，也没有擅自批改其鲁棒性策略。

阶段库快照、源码、独立审计、旧失败和首次精确 IR 审计记录保存在 VM
`~/vmc-runs/legacy-unlifted-20261107/`、Mac
`/Users/c/Desktop/deob/validation-artifacts/legacy-unlifted-20261107/`。
