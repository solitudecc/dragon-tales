# 第 6 步：完整流水线与最终回归（已完成）

分支 `fix/llvm-flow-acceptance`，生产代码基线 `6dbb9cc`。本步扩展执行对拍、修复参考端的根返回判定，并补齐未知控制转移的安全拒绝。
接口签名、IGNIL CALL/RET 抽象及 V2 符号名称保留。

## 新增执行覆盖与框架失败

增加三架构循环内反复 CALL、0/1/2/3/4/7/8/16 层参数的递归（先导 48/48 match）、
A64 调用后消费 NZCV，以及四条完整加载流水线：
x64 PE、x64/i386/A64 ELF。容器经 LoadedImage→Graph→LLVM→O0/O2 Native 分发执行，
外部桩在 Unicorn 与 Native 使用相同 Guest 字节/行为；本地导入 thunk 也被独立编译、
注册和执行。框架同时要求根实现的 `!dragon.imports` 记录精确匹配槽地址与加载器名称。
没有依赖新增的 C/Python API，也没有把名字绑定到宿主函数。

容器生成器复用已测试的 ELF/PE 导入结构，重新布置代码/入口/加载段及重定位；
`readelf -r` 独立核对 A64 的 JUMP_SLOT/GLOB_DAT。输入 Guest 内存明确映射夹具指令及
外部桩、数据和栈。此处测试的是这些 Guest 函数的执行流水线，不是 OS 动态加载、
内存权限或系统库实现的仿真。

首次 8 个新夹具、O0/O2 各 2 输入：32/32 match，0 skip/error。随后补根退出哨兵的
6 个 CALL/JMP 夹具，发现参考端将“到达 until 地址”一律记为 Return；生产运行时正确
返回 NeedTarget。修复前 24/24 different，寄存器/内存均匹配，差异为退出原因/种类和
CALL return_pc。例如：

```json
{"case":"arm64_call_root_marker","opt":"O2","trial":0,"status":"different",
 "differences":[{"field":"exit",
 "got":[3,2,4096,61440,1,4100,0],
 "expected":[1,3,4096,61440,1,0,0]}]}
```

参考端现在检查 Unicorn 实际完成的最后一条指令：只有 Guest near RET/普通 A64 RET
到达 rootExitPC 才记 Return；声明缺模型的 CALL/JMP 记录 NeedTarget；未声明的其他
到达方式报告框架错误，不放行。修复后 24/24 match，全部为 NeedTarget。
这没有削弱 Native 的退出断言，也没有用地址相等来推测已完成 RET。

## 最终检查补修的生产问题

Legacy 原先只搜索 IGNIL CALL，解码器认识、IGNIL 未表达的远 CALL 可被漏过；未知
RETF 也可能没有 RET 操作。现在同时核对机器控制指令，在创建 LLVM 函数前拒绝遗漏
的调用/返回。Graph-free trace 要求没有 unlifted 标记，避免执行未建模效果。
IGNIL 对未知 CALL/RET 即使没有寄存器定义也记录 unlifted，不再默默遗漏。

旧代码新增 3 个 C++ 测试：43 断言中 19 失败；首次修复后 43/43。再加入远 JMP 和
word 内存 JMP 标记回归，修复前 49 断言中 3 失败，最终修复后 49/49。
远/word 内存 JMP 原先错误地把 MC operand 0 当作目标寄存器；现在标为未 lift 并给
未知目标。V2 对不支持的 x86 JMP 前缀/形式、A64 非普通 B/BR 的认证转移，在指令前
Unsupported，不提交猜测的 PC。对应旧版 Native：x86 4 夹具共 16/16 different，
A64 BRAA/BRAB 两夹具 8/8 different。

新增 i386 回绕 JMP/Jcc 覆盖：生产实现原先即正确。测量 Hook 的 widened 地址标签在
回绕条件分支中与架构 EIP 不同，参考端现在以实际 EIP/PC 寄存器定位边界；没有排除
PC 或扩大任何字段掩码。最初 8 次有 6 match、2 reference_error；修正测量后 8/8 match，
不将这两次框架错误计为产品差异或通过。较高缺失目标的观测页只用于在目标执行前停止，
不由夹具读写，不用它假装实现了映射/OS 故障语义。

最终源码已重新运行完整验证；之前的 `step6-*` 和中断的 `step6-final-*` 是修复前/
中间记录，不计入最终接受数字。以下结果均来自完整的 `step6-release-*`。

## 最终验证（2026-11-03）

| 验证 | 比较/跳过/失败 |
|---|---|
| 三架构完整执行与边界 | 85 夹具 × 8 × O0/O2 = 1360 match；Return 890、NeedTarget 230、Unsupported 240 |
| x64 调用状态模型 | 192 match，全部完成根 RET |
| 分支/循环/default/终止边界 | 240 match；Return 144、NeedTarget 32、Unsupported 48、Budget 16 |
| 三组 V2 合计 | 1792 match；0 different/error/skip、0 Legacy 归一；131264 寄存器字段、117440512 内存字节 |
| 允许的未定义排除 | AF 192 个字段；FSTP C0/C2/C3 96 个位（部分 FPSW 字段），没有扩大排除 |
| 完整加载流水线内存负对照 | 4 映像夹具 × 2 × O0/O2 = 16/16 different，均检测输出翻转 |
| x87 定义位 C1 负对照 | 4/4 different；C1 未被条件位掩码排除 |
| corpus 34 样本两策略 | 88196/88196 函数、729 模块 verify；0 拒绝/失败；70137 条导入来源 |
| corpus2 35 样本两策略 | 114917/114917 函数、927 模块 verify；0 拒绝/失败；89458 条导入来源 |
| 69 样本合计 | 203113 函数、1656 模块；0 拒绝/verify 失败；159595 条导入来源 |

