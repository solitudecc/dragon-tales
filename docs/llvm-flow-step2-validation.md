# 任务 8：第 2 步验证记录

2026-11-03；起点 `736a3de`。在独立分支 `fix/llvm-dispatch-exits` 实施最小 V2
分发出口，并修补 Legacy 无法表达的跨界转移。[契约](llvm-execution-contract.md)
第 9 节记录具体 ABI 和本阶段边界。C++/C/Python 接口变更按此前提交的方案及所有者
要求继续下一步的授权实施；没有修改 IGNIL CALL/RET 抽象、RET aux 或旧 LLVM 符号。

## 改动和失败用例

- 显式 StatefulV2 模式，`dragon_v2_<arch>_<hex-address>(state,memory,runtime,exit)`
  四指针入口；共享 C 类型、C++ 分发器、C/Python opt-in 接口。
- 跨界 JMP/CJMP 写回状态和目标 PC，Runtime 按 Guest 地址查实现；两条条件边分别处理。
  已枚举 switch 的 default 走相同分发路径，不使用 `unreachable`。未知值与数值 0 区分。
- 缺少实现时 NeedTarget，预算耗尽时 Budget；Guest/局部循环入口参与预算。
  unlifted/opaque/trap 在最早缺少模型的指令前停止；字节流结束停在下一条指令地址。
  Guest 内存按完整宽度检查边界，不把越界/未知效果交给优化器当作 UB。
- Legacy 不能报告这些跨界/动态转移，故在生成 LLVM 对象前明确拒绝；模块不留残缺函数。
- 模型返回记录须和物化的 Guest PC 一致；错误记录改为 Unsupported + 当前 PC，
  不将模型声称的错误目标继续标为有效。此项加强断言在本步初版上 1 测试/2 断言失败，修复后通过。

新增 Legacy 直接跨界、地址 0、间接未知目标、条件跨界和 switch default 的回归：
只加测试时 **2/2 测试失败，18 个断言中 10 失败**。V2 新接口测试不能在 `736a3de`
头文件上编译；另用第 1 步保存的未修改 Native B 程序做执行前后对照，不能以“不编译”
冒充语义失败。

原 SwitchDetector 测试仍检查两种策略的全部 CFG/case；其 Legacy 部分改为安全拒绝。
新的 V2 JIT 测试实际执行已枚举的两个内部目标、外部默认目标和 0 地址，优化前后各一次，
与 Unicorn 的 PC 对照。避免仅断言 IR 出现 switch，却没有执行默认边。

## 验证数字

VM：aarch64 Linux，主参考为 `~/ucdev/bin/python3` 的 Unicorn dev（包版本 2.1.4），
LLVM/opt/llc 21.1.8，gcc 15.2.0。

| 验证 | 数字 |
|---|---|
| 新增 C++ 回归/分发测试 | 6/6 测试、177/177 断言通过；跳过 0 |
| 默认配置 CTest | 451/451 按配置通过，其中 1 项 A 是 expected-failure；450 项正常通过 |
| 无 Z3 配置 CTest | 302/302 按配置通过，其中 1 项 A 是 expected-failure；301 项正常通过 |
| V2 Native/Unicorn 阶段对拍 | 15 夹具 × 8 输入 × O0/O2 = 240/240 prefix_match；0 差异、0 执行失败、0 跳过 |
| 正对照（现有 Legacy） | 4 夹具 × 2 输入 × O0/O2 = 16/16 一致，0 差异、0 执行失败、0 跳过 |
| V2 故意破坏输出的负对照 | B 两个输入 × O0/O2 = 4/4 差异，0 执行失败、0 跳过；严格命令退出 1 |
| 原始 B 的执行对照 | 保存的 `0aa4b73` Native：8 次异常退出、8 次输出差异；当前 B 两阶段配置共 16/16 前缀一致 |

240 次 V2 比较共检查 **18176 个寄存器字段、15728640 字节内存**，同时比较退出原因、
transfer_kind、source/next/return PC、valid 和 detail。退出统计：192 Unsupported（预定的
CALL/RET 或缺少指令模型边界）、32 NeedTarget、16 Budget；**正常 Guest Return 为 0**，
没有把阶段停止算成完整函数返回。夹具覆盖四个原始正对照、A/B、跨界/间接/缺少/0 目标、
trap、opaque、字节流结束、预算循环及含重叠数据的 REP 局部循环。

没有 Legacy PC/SP/CALL 栈归一，排除数为 0；仅排除 TEST/XOR 手册未定义的 AF 共 64 个字段。
XMM/x87 本阶段主要证明未触碰字段的保留；不代表调用者/被调者的向量和 x87 同步已经验证。

B 的旧 Native 与 before-RET 参考前缀对照不比较旧版未输出的 PC、也没有 V2 退出记录；
它们不是旧版的已验证能力。其余架构输出已经失败，故不是仅靠增加 PC 比较使旧版变红。

### 真实块与语料库

| 样本 | 实际 Native 块比较 | 跳过（原因） | 差异 |
|---|---:|---|---:|
| Lua x64 | 150 | 高地址 28；Unicorn fault 21 | 0 |
| Lua i386 | 150 | Unicorn fault 14；未 lift 6 | 0 |
| Lua A64 | 150 | 高地址 48；Unicorn fault 23；未 lift 2 | 0 |

跳过的块不算成功；这是既有 `realblock_llvm.py` 的范围，没有新增排除。
两个语料库逐一运行线性/递归策略的全部有效函数，使用 `LLVM_MODE=v2`：

