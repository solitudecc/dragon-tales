# 如何开始检测函数分析语义：生成函数对拍

首轮完成：2026-11-04。工具：
[generated_function_diff.py](../scripts/validation/generated_function_diff.py)。
这轮只增加测试及 CTest 注册，没有改变生产语义、接口、IGNIL CALL/RET 或 LLVM 名字。

## 检测步骤

1. 固定随机种子，生成有限的 x86-64 函数：条件菱形、8/16/32/64 位寄存器别名、
   动态但最多 7 次的循环，以及会改写 R15/内存的被调函数。调用后的根函数消费其结果。
2. 每个函数生成原始版和插入单字节 NOP 的扰动版。分支标签、位移和预期 CFG 边由生成器记录；
   函数入口为已知的 0x1000，不用框架的识别结果反过来定义真值。
3. Unicorn 执行原机器码，独立作为执行参考；框架输出分别经 V2 O0/O2、静态 O0/O2、
   静态产品优化流程后 O0/O2 编译原生执行。产品流程为 mem2reg/instcombine/GVN/DCE。
4. 每个输入比较全部 76 个已建模寄存器/Flags/PC 字段和完整 65536 字节内存，检查真实根 RET。
   最终 CMP 定义所有被比较的算术标志；本轮不排除任何未定义字段，不做 Legacy 归一化。
5. 两种反汇编策略都检查函数入口；三个无 CALL 家族还精确比较块及边。
   Unicorn 额外的只读观测 hook 记录实际走过的分支边，不修改寄存器、内存或停止条件。
6. 先用故意错误的翻译侧版本验证检测器：反转 JE/JNE；或把被调函数的非零 R15 写入改成 0，
   模拟沿用调用前旧值。参考机器码和初始内存始终不变，要求检测器报告差异。

NOP 扰动会改变代码布局及栈上返回地址，因此两版本分别与各自机器码做完整对拍。
本轮没有通过忽略代码字节、返回地址槽等方式宣称两布局的完整内存直接相等。
只有受控生成代码被执行，不执行任意输入二进制或系统库；测试中被调代码也由 Unicorn 执行其真实字节。

## 首轮结果

| 项目 | 结果 |
|---|---|
| 生成函数 | 8 个种子（1901..1908）× 4 家族 × 2 布局 = 64 |
| 执行比较 | 64 × 6 配置 × 16 输入 = **6144 match，0 different/error/skip** |
| 完整根返回 / 保守出口 | **6144 / 0** |
| 比较字段 / 内存字节 | 466944 / 402653184 |
| 函数入口检查 | 128/128（两种遍历策略） |
| 无 CALL 家族的完整 CFG 检查 | 96/96，0 缺边/多边/块差异 |
| 有非空分支边的执行覆盖检查 | 192/192 观察到全部生成边；不把空边集合算作分支覆盖 |
| 故意反转条件分支 | **96/96 different**；普通模式返回 1 |
| 故意沿用旧被调状态 | **96/96 different**；普通模式返回 1 |
| CFG 检查器负对照 | 删一条边、增加伪入口均被拒绝，2/2 |
| CTest / 无 Z3 CTest | **482/482 / 333/333** |

输入包括 0、1、2、32 位有符号最大/最小值、全 1、跨 32 位边界及 64 位最高位，
其他 GPR、向量、物理 x87 状态及数据缓冲区也初始化并比较。
本轮向量/x87 只检查未修改状态的保持，没有新增浮点或 SIMD 运算覆盖。
CALL 家族只检查根入口和完整执行，不声称检查其全部块粒度表达（CALL 可在块内或分出后继块）。

首个故障原始输出摘录：

```text
diamond-1901-plain / v2-O0 / trial=0:
  RAX got=0xba63 expected=0x865b
  PF got=0x1 expected=0x0
  memory differences=2
call-1901-plain / v2-O0 / trial=0:
  R15 got=0x0 expected=0x52bc8358eb49bf23
  memory differences=8 (0x8008..0x800f)
```

这两个错误版本都仍然执行根 RET，说明仅检查“函数能正常返回”会漏掉这些语义错误。
CTest 的 `--expect-control` 仅在所有记录都为预期差异、没有编译/参考错误、且 CFG/覆盖检查通过时成功，
没有使用“任意非零退出都算检测成功”的放宽方式。

VM 原始记录在 `~/dt-agent-all/validation-results/`：
`generated-final/`、`generated-negative-branch/`、`generated-negative-call/`、
`generated-ctest.log`、`generated-noz3-ctest.log`。
每轮保存种子、标签、汇编、原始字节、翻译侧字节、LLVM IR、驱动、输入/输出快照及逐项差异。
框架生产 `lib/LLVMLifter.cpp` SHA256 仍为
`aa2bb840993c2773b69d4281088265d895ffe4bac68bdee664303ae6e2d0e567`。

## 复现

在测试 VM 的独立仓库运行：

```sh
cd ~/dt-agent-all
TMPDIR="$HOME/dt-agent-all/validation-tmp" \
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
~/ucdev/bin/python3 scripts/validation/generated_function_diff.py \
  --seeds 8 --trials 16 --out validation-results/generated-new
```

负对照使用独立目录，加 `--control invert-branch` 或 `--control stale-call`。
默认发现差异时返回 1；只有 CI 验证故障检出能力时添加 `--expect-control`。

## 结论边界与后续

这轮没有发现生产翻译/优化差异；它提供新的反例搜索能力，不是任意函数正确性的证明。
全部生成边被观察到不代表所有路径组合、循环次数或输入都已覆盖。
未覆盖 i386/A64 的生成测试、复杂混淆器、间接跳转表、未知外部语义、异常/自修改代码，
也没有新增对切片常量、分支不可达结论的 SMT 证明。
既有 69 样本验证不在本轮重跑，因为生产代码没有变化；完整 CTest 和无 Z3 CTest 均已重跑。
下一轮可在同一证据规则下扩展家族和架构，并为具体常量/分支结论生成反例查询。
