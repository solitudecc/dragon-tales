# 任务 8：第 1 步验证记录

基线 `0aa4b73`；本步仅文档、夹具、框架和测试注册。`lib/`、`include/`、`c_shim/`、
`python/` 的生产代码和现有接口均未修改。契约为[待确认草案](llvm-execution-contract.md)。

## 本步交付

- 独立契约：寄存器所有权、PC/SP/Flags/内存、同步点、退出原因、分发、兼容策略及需要批准的接口。
- 单一 JSON 夹具来源 `tests/fixtures/llvm-function-flow.json`，保留原始 A/B 字节。
- `function_llvm_diff.py`：执行完整函数，不删除 branch/loop/CALL；生成 IR、opt O2、编译为宿主
  原生程序后执行，与 Unicorn 比较。桩在两边具有相同的写寄存器/内存行为并统计调用次数。
- 四个正对照：直接存储、内部条件分支、三次回边循环、返回 0 的模拟调用。
- C++ 的 A/B 规范性断言明确失败；使用 Catch `[!shouldfail]` 记录此阶段的已知缺陷，修复相应
  步骤时移除此标记。另有普通的夹具健康测试及 Native 正对照 CTest，设置/解析/编译失败不会
  靠 expected-failure 获得整体绿灯。

## 数字

测试 VM：aarch64 Linux；`~/ucdev/bin/python3` 的 Unicorn（项目 dev 构建，包版本显示 2.1.4）；
LLVM/opt/llc 21.1.8；gcc 15.2.0。每配置 8 个边界输入，O0/O2 两配置，共 96 次参考执行和
96 次 Native 尝试。输入包括 0、1、全 1、符号位、32/64 位边界，以及 EDI=0 而 RDI 高位非零。

| 夹具 | O0 | O2 |
|---|---|---|
| 4 个正对照 | 32 一致 | 32 一致 |
| A：返回 1 | 8 输出差异 | 8 输出差异 |
| B：条件跨界 | 8 Native 异常退出 | 8 输出差异 |

- 88 份有效 Native 快照：64 一致、24 存在架构输出差异；另 8 次异常退出是失败，不是跳过。
- 实际比较 6544 个寄存器字段、5766912 个内存字节；skipped=0。
- B 的 16 次配置/输入均有不合法语义的 `unreachable` 见证；所有生成配置的 LLVM IR 仍能通过 verify。
- 严格默认命令退出码 **1**。`--assert-baseline` 退出 0 的含义仅是“正对照通过且 A/B 缺陷已检出”，
  其 JSON 仍如实记录 24 个差异和 8 个执行失败，不表示修复。
- 故意翻转 Native 存储结果一个字节的负对照：4/4 差异，0 异常退出、0 跳过，严格命令退出 1。
- 全部 CTest：443/443 按测试配置通过，其中 **2 个是已知失败断言的 expected-failure**，
  不是 443 个语义成功。其余 441 个正常通过；包含原有 439 个测试、1 个普通夹具健康测试和
  1 个函数级 Native 正对照测试。C++ 单独运行显示 2 failed as expected。

## 排除与兼容项

在 88 次实际快照比较中：旧 RET 无最终 PC 输出，PC 字段不比较 88 次；仅抵消根 RET 的
8 字节 RSP 弹栈 88 次；TEST/XOR 的未定义 AF 字段排除 56 次；仅排除实际执行 CALL 写入的
返回地址槽合计 256 字节。CALL 槽来自独立参考的执行轨迹；若普通 STORE 也写该范围，框架
报错，不扩大排除。其他映射字节、GPR、定义明确的 Flags、XMM/x87 都比较。

这些属于已登记的 IGNIL CALL/RET 抽象和手册未定义字段。V2 第 4 步需要移除前三种 Legacy
兼容，并比较真实 PC/SP/返回地址写入。没有把 A/B 排除，也没有把 B 的异常退出算成成功。

## 原始失败输出

以下为 `results.json` 的原始首个输入，未将异常或值改成“预期正确值”：

