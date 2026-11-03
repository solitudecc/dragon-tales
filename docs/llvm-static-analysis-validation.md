# 静态 LLVM 模式实施记录

状态：三步完成，最终验证通过（2026-11-04）。

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
| 根 Return / 返回其他目标的出口 / Transfer / NeedTarget / Unsupported | 804 / 96 / 220 / 96 / 240 |
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

## 第 3 步：产品优化流程与全量回归

此步不改变产品语义、接口或名字。`check_one.py` 新增 `LLVM_MODE=static`，默认在
每一批函数上分别验证原始 IR 和库自带 `optimize()` 后的 IR；`LLVM_OPTIMIZE=0`
可仅检查原始输出。旧工具在真实 Lua 样本上拒绝该模式，原始输出为
`ValueError: LLVM_MODE must be legacy or v2`（`static-step3-before-check.log`）。
既有 V2/Legacy 的默认验证方式不变。

函数级静态验证现先运行库自带四个优化 pass，再分别以 O0 和额外 LLVM O2 编译执行；
保留 `unoptimized.ll`、`raw.ll`（产品 pass 后）和 `optimized.ll`（额外 O2 后）。
外部桩始终在这些优化完成之后才链接，产物不链接 libdragon 执行运行时。
汇总还将 `Return` 细分为实际返回根标记和返回其他目标的出口；第 2 步的 900 条
Return 中只有 804 条为根返回，其余 96 条为重定向/返回 0 的边界，不能混算正常完成。

汇总器 `static_corpus_summary.py` 要求两套语料的文件集合完整、函数和模块覆盖数完整、
原始/优化 IR 均通过验证，并与第 6 步的结构基线逐字段比较。
`test_static_corpus_summary.py` 的 1 个正对照及 **10 个负对照**全部通过，确认统计会拒绝
缺文件、漏函数、漏模块、错误 IR、未优化的记录及结构基线变化。

资源记录：前期批量大小 128，i386 SQLite 优化时出现约 2.3 GB RSS 和约 2.2 GB swap。
后续静态批量改为 32；每个函数仍执行优化前后两次验证，无截断或跳过。
早先完成的记录保留，模块总数因此包含两种批量大小；函数覆盖数仍可直接比较。
单个大型函数本身也可能占用较多内存。重跑默认使用 32，不要求得到相同模块分批数量。

手册中的 A 静态输出示例已实际生成、优化并 verify：22158 → 13050 IR 字节、
13 → 0 个寄存器 alloca，未知外部调用保留 1 个，条件分支保留 6 个。
这是单个示例的 IR 简化记录，不能当作真实二进制反混淆覆盖率。
产物在 VM `validation-results/static-step3-output-example/`。

### 最终验证数字

| 检查 | 结果 |
|---|---|
| CTest（Z3 / 无 Z3） | **479/479 / 330/330** |
| 产品优化后，17 个调用/状态夹具、O0/O2 | 272 match；266 根返回、6 Transfer；0 skip |
| 产品优化后，91 个三架构夹具、O0/O2 | 1456 match；804 根返回、96 其他返回出口、220 Transfer、96 NeedTarget、240 Unsupported；0 skip |
| 两类函数比较合计 | **1728 match，0 different/error，0 skip**；1070 根返回、658 显式边界 |
| Exit 协议 | 288 快照比较通过；另 16/16 非返回子进程按预期退出，无后继快照 |
| 内存负对照 / 定义明确的 x87 C1 负对照 | 各 4/4 different，脚本均按预期返回 1，无基础设施错误或跳过 |
| 两套语料 | **69/69 文件、138/138 策略**，203113 次函数检查，0 lift 拒绝 |
| 语料优化前 / 产品优化后 verify | **5227/5227 / 5227/5227 模块**；两边均覆盖全部 203113 次函数检查 |
| 结构基线 | 138/138 策略的函数数、块数、缺块、边、符号入口及 objdump 告警保持原状 |
| 导入来源 | 159595 条元数据引用；仅诊断来源，不计为语义执行成功 |
| 真实基本块 LLVM 原生对拍 | 450 match，0 different/error；另 142 次已登记跳过 |
| 三份 Lua、两策略跳转表 | exact 104、wrong **0**、unresolved 10、无可证明界 76、不可读 0；六份原始输出与旧基线相同 |
| 三份 Lua 未 lift | i386 188→188、x64 49→49、A64 90→90，完整形式直方图也相同 |
| toy-vm | 6 测试、107 断言通过 |

