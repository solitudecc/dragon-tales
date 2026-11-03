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
