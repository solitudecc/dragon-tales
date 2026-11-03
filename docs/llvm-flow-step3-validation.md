# 任务 8：第 3 步验证记录

2026-11-03；基线 `02e9804`，独立分支 `fix/llvm-call-state`。
本步落实调用边界的权威状态与恢复，不假设 Guest callee 遵守宿主或 Guest ABI。
被调实现仍为第 2 步批准的四指针协议，LLVM 名字和 C/Python 既有签名未再次改变。

## 改动

Guest CALL 同步状态后交给分发器，Guest callee 产生新 PC 后重新分发；Caller 在真实
恢复入口重新加载寄存器文件，原 alloca/SSA 不跨调用存活。`dragon.entrypoints` 属性列出
该实现支持的 Guest BB 和解码的 CALL fall-through；注册这些地址到同一实现才能准确恢复，
不能从函数起点重跑。需要缺少的临时定义时拒绝 lift，避免用隐藏 SSA 代替 Guest 状态。

为验证 callee 读到真实 SP，本步同时实现普通 x64 直接 CALL（无前缀 E8 rel32）的机器
前置效果：先求目标，再在 Guest 栈压入可靠解码所得 return_pc、更新 RSP/PC，最后退出
Transfer Call。缺目标时 NeedTarget 保留该状态，提供模型后从目标继续，不能再次 CALL。
其他 CALL 形式和所有 LLVM RET 仍 Unsupported，完整范围在第 4/5 步补齐。

Legacy 一指针 callee 协议无法传递完整状态，故现在明确拒绝 CALL，包括可达的 trace CALL；
原 trace 在 RET 后忽略普通后续操作的规则保留。没有重定义 IGNIL CALL/RET，也没有改
其操作数或 aux。toy-vm 的实际汇编不含 CALL，完整 CTest 继续覆盖其两套 JIT 流程。

x87 写回以 FPTOP 合成 SW.TOP，保证调用边界中的两个视图一致。状态不经过浮点宿主参数
转换；原始物理 80 位寄存器及 128 位向量都保留位模式。

## 先失败、再修复

只增加 Legacy 回归时，未修改的 `02e9804`：**2 个测试失败，24 个断言中 6 失败**。
新增第三个兼容测试验证 RET 后 CALL 仍被忽略，避免为了安全拒绝而改掉准确的 trace 行为。
当前 `[call-state],[function-flow]`：**5/5 测试、65/65 断言通过**。

12 个调用夹具在 O0/O2、各 2 个输入上运行未修改的第 2 步实现：48 次全部失败，
**44 输出差异、4 次 Native 执行失败**（恢复用例明确检查 NeedTarget 前置状态，旧版返回 94）。
每份差异都包含 FPSW 以外的字段；不是靠未定义状态位使旧版变红。A 的原始首份 O2 输出：

```json
{
  "case": "A",
  "opt": "O2",
  "trial": 0,
  "status": "different",
  "differences": [
    {
      "field": "RAX",
      "got": "0x0",
      "expected": "0x1"
    },
    {
      "field": "PF",
      "got": "0x1",
      "expected": "0x0"
    },
    {
      "field": "ZF",
      "got": "0x1",
      "expected": "0x0"
    },
    {
      "field": "RIP",
      "got": "0x1002",
      "expected": "0x1012"
    },
    {
      "field": "memory",
      "count": 8,
      "examples": [
        {
          "address": "0x8000",
          "got": 0,
          "expected": 1
        },
        {
          "address": "0xc7f9",
          "got": 133,
          "expected": 16
        },
        {
          "address": "0xc7fa",
          "got": 218,
          "expected": 0
        },
        {
          "address": "0xc7fb",
          "got": 12,
          "expected": 0
        },
        {
          "address": "0xc7fc",
          "got": 170,
          "expected": 0
        },
        {
          "address": "0xc7fd",
          "got": 197,
          "expected": 0
        },
        {
          "address": "0xc7fe",
          "got": 87,
          "expected": 0
        },
        {
          "address": "0xc7ff",
          "got": 251,
          "expected": 0
        }
      ]
    },
    {
      "field": "stub_calls",
      "got": {
        "8192": 0,
        "12288": 0
      },
      "expected": {
        "8192": 1
      }
    },
    {
      "field": "exit",
      "got": [
        5,
        2,
        4098,
        4098,
        1,
        0,
        2
      ],
      "expected": [
        5,
        3,
        4114,
        4114,
        1,
        0,
        2
      ]
    }
  ],
  "exit": [
    5,
    2,
    4098,
    4098,
    1,
    0,
    2
  ],
  "budget_left": 998,
  "undefined_flag_fields": 1,
  "register_fields_compared": 75,
  "memory_bytes_compared": 65536
}
```

调用后的结果路径、调用前参数/栈状态、CF 破坏、非 ABI R15、XMM 参数和 clobber、x87
参数和返回、内存参数，以及缺模型后恢复均被固化。A 的 expected-failure 标记已经移除，
Legacy 安全拒绝；V2 用原始 A 字节执行正确调用结果路径到 root RET 之前。

## 验证数字

VM/参考工具沿用第 2 步：aarch64 Linux、Unicorn dev（包版本 2.1.4）、LLVM 21.1.8、gcc 15.2.0。