1728 次函数比较共检查 **126496 寄存器字段、113246208 内存字节**。
只沿用手册允许的 AF 排除（160 个字段实例）和 FSTP C0/C2/C3 排除（96 位实例），
无 Legacy 归一化。Exit 快照检查另比较 21600 寄存器字段、18874368 内存字节；
非返回检查不计入这些快照数字。

真实块跳过明细：高地址限制 76、Unicorn fault 58、未 lift 8；i386/x64/A64 各比较
150 次。它们使用既有 Legacy trace 路径、去掉机器 CALL/JMP，不能替代上面的静态函数级检查。
跳转表本步只重跑三份 Lua 的六次 oracle；两套 69 样本的结构回归全部执行，
未声称重跑了所有 69 样本的跳转表语义 oracle。

语料已知告警没有消失：三份 x64 Lua 各 2 个 objdump-only 地址；
`cxx_mingw32_O2.exe` 线性/递归缺 IGNIL 块 2/1、未解码边 14/13，线性 objdump 缺 70；
`sys_libcrypto.so` 线性缺 IGNIL 块 4、objdump 缺 70，未解码边 0，递归无这些块/边问题。
以上均与第 6 步 JSON 基线逐字段一致，不将原告警算作“全部干净”。

语料分批统计：40 份策略记录使用早期 128，98 份使用 32；因此最终为 5227 个模块，
不是旧 V2 验收的 1656 个模块。没有丢函数，前后验证的函数覆盖数均为 203113。
环境：Ubuntu aarch64，LLVM 21.1.8，GCC 15.2.0，Unicorn dev（报告版本 2.1.4）。
最终 `lib/LLVMLifter.cpp` SHA256 为
`aa2bb840993c2773b69d4281088265d895ffe4bac68bdee664303ae6e2d0e567`。

关键原始记录均在 VM `~/dt-agent-all/validation-results/`：

- `static-step3-progress.json`：全部串行任务、返回码、预期返回码和耗时，最终均符合预期。
- `static-step3-corpus-summary.json`、`static-step3-corpus{,2}-json/`：完整文件集合、优化前后覆盖数及结构基线。
- `static-step3-calls/`、`static-step3-guests/`、`static-step3-exits/`：IR、驱动、原生产物、每输入快照和差异。
- `static-step3-negative-{memory,fpsw}/`、`static-step3-{ctest,noz3-ctest}.log`。
- `static-step3-real-*.log`、`static-step3-table-summary.json`、`static-step3-unlifted-*.log`。

### 覆盖范围和剩余部分

本次交付的是可优化的静态 IR；未知外部调用保留为保守声明，不执行目标程序或实现系统库。
仅在验证程序中提供独立外部实现。真实库的完整运行、普通 C ABI/参数类型重建、
通用反平坦化/去虚拟化、动态生成/自修改代码、把 IR 回写为可替换原二进制，不在本次验收内。
已列出的未 lift 指令仍显式 Unsupported；未知外部影响相关的分支不强行消除。
本项目仍是分层差分验证，没有形式化正确性证明。

复现静态语料验证（默认同时检查产品优化输出）：

```sh
cd ~/dt-agent-all
LLVM_MODE=static TMPDIR="$HOME/dt-agent-all/validation-tmp" \
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
RESULTS_DIR="$HOME/dt-agent-all/validation-results/static-corpus-json" \
~/ucdev/bin/python3 scripts/validation/corpus_check.py ~/bench/corpus
```

第二套将 corpus/RESULTS_DIR 改为相应独立目录；原生函数验证分别运行
`function_static_diff.py`、`function_static_guest_diff.py`、`static_exit_contract.py`，
使用 `--trials 8 --out <独立结果目录>`，保持同一库与 TMPDIR 环境。
