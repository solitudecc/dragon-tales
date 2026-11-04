# Tigress 混淆函数的语义检测

2026-11-04 接入 [tigress_function_diff.py](../scripts/validation/tigress_function_diff.py)。
本轮增加检测工具和回归，修复检测发现的 x86 填充、间接分发 case、常量数据入口误报及 i386 尾声识别。
既有接口、IGNIL CALL/RET 约定、指令执行语义及 LLVM 函数名字保持原样。
结果包含明确的覆盖缺口，**Tigress 全量检测未通过**；不能把下面的完整返回匹配数当成所有样本通过。

## 工具来源及范围

使用公开的 [JonathanSalwan/Tigress_protection](https://github.com/JonathanSalwan/Tigress_protection/tree/382a8e9e9aacad553f0b3c228e2b76077ff0ced2/tigress-2.1)
固定提交 `382a8e9e9aacad553f0b3c228e2b76077ff0ced2` 内的旧版工具。
实测版本：`Tigress Linux-x86_64-2.1, revision 2128, build 2017-04-11 16:56:50+00:00`。
这不是新版 4.x 的测试，也没有宣称 Tigress 已公开完整源码。
[当前官方分发说明](https://tigress.wtf/tigress-download.html)。

在 ARM64 Ubuntu VM 的独立仓库 `~/dt-agent-all` 验证。
旧版生成器的 `cilly.asm.exe` 通过用户目录内的 QEMU x86-64 执行，使用
`CILLY_NATIVE_CC=x86_64-linux-gnu-gcc` 预处理；原始文件校验值保存在
`~/bench/tools/tigress-2.1-github/original-sha256.json`。
QEMU 只运行混淆生成器，不充当下面 LLVM 对拍的执行参考。
Tigress、QEMU 二进制均不加入仓库。

## 检测如何判定

1. 生成定义明确的 C 函数：无符号 32 位整数运算、条件、最多 7 次的循环、真实 helper 调用，
   返回一个整数并写入四个整数。另一个 Python 实现按模 2^32 计算独立规格。
2. 三个种子 `1901..1903`；原版、Flatten/switch、Flatten/goto、EncodeArithmetic、AddOpaque、
   AddOpaque→EncodeArithmetic→Flatten/switch 六种配置。完整参数逐条写入 `commands.jsonl`。
   2.1 的不透明谓词配置先 InitEntropy，再 InitOpaque/array，数组大小 4，AddOpaqueKinds=true、Count=3。
3. Clang 21.1.8 分别按 C `-O0/-O2` 编译成固定低地址的 x86-64 ELF，不剥符号。
   装载完整的已分配代码、只读数据、数据及零初始化 BSS；不重定位代码字节或猜测表地址。
   生成器引入 memcpy 时，链接并执行测试内的实际 Guest C 实现，没有宿主效果桩。
4. 第一层：Unicorn 分别执行原版与变换后机器码，比较返回值、四个输出整数；原版还要与 Python 规格一致。
   不比较不同二进制的寄存器分配、代码字节或栈布局。
5. 第二层：每份可执行的变换后机器码分别经 V2 O0/O2、静态 O0/O2、产品静态优化后 O0/O2，
   编译为 VM 上的原生代码。对照该份机器码自身的 Unicorn 执行结果，比较全部 76 个已建模字段、
   完整 65536 字节内存及 Exit。静态模式的测试分发器在 LLVM 优化后才链接，只执行实际降级的 Guest 代码。
6. 两种分析策略独立运行；以编译产物的 STT_FUNC 符号为已知函数入口，统计漏入口及多出的入口。
   符号真值只用于检查，不作为补丁注入分析图。没有用框架自己的 CFG 反过来定义真值。

每配置 16 个输入，包含 0、1、全 1、32 位符号边界、8/16 位边界、不同分支及 0..7 循环次数。
入口 DF=0、栈按 SysV 对齐；相同机器码的所有状态和内存均保持严格比较。
根包装器最后执行 CMP，定义被比较的算术标志；无未定义字段排除，无 Legacy 归一化。
未专门构造向量或 x87 运算家族，初始向量及 x87 状态仍参与整状态比较。

显式 Transfer/NeedTarget/Unknown/Unsupported/Budget 在根返回前发生时，记为 **incomplete**，
不拿它的中途快照与完整函数输出比较，也不算语义匹配。
参考执行失败记为 reference_error，对应 LLVM 比较明确列入 unexecuted。
这些情况以及入口错误都会使正常命令返回 1；没有把缺失工具、编译失败、异常或未完成计成成功跳过。

## 修复前的检测结果

每种策略的计划规模为 36 个编译配置、576 次 C 层检查、3456 次 LLVM 对拍。

| 项目 | 线性策略 | 递归策略 |
|---|---:|---:|
| 原版与 Python 规格一致 | 96 | 96 |
| 混淆版 C 可观察结果一致 | 416 | 416 |
| 混淆版参考执行异常 | 160 | 160 |
| LLVM 完整根返回且全部状态匹配 | 1808 | 1920 |
| 完整返回后的语义差异 | 0 | 0 |
| 根返回前显式退出，未完成验证 | 688 | 576 |
| 因参考异常而未执行的 LLVM 比较 | 960 | 960 |
| 成功跳过 / 排除字段 | 0 / 0 | 0 / 0 |
| 完整比较的字段数 | 137408 | 145920 |
| 完整比较的内存字节数 | 118489088 | 125829120 |

实际完成的 4992 次 LLVM 执行中，3728 次完整返回且匹配，1264 次提前退出。
另有 1920 次计划对拍因参考异常未执行。生产 LLVMLifter SHA256 仍为
`aa2bb840993c2773b69d4281088265d895ffe4bac68bdee664303ae6e2d0e567`。

每种策略中，按编译配置汇总的部分结果如下：

| 配置 | 线性：完整匹配 / 未完成 | 递归：完整匹配 / 未完成 |
|---|---:|---:|
| 原版 O0/O2 | 576 / 0 | 576 / 0 |
| 算术编码 O0/O2 | 576 / 0 | 576 / 0 |
| Flatten/goto O2 | 288 / 0 | 288 / 0 |

其余平坦化配置保留提前退出记录，不能外推为任意平坦化函数可完整处理。
线性策略在 26 个可执行编译配置中多报 27 个函数入口，全部 26 项入口检查失败；
递归策略 26/26 项入口检查通过，两种策略均无符号入口遗漏、无 unlifted 指令。
绝大部分线性误报来自链接器的 INT3 填充，其中也有分发区域内的错误分割。

## 新发现的失败样本

### INT3 对齐填充误报及修复

未混淆基准 `1901-plain-O0` 的 helper 在 `0x102c` RET，lld 在
`0x102d..0x102f` 填入三个 INT3，真正的 kernel 从 `0x1030` 开始。
线性策略却额外报告 `0x102d` 为函数。LLVM 的 INT3 描述没有 isTrap 标记，
原分析只依赖该标记，未把这类填充交给已有的跳过逻辑处理。

修复位于 `lib/disassemblers/x86/x86Analysis.cpp`：只在已识别尾声之后跳过填充时识别单字节 `0xcc`。
没有把 INT3 改写为 NOP；本修复只涉及填充的边界推断，执行到 INT3 的异常语义不属于本轮验证范围。
还修正了 x86Info 中 i386 RET32/RETI32 的 LLVM 21 操作码名称识别，只影响尾声发现。
`[int3-padding]` 的五项 C++ 回归在原始后台快照上均失败；
最终 i386/x64 的 30 项断言通过，覆盖填充后的真实叶函数入口、显式 INT3 入口、RET imm16、
未知间接分发后的 case、不同已知函数区域，以及读取代码字节时显式入口仍能保留。
旧代码输出保存在 `tigress-origin-five-tests.log`；完整旧后台及 C shim 快照位于 `tigress-origin-backend/`。

中间一次把 INT3 加入全局陷阱分类的修复过宽，导致三项已有 SwitchDetector 测试失败
（resolved CFG edges、loop-invariant base、32-bit bounds across joins）。
该改法未保留：其他位置的 INT3 不改变分类。
仅跳过填充仍会把分发表 case 误认成函数，另有一项既有测试检出了这个问题。
最终以尾声猜测前已有的入口候选划分区域；该区域有未解析间接跳转时，
不凭 RET 后的布局推测新函数。既有的显式入口、调用目标、序言入口候选仍保留，
其他已知函数区域独立处理，case 块由跳转解析建立。
失败输出保存在 `tigress-fixed-ctest.log` 和 `tigress-release-switch-tests.log`。
最终五项新增回归和全部 32 项 SwitchDetector 测试（199 断言）通过。

未 lift 核对还检出了一个中间回归：i386 的记录由 188 增至 194，新增四条 AAS 和两条
`pop edx` 实际位于 `0x43b8f8/0x43b934` 的 `one/limit` 浮点常量中。
这些位置由 FLD/FSUB/FCOMP 作为数据读取，不能仅凭前面的 RET 判定新函数。
最终对没有其他入口线索、但有明确数据引用的候选保持未知；只使用无索引/无段覆盖的
默认地址宽度绝对或 RIP 相对地址，不猜测地址前缀的效果。
显式入口、调用及序言候选仍保留；回归同时覆盖“读取代码字节”的合法双重用途。
修复后 i386 未 lift 记录恢复为 188，没有通过扩大排除范围或删除指令来降低计数。

仅修 INT3 时，线性策略仍有三个位于 Flatten/switch O0 内部的 case 入口误报。
最终的区域限制也消除了这三处；26 个可执行 ELF 的入口检查在两种策略下均为 26/26 通过，
无多报、无漏报。与原始输出逐文件比较，ELF 字节相同，移除的填充伪入口共 24 处，字节均为 `0xcc`。

## 最终结果

| 项目 | 线性策略 | 递归策略 |
|---|---:|---:|
| C 结果匹配 / 参考异常 | 416 / 160 | 416 / 160 |
| LLVM 完整根返回且状态匹配 | **1920** | **1920** |
| 完整执行后差异 | 0 | 0 |
| 根返回前未完成 | 576 | 576 |
| 因参考异常未执行的 LLVM 比较 | 960 | 960 |
| 函数入口检查 | 26/26 | 26/26 |
| 多报 / 漏报 / unlifted | 0 / 0 / 0 | 0 / 0 / 0 |
| 比较字段 / 内存字节 | 145920 / 125829120 | 145920 / 125829120 |

合计 **3840** 次完整状态匹配，**1152** 次未完成，**1920** 次计划对拍因参考异常未执行。
线性策略比原始实现增加 112 次完整执行匹配；没有把提前退出的快照当作完整输出。
未完成项均来自 Flatten/switch O2 和 Flatten/goto O0 的未恢复分发，每种策略各 288 次。
原版和算术编码 O0/O2、Flatten/switch O0、Flatten/goto O2 的全部受测输入均完整匹配。

### 原错误分割及仍未恢复的分发

线性策略首例 `1901-flatten-switch-O0 / static-O0 / trial=0`：

```text
Exit = [2, 1, 0x1045, 0x110b, 1, 0, 0]   # Transfer/Branch
```

该地址原来被分析为额外函数入口，静态调用沿契约传播跨函数跳转。
最终修复后这一错误分割消失，该配置已完整执行并对拍。
O2 switch 首例 V2 仍在 `0x1066 -> 0x10e8` 返回 NeedTarget；这是未恢复完整分发表的覆盖缺口。
这些结果不能证明整个函数语义正确，也不能直接称为完整执行后的寄存器错误。
没有为了完成对拍而注入执行轨迹中的边、合并猜测目标或绕过保守出口。

### 旧版不透明谓词输出包含除零

三个种子有 10 个编译配置触发参考异常，每个配置 16 个输入，共 160 次。
首例 `1901-opaque-O0 / trial=0` 原始诊断：

```text
Unhandled CPU exception (UC_ERR_EXCEPTION)
PC = 0x1092; bytes = f73c252c200000
idiv dword ptr [0x202c]
```

生成的 `_2_kernel__opaque_array[12]` 只有 11 个显式初始化项；第 12 项零初始化，
机器码把它用作除数。对这一份生成 C，另外在 Mac/Rosetta 用 x86-64 Clang O0 编译执行，
独立得到返回码 `-8`（SIGFPE）。该例确认不是 Unicorn 单方报错。
其他异常的 PC/字节也逐条保留，未声称全部做了第二参考复验。
不手改 Tigress 的输出消除这个失败，不据此归咎于框架。
部分 O2 配置可返回并匹配，只代表这些具体机器码/输入；不代表上述 C 输出不存在未定义行为，
也不代表编译器保留了不透明谓词的保护强度。

## 检测器的负对照

同样通过 C→ELF→加载→分析→LLVM→原生执行流程：

| 故意错误 | C 层 | LLVM 层 | 普通命令返回码 |
|---|---:|---:|---:|
| c-result：把变换侧返回值 XOR 1 | 32 different | 192 match | 1 |
| llvm-register：只损坏降级执行结果的 RAX bit 0 | 32 match | 192 different | 1 |
| llvm-memory：只损坏降级执行结果的数据字节 | 32 match | 192 different | 1 |

第一行说明：即使 LLVM 忠实翻译了错误样本，独立的原版/规格检查也能拒绝它。
后两行分别确认完整状态和内存比较确实生效。
CTest 用 `--expect-control` 严格检查预期数量、差异所在层、完整根返回和入口检查，
不能把任意非零退出或基础设施异常当成负对照成功。
四个新增 CTest 验证检测框架本身，使用 plain 模式；**它们不代表 Tigress 实际语料通过**。
完整 CTest **491/491**，无 Z3 **342/342**。新增五项 C++ 回归和四项检测框架测试均为正常通过，
没有用预期失败标记隐藏旧代码的问题。

## 真实程序回归

最终记录覆盖 69 个 ELF/PE、138 次策略分析、206651 次函数检查、1686 个 V2 原始 IR 模块；
全部已生成模块通过 verify，lift 失败为 0。最后的数据引用修复后重跑全部 38 个 x86 样本，
31 个未改动的 ARM 样本复用前轮记录；输入校验值和复用来源单独存档。
这只验证 IR 合法性，不是 206651 个函数的执行语义证明。
已有告警未增加：三份 Lua x64 的线性 objdump 缺失各 2；cxx mingw32 O2 的
线性/递归无 IGNIL 块为 2/1、未解码边为 14/13，线性 objdump 缺失 70；
libcrypto 线性无 IGNIL 块 4、objdump 缺失 70。原记录及严格审计均保留。

| 检查 | 最终数字 |
|---|---|
| Lua i386/x64/A64 跳转表，两种策略 | exact 104、wrong 0、未解析 10、无可见边界 76、不可读 0 |
| 三架构真实块 LLVM 原生执行 | 执行 450，严格匹配 449，参考差异 1，编译失败/原生崩溃 0 |
| 真实块跳过 | 高地址 83、Unicorn 异常 57、unlifted 4，共 144 |
| 未 lift 记录，修复前 → 后 | i386 188 → 188、x64 49 → 49、A64 90 → 90，完整形式统计相同 |
| 剥符号 GCC Lua 入口召回 | 两种策略均保持 699/700；线性符号体内部伪入口 135 → 84 |
| Clang Lua 入口召回 | 两种策略均保持 651/651；线性符号体内部伪入口 107 → 83 |

两份 Lua 的递归策略内部伪入口仍分别为 84/83。这些现存误报和 GCC 的一个漏入口没有
在本轮解决，不能把受控 Tigress 的零误报外推为真实程序的零误报。

### 真实块发现的参考差异

最终 i386 抽样出现下列原始差异，完整旧后台针对同一块也产生相同结果：

```text
DIFF block 0x43319d: fld tbyte ptr [eax]; lea ebx, [eax + 12]; fxam ; wait ; fnstsw ax; and ax, 17664; cmp ax, 256
EAX: llvm=0x10000000 unicorn=0x10000400; CF: llvm=1 unicorn=0; SF: llvm=1 unicorn=0
```

输入 80 位值为 `0x7a5c4474d2addbb03b5b`，指数非零、整数位 J=0，属于
[Intel SDM 第 1 卷 §8.2.2、表 8-3](https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf)
列出的 unsupported 编码；§8.3.6 说明 FXAM 区分该类别。
对同一原始值用 Mac/Rosetta 执行 `FLD; FXAM; FNSTSW`，得到状态字 `0x3800`，
分类位 `C3/C2/C0=000`，支持 Dragon；Unicorn dev 则给出 normal 类别 `010`。
该例为经第二参考核实的 Unicorn 分类错误，没有修改指令语义。

`x87_native.c` 加入同一原始值，`TestX87.cpp` 的既有测试新增一节固定正确状态、EAX、CF、SF，
两架构共 12 项断言；此节在旧/新后台上均通过，作用是固定参考错误的正确值，
不是宣称修复了旧 Dragon 的 FXAM。完整真实块比较仍保留这 1 处 DIFF，未增加排除或校正。
日志为 `tigress-final-blocks-x32.log`、`tigress-final-x87-old.log`、
`tigress-final-x87-native.log` 和 `tigress-x87-oracle-{old,new}.log`。
包含新增一节的该项 C++ 测试在旧/新后台均为 73 项断言通过；随后完整 CTest 再次为
491/491、无 Z3 342/342，记录于 `tigress-oracle-final-{ctest,noz3-ctest}.log`。

## 复现和原始证据

工具路径指向已准备好的 Tigress 或环境包装脚本：

```sh
cd ~/dt-agent-all
TMPDIR="$HOME/dt-agent-all/validation-tmp" \
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
CILLY_NATIVE_CC=x86_64-linux-gnu-gcc \
~/ucdev/bin/python3 scripts/validation/tigress_function_diff.py \
  --tigress "$HOME/bench/tools/tigress-2.1-github/run-tigress" \
  --strategy 0 --seeds 3 --trials 16 --out validation-results/tigress-new
```

递归策略用 `--strategy 1`。不依赖 Tigress 检查检测器可用 `--profile plain`。
负对照另加 `--control c-result`、`--control llvm-register` 或 `--control llvm-memory`；
正常运行故意错误版本应返回 1，仅 CI 检查负对照时添加 `--expect-control`。
CLI 也预留 flatten-indirect 和 virtualize，但本轮未运行，不能算已覆盖。

VM `validation-results/` 中保存修复前的 `tigress-linear/`、`tigress-recursive-final/`、
最终实现的 `tigress-final-linear/`、`tigress-final-recursive/`、
`tigress-final-negative-{c-result,llvm-register,llvm-memory}/`，以及构建和 CTest 日志。
每份记录包含生成前后 C、命令、ELF、Guest 支持代码、链接布局、原始/优化 IR、原生驱动、
输入输出快照、入口差异、错误 PC/字节、逐项比较和未执行数量。
结果还记录 C shim 及实际映射的 `libdragon.so` 校验值；旧版对照使用两者的完整快照及
对应 `LD_LIBRARY_PATH`，仅替换 C shim 不能隔离旧版分析代码。
最后一次数据引用修复只影响 x86：最终重跑全部 38 个 x86 样本，31 个未改动的 ARM 样本
沿用前轮已经完成的记录。`tigress-final-corpus-provenance.json` 逐文件保存架构、输入校验值、
是否复用及前轮记录路径，不把复用项称为重新执行。
初次错误探测、旧版参数不兼容及中间试跑的输出也保留，没有覆盖成最终成功记录。

## 未覆盖和遗留

受测样本的 INT3 填充和内部 case 入口误报已修复；尚未恢复的分发仍明确记作未完成，
没有把这些未完成项作为预期通过的产品回归。
尚未验证新版 Tigress、Virtualize、间接 Flatten、并发/JIT/自修改保护、i386/A64 混淆语义、
真实未知外部函数、异常投递或所有输入；没有对切片常量或不可达结论做新的 SMT 证明。
完整 Tigress 配置矩阵使用启用 Z3 的构建；无 Z3 构建运行完整 CTest，没有重复该矩阵。
本轮的入口真值依赖受控编译产物符号，不能证明任意剥符号程序的完整函数边界。
本轮未改动指令执行语义，未重跑全部单指令对拍；函数级执行、CTest、真实块和语料库回归
另行记录，它们也不能替代尚未完成的路径验证。
真实库的本轮 LLVM 全量回归使用 V2 原始 IR 校验；未重跑 69 样本的全部静态产品优化模块。
静态模式覆盖来自本轮受控 Tigress 整函数执行及 CTest，不能把 V2 的 IR 校验称为真实库静态语义证明。