| 验证 | 结果 |
|---|---|
| 调用 Native/Unicorn 对拍 | 12 夹具 × 8 输入 × O0/O2 = 192/192 prefix_match；0 差异、0 执行失败、0 跳过 |
| 分发/循环前缀回归 | 15 夹具 × 8 输入 × O0/O2 = 240/240 一致；0 差异、0 执行失败、0 跳过 |
| 默认配置 CTest | 455/455 正常通过；0 expected-failure |
| 无 Z3 配置 CTest | 306/306 正常通过；0 expected-failure |
| A 的内存破坏负对照 | 4/4 差异，0 执行失败、0 跳过；命令返回 1 |
| x87 定义明确的 C1 破坏负对照 | 4/4 差异，0 执行失败、0 跳过；命令返回 1 |
| 代表性完整函数 IR | 三种架构 Lua、A64/x64 SQLite，共 5 样本 × 两策略；13788/13788 函数、113 模块 verify，拒绝/verify 失败均 0 |

192 次调用快照比较 **14528 个寄存器字段、12582912 字节内存**，包括 PC、SP、CALL 返回
地址槽和退出记录。没有 Legacy 归一或内存排除。全部 192 次终止是 root RET 前的预定
Unsupported，不是完整 Guest Return，不能声称 LLVM RET 已经实现。

分发回归中两个 CALL 夹具现在执行到 callee RET 前（真实 CALL 效果已比较），其余边界不变。
Legacy 原有 call-zero 正对照没有被删除：迁到更强的 V2 调用对拍，并增加 Legacy 拒绝断言；
Legacy 原生对照保留其可准确表达的三种函数。

### 精确的未定义字段排除

- XOR/TEST 的最终 AF：64 个字段；仅声明 AF 的四个调用夹具使用，其他 CPU flags 都比较。
- 两个 FSTP 夹具的 SW.C0/C2/C3：各次只屏蔽位 8/10/14，总共 96 位，仍比较其余 SW 位。
  参考 [Intel SDM FST/FSTP](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf)
  Vol. 2A 3-437。TOP、TAG、CW、C1、异常位和所有物理寄存器的完整 80 位没有排除。
- 本次最初只发现上述未定义位的差异；查手册后按规则加入窄掩码，没有改参考值或改降级。
  定义明确的 C1 位负对照证明未将整个 FPSW 字段屏蔽；原始错误输出为：

```json
{
  "case": "call_x87",
  "opt": "O0",
  "trial": 0,
  "status": "different",
  "differences": [
    {
      "field": "FPSW",
      "got": "0x200",
      "expected": "0x0"
    }
  ],
  "exit": [
    5,
    3,
    4105,
    4105,
    1,
    0,
    2
  ],
  "budget_left": 996,
  "undefined_flag_fields": 0,
  "undefined_fpsw_bits": 3,
  "register_fields_compared": 76,
  "memory_bytes_compared": 65536
}
```

这些是已有架构未定义规则的登记，不是新增刻意设计差异或 Unicorn 修正。原始二进制快照
保留全部位，比较日志显示相应窄掩码；计数包含部分比较的 FPSW 字段。

## 复现和原始产物

```sh
cmake --build build -j3
build/bin/dragon_tests "[call-state],[function-flow]"
ctest --test-dir build -j3 --output-on-failure
~/ucdev/bin/python3 scripts/validation/function_call_diff.py --trials 8 --out validation-results/step3-calls
~/ucdev/bin/python3 scripts/validation/function_dispatch_diff.py --trials 8 --out validation-results/step3-prefix
# 以下两条应返回 1，并各报告四份实际值差异
~/ucdev/bin/python3 scripts/validation/function_call_diff.py --case A --trials 2 --corrupt-output --out validation-results/step3-memory-control
~/ucdev/bin/python3 scripts/validation/function_call_diff.py --case call_x87 --trials 2 --corrupt-fpsw --out validation-results/step3-status-control
ctest --test-dir build-noz3 -j3 --output-on-failure
```

VM 私有工作副本 `~/dt-agent-all/validation-results/step3-*` 保存旧版失败、IR/优化 IR、Native
驱动/模型/可执行文件、输入/输出、结果 JSON 和工具/源码哈希；`step3-ir-*.json` 是代表性
整函数验证。外部模型独立实现夹具机器效果和真实 Guest RET，Unicorn 执行原始机器字节；
未改写降级生成的 IR，也没有剔除失败模板。

## 尚未覆盖

本步没有完整 root LLVM RET、i386/A64 CALL/RET、RET imm16/RET Xn、非普通/间接 CALL、
嵌套调用或被改写返回地址的完整执行验证；按第 4/5 步先加失败用例再实现。x64 普通直接
CALL 的 Guest 前置效果已实现，其他尚不支持的效果明确停止。

未复跑两套 69 样本全集、三种架构真实块全集和跳转表真值：本步没有改 IGNIL/目标发现，
第 2 步完整数字只作为基线；本步针对新增恢复入口实际复验了 13788 个函数和全部 CTest。
最终第 6 步还会做当前实现的整体回归，不将上一轮结果冒充本轮测量。
未 lift：本步没有改 IGNIL，未重做全库直方图；第 2 步三份 Lua 的 188/49/90 保留为已有基线。
