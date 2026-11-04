# 完整 SHA-256 的 Tigress 混淆分析

2026-11-04，Dragon `26a15bf` 后台；本轮只增加样本、验证脚本和回归，不修改分析器、
指令语义、公共接口、IGNIL CALL/RET 抽象或 LLVM 函数命名。

结论：受测混淆版 C `-O0` 可得到完整函数执行一致的分析；C `-O2` 有三个分发点尚未恢复，
只能得到部分分析。**本轮完整混淆矩阵未通过，200 次未完成没有计为成功。**
生成的是 CFG 和 LLVM IR，没有声称恢复出原始 C 或消除全部平坦化。

## 样本及独立参考

[样本](../tests/fixtures/tigress-sha256.c) 是完整的流式 SHA-256：

- 64 个轮常量、64 字消息扩展、64 轮压缩及八个状态字累加。
- init/update/finish，支持任意分块更新、直接整块处理、剩余缓冲、双块填充及大端长度编码。
- kernel 保留真实内部函数调用，返回摘要前四字节，并写出完整 32 字节摘要。
- 所有算术使用无符号类型，没有调用宿主哈希库或用哈希效果桩替代 Guest 指令。

算法按 [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) 实现。
使用 [NIST NSRL](https://www.nist.gov/itl/ai/ai-standards-and-guidelines-group/nsrl-test-data)
公布的 `abc`、56 字节消息和一百万个 `a` 的 SHA-256 向量固定摘要；其他输入与 Python
`hashlib.sha256` 独立比较，不用 Dragon 自己生成预期摘要。

25 个 Guest 输入包括空串、上述短标准向量、全零、全 FF、所有字节值、确定种子的随机消息，
长度涵盖 1/2、55/56/57、63/64/65、111/112、119/120、127/128/129、255、1024、4096。
分块大小为整段、1、7、64；同一 `abc` 还分别测试整段和逐字节更新。
一百万个 `a` 仅在 ARM 原生 C 检查，**没有在 64 KiB Guest 内存的 LLVM 对拍中执行**。
这不是所有长度、所有分块组合或所有输入的证明。

## 混淆与分析配置

使用公开旧版 Tigress 2.1 revision 2128，来源及 QEMU 生成器环境见
[工具来源记录](tigress-function-validation.md)。固定种子 `2561901`。
QEMU 仅运行生成器，不是执行语义的参考。

| 配置 | 变换 |
|---|---|
| plain | 原始实现 |
| flatten-switch | Flatten，switch 分发，随机块顺序，拆分基本块 |
| arithmetic-flatten | EncodeArithmetic，然后同样的 Flatten |

两种混淆均作用于 `sha256_compress,sha256_update,sha256_finish`，不只包裹外围入口。
生成 C 由 Clang 21.1.8 编译为 x86-64 ELF，分别 C `-O0/-O2`。
保留并装载代码、常量、数据、BSS；固定低地址链接，不重新编排机器指令或手填跳转目标。

每份 ELF 分别运行线性、递归策略，输出函数清单、逐函数 CFG DOT、未恢复跳转和 unlifted。
已知函数入口来自 ELF STT_FUNC 检查，不额外注入运行轨迹或修复边。
执行参考另行记录访问的指令和跳转，检查是否有遗漏；观察到的路径不代表所有路径。
跳转边检查使用 BasicBlock.successors，包含条件跳转的落空边。
每次分析都从检查器的私有 CFG 副本删掉一条实际观察到的边，确认检查器能拒绝它；
12/12 个删除边的负对照有效，实际分析图不受改动。

两种 LLVM 流程：

1. `v2-O0`：V2 原始 IR，运行时分发预算 1000000 个块。
2. `product-O2`：静态模式，先执行产品优化，再 `opt default<O2>`；测试分发器在优化之后链接，
   只执行真实 Guest 降级代码，静态预算为 0 并检查未被修改。

24 份执行模块均验证原始及最终 IR。每次根返回比较全部 76 个已建模字段、65536 字节内存、
PC/SP/标志位及 Exit；无字段排除、无 Legacy CALL/RET 归一化。
Unicorn 参考预算 10000000 条指令并要求实际 RET 到根标记，不把预算耗尽算成功。

## 结果

| C 编译及样本 | 完整状态匹配 | 根返回前未完成 | 完整返回后的差异 |
|---|---:|---:|---:|
| 原版 O0 | 100 | 0 | 0 |
| 原版 O2 | 100 | 0 | 0 |
| Flatten O0 | 100 | 0 | 0 |
| Flatten O2 | 0 | 100 | 0 |
| 算术编码 + Flatten O0 | 100 | 0 | 0 |
| 算术编码 + Flatten O2 | 0 | 100 | 0 |
| 合计 | **400** | **200** | **0** |

每行：25 个输入 × 两策略 × 两 LLVM 流程。
400 次完整比较包含 30400 个字段和 26214400 字节内存。
计划及实际执行均为 600；未执行、参考异常、编译失败、原生异常、成功跳过、字段排除均为 0。
正常命令返回 **1**，因为未完成项和观察到的 CFG 缺失均不能通过。

- ARM 原生 C：156/156 摘要匹配（六份编译配置，每份 25 个常规输入和一个百万字节向量）。
- x86-64 Unicorn：150/150 摘要及返回值匹配（六份配置，每份 25 个输入）。
- 入口检查：12/12，无漏报、多报；本样本各配置 unlifted 均为 0。
- 不同二进制之间只比较摘要；完整寄存器/栈/内存只比较同一份机器码，避免把寄存器分配差异误报为错误。

### 实际恢复出的控制流

两策略在 C O0 的结果一致：

| sha256_compress | 恢复的基本块 | 恢复的指令 |
|---|---:|---:|
| 原版 | 10 | 249 |
| Flatten | 59 | 351 |
| 算术编码 + Flatten | 59 | 1598 |

三份 O0 的已执行指令和跳转均包含在静态分析结果中；原版 O2 也没有观察到缺失。
这说明本轮确实分析了增加复杂度后的 SHA 主体，而不只是没有混淆的包装器。

C O2 的两份混淆版，每份都有三个间接分发未恢复。
此时每个受混淆函数只恢复出入口附近三个块；不能把这种小 CFG 解释成成功简化了函数。
Flatten O2 的原始诊断：

```text
0x1076: jmp qword ptr [8*rax + 8192]   # finish
0x12aa: jmp qword ptr [8*rcx + 8488]   # compress
0x17a6: jmp qword ptr [8*rax + 9184]   # update
Exit = [3, 1, 0x17a6, 0x1851, 1, 0, 0]   # NeedTarget / Branch
```

算术叠加版 O2 首例：

```text
Exit = [3, 1, 0x1246, 0x122e, 1, 0, 0]
```

每份 O2 混淆版：线性策略缺少 186 条已观察跳转边；递归策略缺少 96 条已观察跳转边。
递归策略还分别缺少 443/665 个已执行指令地址；线性策略解码到了这些指令，但未连成完整函数图。
96 和 186 的检查分母不同，不能据此说递归恢复得更好。
这些运行均归为未完成，不拿中途状态与最终摘要作语义匹配。
没有通过补入执行轨迹、猜测表边界或删除保守出口来制造完成结果。

## 初始化遗漏的失败探测

Tigress 将常量 K 的静态初始化改写成真实 `K_i$nit()`、`megaInit()`，由生成的 main 调用。
第一次直接从 kernel 开始的探测遗漏了初始化，五个 ARM 和五个 Unicorn 摘要都不匹配。
例如空串错误得到：

```text
8b025d1c663475e5835cfdb00f764c1e5693643718e04f25af7042405e72a1d6
```

同时 Dragon 忠实执行了这份缺少初始化的机器码，LLVM 5/5 完整状态匹配。
这证明只看 LLVM 对拍还不够，独立摘要检查确实拒绝了错误的测试入口。
该结果不能归因于 Tigress 算错 SHA，也不是 Dragon 指令语义错误。

最终入口先执行生成的真实 Guest `megaInit()`，保存并恢复传给 kernel 的四个参数，
然后调用 kernel。ARM 主机驱动也调用同一初始化。
没有手工修改生成 C 的算法或把正确 K 表直接写进 Guest 内存。
主机编译只重命名 Tigress 自动生成的无关 main，避免与测试驱动 main 重定义；
Guest 链接删除不可达 main，保留初始化函数和被调用的 SHA 函数。

早期 `sha256-flatten-pilot2` 的失败与 `sha256-flatten-initialized` 的五项通过均保留。
早期 `sha256-pilot` 使用了错误的分支属性读取，误报条件落空边；最终改用实际 CFG 后继，
同时用上述删边负对照证明检查仍有效。
`sha256-full` 是早期只执行 V2 的记录，最终两流程数字只来自 `sha256-final`，不混合计数。

## 负对照与回归

使用 plain、C O0、线性策略、五个快速输入、同样的两种 LLVM 流程：

| 故意错误 | ARM / Unicorn 摘要检查 | LLVM 完整状态检查 | 普通命令返回码 |
|---|---:|---:|---:|
| C 代码翻转摘要首字节 | 5 different / 5 different | 10 match | 1 |
| 损坏降级结果 RAX bit 0 | 5 match / 5 match | 10 different | 1 |
| 损坏降级结果输出内存 | 5 match / 5 match | 10 different | 1 |

CTest 的 `--expect-control` 要求数量完整、错误出现在指定层、实际根返回、入口和边检查正常；
基础设施错误不能冒充有效负对照。四项 CTest 使用 plain，不依赖 Tigress，也不声称混淆矩阵通过。
完整 CTest **495/495**，无 Z3 **346/346**，分别 44.97/43.88 秒。
构建均成功；生产指令语义和后台未改动，本样本 unlifted 前后均为 0。

## 复现及产物

```sh
cd ~/dt-agent-all
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
TMPDIR="$HOME/dt-agent-all/validation-tmp" \
CILLY_NATIVE_CC=x86_64-linux-gnu-gcc \
~/ucdev/bin/python3 scripts/validation/tigress_sha256.py \
  --tigress "$HOME/bench/tools/tigress-2.1-github/run-tigress" \
  --out validation-results/sha256-final-reproduction
```

正常全矩阵预期返回 1，不能加“预期失败”标签称其全部通过。
可用 `--profile plain --c-level O0 --strategy 0 --quick` 检查检测器；默认完整输入不启用 quick。
`--plan` 可显式选择既有六种流程，本轮只运行元数据记录的 V2 O0、产品静态 O2 两种。
`--control c-digest|llvm-register|llvm-memory` 用来检验故意错误，CI 严格检查才加 `--expect-control`。

VM `~/dt-agent-all/validation-results/sha256-final/` 保存生成前后 C、完整命令、六份 ELF、
十二份分析、各函数 CFG DOT、原始/优化 IR、原生驱动、输入输出快照、摘要、逐项 Exit 和检查结果。
`metadata.json` 固定样本、脚本、实际加载的后台及 C shim 校验值；生产 LLVMLifter SHA256 保持
`aa2bb840993c2773b69d4281088265d895ffe4bac68bdee664303ae6e2d0e567`。
本机 `ANALYZE/tigress-sha256-20261104/` 保存可直接查看的审阅副本和压缩包：
203 个文件，解包 43064743 字节，清单中的 202 项 SHA256 已复核。
包含全部生成 C、ELF、分析和 CFG，以及线性策略的原始/优化 IR；完整执行快照保留在 VM。
该目录按仓库既有规则忽略，不把生成器二进制或大型实验产物加入 Git。

未覆盖新版 Tigress、多种随机种子、其他架构、Virtualize、全部六种 LLVM 流程、未知外部函数、
所有消息与分块组合、完整符号路径等价证明。没有改动生产语义，未重跑单指令和 69 份真实库矩阵；
本轮范围是该 SHA 实例及新检测器。保留符号的入口检查也不能证明剥符号函数边界可靠。
