# 静态 LLVM 模式实施记录

契约：[llvm-static-analysis-contract.md](llvm-static-analysis-contract.md)。
仓库所有者于 2026-11-03 同意新增 mode=2、`dragon_static_*` 和保守外部声明。
各步独立分支、提交及合入 main；以下区分静态 IR 检查和验证时的原生执行。

## 第 1 步：显式静态模式与未知调用

增加 C++ `StaticAnalysis`、C `DRAGON_LLVM_STATIC_ANALYSIS` 和 Python
`STATIC_ANALYSIS`（均为 2），以及四指针 `dragon_static_<arch>_<addr>`。
当前所有 CALL 都通过未实现的 `dragon_static_external` 声明。现有 Guest CALL/RET、
内存范围检查和寄存器别名逻辑复用；未知指令和跨界分支仍为显式退出。
调用前同步状态，调用后重载全部使用到的寄存器视图、检查 Exit 和实际 PC，
仅正常返回到确切后继时继续；其他结果传播，非法 Exit 明确拒绝。
静态代码不消耗预算。Legacy/V2 和 IGNIL 抽象保持现状。

### 新增失败用例和修复前结果

测试使用数值 mode=2，故能在旧版本编译运行，不依赖尚不存在的枚举名。

| 检查 | 旧版本结果 |
|---|---|
| C++ 静态模式与 B 条件跨界回归 | 2/2 测试失败；4 个断言中 2 失败 |
| C 模式选择 | 1/1 测试失败；3 个断言中 1 失败 |
| Python 模式选择 | `ValueError: invalid LLVM execution mode` |
| 首批 12 个调用夹具、O0/O2 | 24/24 编译阶段拒绝 mode=2，尚无语义比较 |

原始失败输出保存在 VM `validation-results/static-step1-before-{cpp,c,python}.log`，
以及 `static-step1-before-native/`。例如：

```text
REQUIRE( lifter.setExecutionMode(static_cast<ExecutionMode>(2)) )
with expansion: false
ValueError: invalid LLVM execution mode
```

另有一次初版测试代码将花括号直接传给 span 参数，编译失败；改为显式 vector 后，
才取得上述真实旧版本模式拒绝结果，不把编译错误算成功的失败验证。

### 验证结果

- C++：2 测试、66 断言通过，覆盖三架构未知声明和 B；C：1 测试、3 断言通过；
  Python：三架构镜像通过。
- 完整 CTest **474/474** 通过（包含已有 V2 函数级对拍、toy-vm 等）。
- 新增 `function_static_diff.py`：17 个夹具 × 8 边界输入 × O0/O2 = **272 比较**；
  全状态与完整 64 KiB 内存对比，无 Legacy 归一化。覆盖 A、B、参数、非 ABI GPR、
  Flags、向量、raw x87、内存读写、栈，以及基本分支/循环。
  原生验证仅在链接阶段提供独立 C 桩；Unicorn 独立执行其机器字节。
  opt 阶段无函数体，验证程序无 libdragon 运行时依赖，预算为 0 且保持 0。
- A 输出内存刻意损坏：**4/4 different**，脚本返回 1，证明完整比较生效。
- 跳过 0。只排除沿用夹具手册规定的 AF 和 FSTP C0/C2/C3（bits 8/10/14）；
  不排除 C1、TOP 或异常位。未修改任何指令 lift，未 lift 统计本步未重跑。

原始 IR、优化 IR、驱动、原生产物、每项寄存器/内存差异及 JSON 位于 VM
`~/dt-agent-all/validation-results/static-step1-final-all/`；负对照为
`static-step1-negative/`，CTest 日志为 `static-step1-ctest.log`。
B 在 Unicorn 到达外部目标前停止；静态 IR 报告已提交的 Transfer，V2 运行时才将
没有实现的 Transfer 改报 NeedTarget。这是两个协议的观测出口差别，PC、SP、Flags、
寄存器和内存仍逐项比较，不对状态作归一化。
`call_resume` 在此验证外部声明最终链接后运行同一段机器代码，不执行 V2 专属的补注册恢复流程。

本步尚未覆盖：直接内部函数声明/定义及处理顺序、三架构完整静态函数对拍、非法 Exit
和非正常返回的执行回归、两套真实语料的静态模式与优化后验证、独立真实块和跳转表回归。
这些留在第 2/3 步，不能由 66 个 C++ 断言或旧 V2 测试替代。

## 第 2 步：内部函数和其余调用出口

直接常量目标为已知内部入口时，生成对应 `dragon_static_*` 四指针调用。
允许先声明后定义，或先定义后调用；重复定义继续拒绝。如果被调用体 lift 失败，
保留被其他函数使用的原声明，清除失败的函数体，不产生悬空调用。
没有已知入口的目标和间接目标继续走保守外部声明，导入元数据沿用原机制，
不把符号名变成宿主 API 调用或 ABI 保证。

新增两个 C++ 用例在第 1 步代码上都失败：内部声明为空。
原始 `static-step2-before-cpp.log` 为 4 测试中 2 失败、74 断言中 2 失败。
修复后 C++ 4 测试、95 断言通过；CTest **478/478** 通过。
用于构造非法被调体的测试最初误用 `Graph::addIGNILBlock`，其 emplace 不替换现有块；
改为明确替换测试块后，才实际覆盖失败体清理分支，没有更改产品 Graph 行为。

新增 `function_static_guest_diff.py` 复用独立三架构 Unicorn oracle：

| 检查 | 数字 |
|---|---|
| 91 夹具 × 8 输入 × O0/O2 | **1456 match，0 different/error，0 skip** |
| 正常 Return / 显式 Transfer / NeedTarget / Unsupported | 900 / 220 / 96 / 240 |
| 寄存器字段 / 内存字节 | 105920 / 95420416 |
| 手册未定义标志字段排除 | 64（AF），无新增排除 |

含原 85 个夹具及三架构各两种内部处理顺序。原始/优化 IR 都验证；内部顺序夹具
额外要求存在直接 `dragon_static_*` 调用。覆盖普通/word CALL/RET、栈目标、递归、
循环、向量、Flags、间接目标 0、PE/ELF 导入来源、认证/far 指令拒绝和地址回绕。
改写返回地址、尾调用、导入跳板和函数外分支在**预先声明的静态出口**停止，Unicorn
也在同一机器边界停止；不把 Transfer 或 Unsupported 算作完整执行成功。
V2 可继续分发这些出口，静态模式本步只保留其保守表示。没有状态归一化。

新增 `static_exit_contract.py` 的 18 类 Exit × 8 输入 × O0/O2：**288 状态比较通过**，
比较 21600 寄存器字段和 18874368 内存字节，0 skip；另有 **16/16** 非返回子进程
按预期 `_Exit(42)`，无后继输出。这 16 次不计入状态比较。
Unicorn 提供准确的 CALL 前缀/压栈状态，随后由独立 C 桩写出各协议状态；测试独立构造
预期快照，检查正常继续、重定向、全部退出类型、非法 reason/kind/valid 和 PC 镜像。
这是 Exit 协议检查，不冒充整段被调函数 ISA 对拍。

原始结果：VM `validation-results/static-step2-guests-final/`、
`static-step2-exits-final/`、`static-step2-ctest.log`。
生产库没有增加桩或执行库模型。剩余第 3 步：真实语料静态 IR 和优化后验证、
无 Z3 构建回归、真实块与跳转表等既有层回归，以及可实际使用的静态优化输出示例。
