# 系统调用与异常切换边界

本修复的基线是 `d0fab8d`。它处理未知环境效果被当成“没有效果”的正确性问题，不新增
系统调用实现，不假定 OS、系统调用号、函数名、原型、调用约定或成功返回。

## 两类调用

普通外部 API 仍是 Guest CALL。Static IR 提交完整调用状态，经不透明
`dragon_static_external` 边界调用；正常返回需验证 Exit 和精确的返回 PC，再重新读取状态。
未证明的寄存器/内存事实不能跨调用传播。内部函数有机器码时可继续分析其机器状态效果，
这不要求先恢复 C 函数原型。

直接 `SYSCALL`、`SYSENTER`、`INT`、`SVC` 等是环境/异常切换。LLVM MC 的显式/隐式寄存器
定义和 mayLoad/mayStore 不能描述未知 OS 处理器的全部效果。旧版 x64 SYSCALL 和 A64 SVC
没有触发 fallback 的 unlifted 条件，会只留下后面的 RET。A64 ERET/DRPS 还可能误入普通 RET
分派，不能用默认 X30 来表示异常返回。

## 显式停止合同

IGNIL 末尾追加 opcode `UNSUPPORTED = 72`，C/Python 同值，原编号不变。它没有 dst/src/aux，
表示“在这条机器指令执行前停止”；没有执行后状态，因此不为其伪造输出 UNDEF。
普通已执行指令的不确定值仍使用原 UNDEF/partial-unlifted 合同。

- 架构入口先识别环境切换，再做普通 CALL/RET/branch 分派；同时保留 unlifted marker。
  块/trace 提升在该处结束，不补一个假的 fall-through IGNIL JMP。
- SymbolicExecutor 的 Block、op-vector 和单步接口都识别该边界；即使允许 trace 跨普通
  控制转移，也返回 UnsupportedOp。run 返回 Error 和来源 PC，状态保留已执行前缀。
- Simplify/DSE 对含该边界的块保持原样，避免删除边界可观察的前缀写入。
- 切片及有限值查询不能把后缀常量、直接 JMP 字面量或入口内存事实当成已经得到的值。
  手工 IGNIL 未加 marker 时同样受保护。
- **Graph 的真实入边仍保留**，有限值向后继传递 Unknown。未知系统处理器可能返回；
  不能把“模型在此停止”当成“机器路径不可能到达”，否则 diamond 的另一路常量会错误
  冒充完整目标集合。独立入口继续使用原入口规则。
- Legacy 的 ABI 无法表达该边界，拒绝提升且不改模块；Static/V2 返回七字段
  `Unsupported/None/source_pc=PC/next_pc=PC/valid=1/return_pc=0/Instruction`。
  它是明确的不完整边界，不计为完整根返回或去混淆成功。

识别覆盖 x86 SYSCALL/SYSENTER、SYSRET/SYSEXIT、INT/INT3/INTO、IRET 及已解码的
环境切换形式，A64 SVC/HVC/SMC、ERET 系列和 DRPS；真实 trap 也使用该显式停止。
HLT 另按 opcode 识别，因为当前 LLVM 没有把它标成 trap。普通 CALL/RET、NOP 和已支持的
fence/hint 保持原行为。ARM32/Thumb 的 SVC/HVC/SMC 先经过相同 gate；这不表示新增了
一般 A32 数据语义或 A32 LLVM 支持。

LLVM 21 的当前解码器不接受 x86 INT1（F1），x64 INTO 也没有被本轮测试算作已解码覆盖。
gate 的名字注册不是对所有 ISA 扩展、未解码机器字节或任意特权指令的完整性证明。

## 独立参考边界

Unicorn 不提供真实 OS 服务；本轮不让它充当系统调用返回语义的参考。code hook 在真实
系统指令 PC 执行前停，记录前缀全部模型寄存器、PC/SP、64 KiB 内存和七个 Exit 字段，
再与 LLVM 生成代码比较。只执行生成的 host 函数，不执行原 Guest 系统指令。
返回值和内存单字段损坏对照必须都能检出。