1792 个匹配记录中，正常根 Return 为 1226；NeedTarget 262、Unsupported 288、Budget 16
为按契约比较的停止边界，不冒充正常返回。导入来源数量是元数据测量，不是外部语义通过数。
全部有效函数均遍历，分块大小 128 仅限制每个模块的内存量，没有对函数数设上限。

CTest 和真实块已完成：

| 验证 | 结果 |
|---|---|
| CTest（Z3） | 469/469，0 失败 |
| CTest（无 Z3） | 320/320，0 失败 |
| toy-vm（额外单独执行） | 6 测试、107 断言全通过 |
| guard truth（Z3/无 Z3 各一次） | 各 27 CFG、20736 次 Unicorn 执行、wrong=0 |
| LLVM 真实块 x64 | 比较 150；high-address 跳过 28、Unicorn fault 跳过 21；0 差异/编译/执行错误 |
| LLVM 真实块 i386 | 比较 150；Unicorn fault 跳过 14、unlifted 跳过 6；0 差异/编译/执行错误 |
| LLVM 真实块 A64 | 比较 150；high-address 跳过 48、Unicorn fault 跳过 23、unlifted 跳过 2；0 差异/编译/执行错误 |
| 真实块合计 | 比较 450、跳过 142（high 76、fault 58、unlifted 8），0 差异/错误 |
| 语料跳转表 | 69 样本、138 次两策略运行；exact 1124、wrong 0、unresolved 116、无独立界限 558、不可完整读取 42 |
| 最终未 lift（三份 Lua O2） | i386 188→188、x64 49→49、A64 90→90，逐项直方图完全相同 |

跳转表的 unresolved/无独立界限/不可读均不是已验证集合，未计入 exact。全部命令完成，
最终两套 CTest 无预期失败标记。没有重跑不受本任务影响的 SSE/x87 算术模板全集；
相关既有结果不能算作本轮新测量。本轮实际运行的层次、数字和盲区均如上。

## 已登记的语料告警

全部 69 样本与第 2 步相同路径 JSON 逐字段对照，函数数、缺 IGNIL 块、未解码边、
objdump-only 和 LLVM 拒绝数未变。保留的具体告警：

- 三份 Lua x64：线性 objdump_missing=2，原因是 .text 中嵌入常量。
- `cxx_mingw32_O2.exe`：线性/递归缺 IGNIL 块 2/1，未解码边 14/13；线性 objdump_missing=70。
- `sys_libcrypto.so`：线性缺 IGNIL 块 4、objdump_missing=70；未解码边 0，递归对应告警为 0。

这些不是本轮新错误，也不等于所有机器代码均已支持。IR verify 与语义执行的结论分别记录。

## 复现与原始记录

VM 私有副本 `~/dt-agent-all/validation-results/step6-*` 保存容器、IR/优化 IR、Native
驱动/可执行文件、全部输入输出、对拍 JSON、各命令原始日志和返回码。
`step6-marker-before` / `step6-marker-after` 保留上述框架失败和修复后状态；
`step6-release-progress.json`、`step6-release-finish.log` 和
`step6-release-summary.json` 记录最终命令结果、源码 SHA 和数字汇总。
复现时设置 `DRAGON_C_LIB` 为对应构建的 `c_shim/libdragon_c.so`，并使用磁盘上的
`TMPDIR=$HOME/dt-agent-all/validation-tmp`。

```sh
LLVM_MODE=v2 RESULTS_DIR=validation-results/step6-release-corpus-json ~/ucdev/bin/python3 scripts/validation/corpus_check.py ~/bench/corpus
LLVM_MODE=v2 RESULTS_DIR=validation-results/step6-release-corpus2-json ~/ucdev/bin/python3 scripts/validation/corpus_check.py ~/bench/corpus2
~/ucdev/bin/python3 scripts/validation/function_guest_diff.py --trials 8 --out validation-results/step6-release-guests
~/ucdev/bin/python3 scripts/validation/function_call_diff.py --trials 8 --out validation-results/step6-release-calls
~/ucdev/bin/python3 scripts/validation/function_dispatch_diff.py --trials 8 --out validation-results/step6-release-branches
ctest --test-dir build -j3 --output-on-failure
ctest --test-dir build-noz3 -j3 --output-on-failure
```

## 覆盖边界

执行对拍比较具体输入下的 PC/SP、所有已建模寄存器、定义明确的 Flags、完整 SIMD/x87
字段、全部映射内存、桩事件和退出记录；架构未定义位只使用已登记窄排除。
IR verify 遍历全部有效函数，不能证明那些真实函数的语义；真实块对拍切掉 CALL/分支，
不能代替完整函数用例。跳转表真值仍受独立界限和表数据可读性的限制，未知部分单列。

没有模拟 MMU/OS、精确故障恢复、远调用/系统转移、CET/PAC、并发、自修改或任意外部
库行为。复杂跨块名称证据仍保守缺失。UnknownTarget 的不可知数据传播不由本轮具体值
函数对拍证明；实际数值已知但缺实现时使用 NeedTarget，不将 0 地址当作未知。
