# 第 4 步：完整 Guest CALL/RET 验证记录

实现分支 `fix/llvm-guest-call-ret`，基线 `84faa9d`（第 3 步）。仅 StatefulV2 实现
机器 CALL/RET；Legacy 名字/签名和 IGNIL 的无压栈 CALL、无操作数 RET 抽象保留。
A64 RET 的 aux 使用已批准的寄存器约定。没有增加 C/Python 接口。

## 修改及独立参考

- x64/i386 near CALL 从旧寄存器/内存快照求目标，再压 Guest 返回地址并更新 SP。
  i386 支持 4 字节与 66 前缀的 2 字节；16 位目标 IP 截断。内存目标加载实际宽度。
- x86 RET 加上返回地址宽度及无符号 aux imm16；Guest 返回地址可以被程序改写。
- A64 BL/BLR 在求目标后更新 X30；RET 使用真实 Xn，X31 是 XZR。
- 返回目标由分发器处理，只有 rootExitPC 才结束根执行；目标 0 是有效已知目标。
- i386 `CALLpcrel16/CALL16r/CALL16m/RET16/RETI16` 补齐原有抽象控制操作的分派。
  此前相对 CALL16 会被静默遗漏；不是通过放宽对拍绕过。
- far/system/authenticated 转移、x64 66 前缀 CALL/RET 保持执行前 Unsupported。

参考使用 Unicorn dev 2a05c41（包版本 2.1.4），相同机器字节完整运行至根哨兵，或在
夹具明确声明的 NeedTarget/Unsupported 边界停止。普通 near CALL/RET 的栈与 IP 规则
对照 [Intel SDM](https://www.intel.com/content/dam/develop/external/us/en/documents-tps/253665-sdm-vol-11.pdf)
§6.4.7；A64 可指定 RET 寄存器见
[Arm 指令集概述](https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf)。
没有将参考差异判为模拟器 bug，没有新增比较排除。

## 先失败，再修复

新增 `function_guest_diff.py`、40 个夹具及 `[guest-ret]` C++ 回归。
初始 36 个夹具在第 3 步二进制 O0/O2 各 1 次：72/72 different，0 参考/基础设施错误，
0 跳过。其中包括普通/嵌套 CALL、RET imm16（1 和 65535）、改写返回地址、CALL SP、
CALL [SP]、BLR X30、RET X9/XZR、向量破坏及原始 A/B。

第一版第 4 步实现的 144 次先导验证：140 match、4 different，全部差异来自遗漏 CALL16。
再加入寄存器、内存及 RET imm16 的 3 个 word 夹具，修复前 6/6 different。例如：

```json
{"case":"x86_32_word_call_reg","opt":"O0","trial":0,"status":"different",
 "differences":[{"field":"EAX","got":"0x2000","expected":"0x7"},
 {"field":"memory","count":2,"examples":[
 {"address":"0xc7fe","got":141,"expected":7},
 {"address":"0xc7ff","got":185,"expected":16}]},
 {"field":"calls","got":{"8192":0,"12288":0},"expected":{"8192":1}}]}
```

跨 64 KiB 的 CALL16 另有 C++ 回归：旧处理给 0x12000，Unicorn 为 0x2000；
修复前 4 测试/57 断言中 1 测试、1 断言失败，修复后 57/57 通过。
独立执行 `66e8fc0f @0x11000` 得 EIP=0x2000、ESP=0xc7fe、返回槽=0410。
最后一个 word 内存夹具把目标指针放在映射末端 0xfffe，确认只读取 2 字节。
最初的 A64 元数据和 V2 RET 回归在旧实现有 5/30 断言失败。

## 当前验证数字

| 验证 | 比较、跳过、失败 |
|---|---|
| C++ Guest 控制流 | 4 测试、57 断言全通过 |
| 三架构完整执行 | 40 夹具 × 8 输入 × O0/O2 = 640 match；0 different/error/skip |
| 正常返回与边界 | Return 544、NeedTarget 64、Unsupported 32；各边界状态均独立比较 |
| 比较覆盖 | 46576 寄存器字段、41943040 内存字节；48 个手册未定义 AF 字段；0 Legacy 归一 |
| 原调用状态框架升级 | 192/192 match；全部完成根 RET |
| 原分支/循环框架升级 | 240/240 match：Return 144、NeedTarget 32、Unsupported 48、Budget 16 |
| 三架构内存负对照 | 12/12 different；都检出故意翻转的输出字节，0 跳过 |
| x87 定义位 C1 负对照 | 4/4 different；未扩大 FSTP 排除范围 |
| 未 lift（三份 Lua O2） | i386 188→188、x64 49→49、A64 90→90，逐项直方图相同 |
| 完整 CTest（Z3） | 460/460；0 失败/预期失败 |
| 无 Z3 CTest | 311/311；0 失败/预期失败 |
| A64 单指令 BL/BLR/RET 专项 | 5 模板 × 40 输入 = 200；2 模板/80 输入相同，3 个已登记抽象差异/120 输入；0 未 lift/框架错误 |

工具：LLVM/opt/llc 21.1.8，GCC 15.2.0，aarch64 Linux 宿主。新框架比较所有已建模
GPR、PC/SP、Flags、128 位向量、x86 原始 x87/控制状态、全部 64 KiB 内存、桩进入次数
及七字段退出记录。i386 只比较该架构的 8 个 GPR/XMM；A64 比较 31 个 X、SP/XZR、
32 个完整 V 和 NZCV。未建模的快照 padding 不冒充寄存器字段。
原调用 x87 框架只沿用 FSTP 手册未定义 C0/C2/C3（8/10/14）窄掩码；定义位仍比较。

## 原始产物及复现

VM 私有副本 `~/dt-agent-all/validation-results/step4-*` 保存旧版失败、IR、优化 IR、
Native 驱动/可执行文件、每次输入/输出和完整 JSON。`step4-final-guest/results.json`
记录源码 SHA、工具与参考版本；不修改生成的 IR 来适配测试。

```sh
cmake --build build -j3
build/bin/dragon_tests '[guest-ret]'
~/ucdev/bin/python3 scripts/validation/function_guest_diff.py --trials 8 --out validation-results/step4-final-guest
~/ucdev/bin/python3 scripts/validation/function_call_diff.py --trials 8 --out validation-results/step4-calls
~/ucdev/bin/python3 scripts/validation/function_dispatch_diff.py --trials 8 --out validation-results/step4-branches
ctest --test-dir build -j3 --output-on-failure
```

## 未覆盖

没有模拟分段/OS、架构精确故障恢复、CET/PAC、自修改或并发；这些条件不算成功。
本阶段是小型函数和显式外部模型执行，不是任意真实程序的全系统执行。
外部名称、PE IAT/ELF GOT/PLT/thunk 标识及其保守传播留给第 5 步。
本步没有复跑两套完整语料、三架构真实块和跳转表全集；第 6 步对最终实现整体回归。
A64 全指令 40 次输入的运行手动中断，未计通过；本步仅统计上述控制指令专项。