| 语料库 | 样本 | 函数降级/verify 次数（两策略合计） | 已验证模块 | 拒绝 / verify 失败 |
|---|---:|---:|---:|---:|
| corpus | 34 | 88196 / 88196 | 729 | 0 / 0 |
| corpus2 | 35 | 114917 / 114917 | 927 | 0 / 0 |

合计 69 样本、138 个样本/策略组合、203113 次函数验证、1656 个模块。
V2 每模块至多 128 个函数，**遍历全部函数，不截断候选集**。Native 夹具还验证了同一模块
含多个 V2 实现的执行；没有把全语料 IR verify 当作全语料语义执行成功。

反汇编例外仍是 AGENTS.md 已登记的项目：三份 Lua x64 各 objdump_missing=2；
C++ mingw32 线性/递归缺少 IGNIL 2/1、未解码边 14/13（线性 objdump_missing=70）；
libcrypto 线性缺少 IGNIL=4、objdump_missing=70。未新增例外、崩溃或超时。

未 lift 对照使用上一轮总验收保存的 `final-unlifted.json`，重跑同路径三个 O2 Lua：
i386 **188→188**、x64 **49→49**、A64 **90→90**，全部形式直方图相同。
最初尝试的用户工作副本旧库早于 x87 实现，另一个无 Z3 旧构建也不具备相同配置，
均未用作正式前后基线。未重跑 69 样本的完整未 lift 直方图；本步未修改 IGNIL 实现。

### 测试资源与原始失败

首次整模块尝试接近 VM 内存上限并被主动停止，未记通过。该尝试在 `/tmp` 留下两份
本任务 V2 IR（约 438 MB、629 MB）；配额导致随后 30 个编译配置报 infrastructure_error、
0 次快照比较，该轮也未记通过。文件已移入工作副本保留；驱动改为有清理保证的临时目录，
V2 分批验证全部函数，原始异常和改动后的完整成功结果分开保存。

```text
fatal error: error closing /tmp/ccIdOlkN.s: Disk quota exceeded
compilation terminated.
```

Legacy 旧代码的原始回归输出：

```text
CHECK_FALSE( lifter.lift(root,graph) )
with expansion:
  !true
CHECK( lifter.module().empty() )
with expansion:
  false
test cases:  2 |  2 failed
assertions: 18 | 8 passed | 10 failed
```

退出记录初版的原始失败：

```text
CHECK( exit.next_pc==0 )
with expansion:
  123 == 0
CHECK( exit.transfer_kind==DRAGON_TRANSFER_NONE )
with expansion:
  1 == 0
test cases: 1 | 1 failed
assertions: 6 | 4 passed | 2 failed
```

负对照首份原始记录：

```json
{
  "case": "B",
  "opt": "O0",
  "trial": 0,
  "status": "different",
  "differences": [
    {
      "field": "memory",
      "count": 1,
      "examples": [
        {
          "address": "0x8000",
          "got": 8,
          "expected": 9
        }
      ]
    }
  ],
  "exit": [
    5,
    3,
    12294,
    12294,
    1,
    0,
    2
  ],
  "budget_left": 996,
  "undefined_flag_fields": 1,
  "register_fields_compared": 75,
  "memory_bytes_compared": 65536
}
```

## 复现与产物

```sh
cmake --build build -j3
ctest --test-dir build -j3 --output-on-failure
build/bin/dragon_tests "[dispatch-v2],[dispatch-regression]"
~/ucdev/bin/python3 scripts/validation/function_dispatch_diff.py --trials 8 --out validation-results/step2-dispatch
# 负对照应返回 1
~/ucdev/bin/python3 scripts/validation/function_dispatch_diff.py --case B --trials 2 --corrupt-output --out validation-results/step2-negative
LLVM_MODE=v2 RESULTS_DIR=validation-results/step2-corpus-json ~/ucdev/bin/python3 scripts/validation/corpus_check.py ~/bench/corpus
LLVM_MODE=v2 RESULTS_DIR=validation-results/step2-corpus2-json ~/ucdev/bin/python3 scripts/validation/corpus_check.py ~/bench/corpus2
ctest --test-dir build-noz3 -j3 --output-on-failure
```

原始产物保存在 VM 独立工作副本 `~/dt-agent-all/validation-results/step2-*`；
`step2-dispatch/results.json` 附工具版本和降级/Runtime 源码 SHA，目录内保留各配置的
IR、Native 驱动、可执行文件、输入/输出。保存的旧 B 对照为 `step2-old-B/results.json`，
正式未 lift 对比为 `step2-unlifted.json`；资源失败单独为 `step2-tmpquota.*`。

## 未覆盖与下一步

本步完成控制流出口，不等同于完整 Guest CALL/RET：A 的调用结果仍未修，Legacy A 的
预期失败标记仍在；V2 A 只验证在 CALL 前停止。第 3 步继续补调用前状态可见、调用后全状态
同步、非 ABI 寄存器/flags/SIMD/x87 破坏的失败用例，再实现同步；第 4 步覆盖真实 Guest
CALL/RET、i386/A64、RET imm16、RET Xn、嵌套调用与恢复。外部命名/剩余间接控制流在
第 5 步实现；真实函数级完整执行和整体回归按第 6 步验收。

单指令全集、跳转表真值未在本步重跑：没有改 IGNIL 或目标发现，相关整体回归留给第 6 步。
V2 i386/A64 的函数级执行、未知值产生 UnknownTarget 的分支、精确内存故障恢复也未由
当前 x64 阶段矩阵证明；不将这些能力计为通过。映射有效且代码不变仍是执行前提。