```json
{
  "case": "A",
  "opt": "O0",
  "trial": 0,
  "known_defect": "call_state",
  "ir_unreachable": false,
  "exclusions": {
    "legacy_pc_fields": 1,
    "legacy_root_ret_rsp_adjustments": 1,
    "undefined_flag_fields": 1,
    "legacy_call_stack_bytes": 8
  },
  "register_fields_compared": 74,
  "memory_bytes_compared": 65528,
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
      "field": "memory",
      "count": 1,
      "examples": [
        {
          "address": "0x8000",
          "got": 0,
          "expected": 1
        }
      ]
    }
  ]
}
```

```json
{
  "case": "B",
  "opt": "O0",
  "trial": 0,
  "known_defect": "conditional_exit",
  "ir_unreachable": true,
  "status": "native_exit",
  "detail": {
    "kind": "native_exit",
    "returncode": -6,
    "stderr": "*** stack smashing detected ***: terminated\n"
  }
}
```

```json
{
  "case": "B",
  "opt": "O2",
  "trial": 0,
  "known_defect": "conditional_exit",
  "ir_unreachable": true,
  "exclusions": {
    "legacy_pc_fields": 1,
    "legacy_root_ret_rsp_adjustments": 1,
    "undefined_flag_fields": 1,
    "legacy_call_stack_bytes": 0
  },
  "register_fields_compared": 74,
  "memory_bytes_compared": 65536,
  "status": "different",
  "differences": [
    {
      "field": "CF",
      "got": "0x1",
      "expected": "0x0"
    },
    {
      "field": "SF",
      "got": "0x1",
      "expected": "0x0"
    },
    {
      "field": "OF",
      "got": "0x1",
      "expected": "0x0"
    },
    {
      "field": "memory",
      "count": 1,
      "examples": [
        {
          "address": "0x8000",
          "got": 0,
          "expected": 9
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
        "12288": 1
      }
    },
    {
      "field": "ir",
      "detail": "unreachable in a fully modelled fixture without traps"
    }
  ]
}
```

## 复现命令与产物

```sh
# 严格执行：当前必须返回 1，A/B 是真实失败
~/ucdev/bin/python3 scripts/validation/function_llvm_diff.py --trials 8 --out validation-results/function-flow-strict
# 只核验当前基线的缺陷与正对照；不算语义成功
~/ucdev/bin/python3 scripts/validation/function_llvm_diff.py --trials 8 --assert-baseline --out validation-results/function-flow-step1
# 负对照：必须返回 1，4 次均检测到被破坏的输出
~/ucdev/bin/python3 scripts/validation/function_llvm_diff.py --case control_store --trials 2 --corrupt-output --out validation-results/function-flow-control
build/bin/dragon_tests "[function-flow]"
ctest --test-dir build -j3 --output-on-failure
```

VM 独立工作副本 `~/dt-agent-all/validation-results/` 保存原始日志、每夹具/优化级的
`raw.ll`、`optimized.ll`、Native 驱动、输入/输出、`results.json` 及工具版本/生产降级源码哈希。
Native 在独立进程中执行，禁用 core dump，超时/信号/缺失快照均判失败。

## 尚未覆盖及下一步

本步没有改降级，因此 A/B 都未修复；未进行 i386/A64 函数级、嵌套调用、未解析/间接控制流、
RET imm16、RET Xn、callee 读取 caller 的新状态或非 ABI/vector/x87 破坏测试。按六步计划，在
各自实现之前先增加相应失败用例。SIMD/x87 本步只证明未触碰字段的保留。

本步未复跑两套语料库、单指令全集、跳转表和未 lift 统计：没有相应生产代码变更，上一轮
`0aa4b73` 的结果只作为已有基线，不冒充本轮测量。最终整体回归按第 6 步执行；本步已有
函数级真实执行、负对照、IR verify、C++ 失败断言及完整 CTest。

**停止点：等待所有者确认契约。第 2–6 步未开始；没有修改现有接口或 IGNIL 刻意抽象。**