架构依据：[Intel 指令集手册](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)、
[Arm 异常模型](https://support.arm.com/documentation/102412/0103/Exception-types/Synchronous-exceptions)。
这些手册定义了异常切换，不提供未知 OS 处理器的效果摘要。

## 组件回归

最终 13 项 C++ 回归使用旧公开 API 和数值 72，可在冻结的 `d0fab8d` 头文件/主库上真正
编译运行：12 项失败、724 条失败断言；不是把新 API 编译缺失算作语义失败。修复版主构建
13 项 / 2424 条断言通过，无 Z3 为 10 项 / 1736 条断言通过；完整 CTest 为
827/827、692/692。永久 validator 的 31 项元数据负对照也通过，包含缺失/重复行、布尔/浮点
编号、七个 Exit 字段、ARM X0 和恰好一个内存字节；这些纯自检不计为 CPU 执行次数。

24 个已解码的机器形态覆盖系统入口、异常返回和真实 trap；普通 CALL/RET、NOP、PAUSE 和
fence 是负控。另测手工无 marker 边界、格式错误时模块零修改、import 后缀不标注、常量 JMP/
查询/入口内存不穿越、diamond 的未知入边，以及 Block/vector/trace/step 停止与前缀内存。
没有为通过测试放宽原有 marker-only LOAD 合同。

旧版独立探针：SYSCALL 与 SVC 各两种执行模式、八个初态，共 32 次越过声明的前指令边界
到达 RET；原 INT80 对照的 16 次则精确匹配边界。48 次 Unicorn 均由 code hook 在系统指令
执行前停止，实际执行的系统指令为 0。这验证模型停止合同，不推断任意 OS 服务的执行结果。
原始旧探针与主/无 Z3 最终验证统一保存在
`~/vmc-runs/system-transition-boundaries-20261110/`。

本次不改变已支持普通指令的算术、SIMD 或浮点语义，没有新增全指令差分活动；对发生改变的
系统/异常指令，参考是上述完整前缀边界，不是不存在的 Unicorn OS 服务模型。

## 完整 Guest 对拍

`system_transition_diff.py` 使用 8 个永久夹具：i386 INT80/SYSENTER、x64 SYSCALL/HLT、
A64 SVC/HVC/SMC/ERET。每个夹具在边界前真实执行一次寄存器写入和一次 Guest STORE，
边界后另有写寄存器、STORE、RET，后缀必须不执行。code hook 的准确轨迹、参考快照、实际
native 快照、七字段 Exit 及生成对象的反汇编均保留。

主/无 Z3 各三模式（V2、Static、product）×两策略×O0/O2×八输入：每配置 768，合计
**1536 次完整状态边界匹配**。它们全部是明确的 incomplete，完整根返回为 0。主构建
product 的结果/内存负控各 128 次，全部检出指定的单字段差异（A64 是 X0，内存恰好一字节），
没有排除、跳过或归一化。1792 次正常/控制观察合计比较 148736 个模型字段和 117440512
字节内存；控制原始 native 快照保持不变，损坏只发生在比较副本。

两份原冻结 Tigress O0 ELF（f-mba/f-combo）、线性策略、product O0/O2、八输入：主/无 Z3
各 C16、LLVM32 完整根返回匹配，共 C32、LLVM64。仍执行真实 Guest megaInit，没有向分析
注入初始化后的堆快照；主构建诊断完整，无 Z3 的诊断不可用按原合同报告。这是已有函数执行
兼容验证，不是新去混淆收益，也不覆盖所有 Tigress 配置。

最终库/SDK 固定入口为 `final-runtime/freeze.json`；本次所有实际编译源码与本地源码的
274 项哈希已核对一致。实际构建基线是 `d0fab8d + final-fix.patch`，随后 Git 提交不算再次
构建。两套完整语料的最终验收另记，不把尚在运行的活动计为通过。
