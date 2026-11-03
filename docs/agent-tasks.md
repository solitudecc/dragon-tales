# Agent tasks（派给其他 AI agent 的任务提示词）

用法：先把"通用前置说明"贴给 agent，再贴对应的任务。每个任务都可以单独派给一个 agent。
任务完成后，在下面的状态表里标记为已完成（或删掉该任务），避免重复派活。

| # | 任务 | 状态 |
|---|---|---|
| 1 | AArch64 浮点和 SIMD lift | 已完成（2026-11-02；420/420 ctest；835 模板对拍） |
| 2 | x86 `rep movs/stos`（及 `cmps/scas`） | 已完成（2026-11-02；428/428 ctest；3960 次串操作对拍） |
| 3 | 剩余的 SSE 和单精度浮点 | 已完成（2026-11-02；422/422 ctest；393 模板对拍） |
| 4 | x87 浮点（分阶段） | 已完成（2026-11-02；439/439 ctest；214 形式 / 27392 次对拍；扩展算术保持 UNDEF） |
| 5 | 剥离符号的二进制的函数入口识别 | 已完成（2026-11-02；412/412 ctest；300 次 fuzz 无崩溃） |
| 6 | 切片器的栈内存跟踪 | 已完成（2026-11-02；407/407 ctest；跳转表 wrong=0） |
| 7 | C shim / Python 暴露 AnalysisHints、callingConvention | 已完成（2026-11-02；403/403 ctest） |
| 8 | LLVM Lift：跨函数控制流与调用边界（详见 docs/llvm-control-flow.md） | 已完成（2026-11-03；六步独立提交；ctest 469/469，无 Z3 320/320；1792 执行匹配；69 样本、203113 函数 verify；跳转表 wrong=0） |

任务 8 的静态模式后续已完成（2026-11-04；三步各自独立提交）：
`STATIC_ANALYSIS=2` 保留未知外部调用，支持内部声明/定义及保守状态同步，
无需提供外部函数实现即可生成和优化 IR。CTest 479/479、无 Z3 330/330；
1728 次执行/声明边界比较一致；69 样本、203113 次函数检查的优化前后 IR 均 verify。
具体退出分类、跳过、负对照及盲区见 [静态模式记录](llvm-static-analysis-validation.md)。

**分配建议**

- 可以并行：任务 1、2、5、6、7 基本互不冲突。
- 会冲突：任务 3 和 4 都要改 `X86IGNILLifter.cpp`；任务 1 和 3 都可能新增 IGNIL 浮点操作。
  这几个建议串行做，或者先由一个 agent 把"FP32 / 新浮点操作"做好合入，其他 agent 再基于它继续。
- 任务 4（x87）最容易为了覆盖率用 double 近似 80 位运算，违反"不输出错误语义"的原则，验收时要重点检查。

---

## 通用前置说明（每个 agent 都要先贴这段）

```
项目：https://github.com/solitudecc/dragon-tales（Fare9/dragon-tales 的 fork）
C++20 / LLVM 21 二进制分析库：反汇编 → IGNIL 中间语言 → 符号执行 / LLVM IR，带 C shim 和 Python(cffi) 接口。
开工前先完整阅读仓库根目录的 AGENTS.md（模型不变量、验证工具、已知的刻意设计差异都在里面）。
同时阅读 docs/correctness.md（怎么确认结果正确：验证分层与盲区、测试框架规则、允许跳过的情况、
和参考实现不一致时的处理、汇报要求），所有改动语义的工作都必须遵守。

环境：
- 测试 VM：访问方式见 AGENTS.md 第 2 节（aarch64 Ubuntu，3GB 内存，8 核）。登录和 sudo 凭据向仓库所有者索取。
- 仓库在 ~/dragon-tales，构建目录 build/（cmake --build build -j6；cd build && ctest -j6）。
- 样本：~/bench/corpus（34 个）、~/bench/corpus2（35 个），x86-64/i386/AArch64 的 ELF/PE 都有。
- 参考模拟器：用 ~/ucdev/bin/python3 跑验证脚本（这是 Unicorn dev 分支，修了 2.1.4 的 BZHI/BLSI bug）。
- 如果有多个 agent 并行：每个 agent 在 VM 上自己 clone 一份到 ~/dt-<任务名>，用独立构建目录；
  内存只有 3GB，同一时间只跑一个重任务（语料库检查、real-block 对拍）。

不可违反的原则：
1. 绝不输出错误语义。拿不准就输出 UNDEF，并调用 Block::noteUnlifted(addr)。
   手册规定"未定义"的结果也要给 UNDEF，不能猜一个值。
2. 参考实现是 Unicorn。和 Unicorn 不一致时，先查 Intel SDM / ARM ARM；
   怀疑是 Unicorn 的错，要找第二个参考来确认（比如 x86 代码在 Rosetta 或真实 x86 上运行的结果）。
3. 每个修复或新增都要有 C++ 回归测试，并确认这个测试在旧代码上失败、在新代码上通过。

验证流程（全部要过）：
- scripts/validation/difftest_x64.py / difftest_sse.py / difftest_a64.py：单指令对拍。
  给新指令加模板，要求 0 差异、0 未 lift。
- scripts/validation/realblock_diff.py BIN [BLOCKS] [TRIALS]：真实基本块的符号执行对拍。
  REALBLOCK_FILTER=<操作数文本> 只挑包含该文本的块；REALBLOCK_OPT=1 同时检查优化器。
- scripts/validation/realblock_llvm.py BIN [BLOCKS]：LLVM IR 编译后原生运行对拍。
  需要先执行 sudo sysctl vm.mmap_min_addr=4096。
- scripts/validation/unlifted.py BIN [N]：统计未 lift 的指令形式，用来给出前后对比。
- scripts/validation/corpus_check.py ~/bench/corpus 和 ~/bench/corpus2：回归检查，
  只允许出现 AGENTS.md 里列出的已知条目。
- ctest 全部通过。

Git 流程：从 main 开分支 → 提交（提交信息写清楚问题、做法、验证结果）→ push 分支 →
git merge --ff-only 合入 main → push main → 删除分支。
不要向上游 Fare9/dragon-tales 开 PR 或推送。

完成后汇报：改了什么、验证结果（各项的数字）、未 lift 的前后对比、遗留问题；
并在 AGENTS.md 里补上新的不变量和工具说明，在 docs/agent-tasks.md 里把任务标记为已完成。
```

---

## 任务 1：AArch64 浮点和 SIMD lift（推荐优先做）

```
任务：给 AArch64 lifter（lib/disassemblers/arm/armIGNILLifter.cpp）补上浮点和常用 SIMD 指令。

先运行 unlifted.py，统计 ~/bench/corpus 和 corpus2 里所有 aarch64 样本未 lift 的指令，按频率排序决定先做哪些。
预期的高频指令：fmov（GPR↔D/S、立即数）、ldr/str/ldp/stp 的 D/Q/S 寄存器形式、fcmp/fcmpe、
scvtf/ucvtf、fcvtzs/fcvtzu、fadd/fsub/fmul/fdiv/fneg/fabs（双精度）、fcsel、frintm/frintp/frintz、
clz、rbit，以及 movi/dup/ins/umov 等常见 SIMD 数据搬移。

要点：
- 先确认 V 寄存器在 IGNIL 里的建模方式（AGENTS.md 说 V 寄存器是 16 字节）。
- 写 D 寄存器（64 位）或 S 寄存器（32 位）时，按架构规定要清零该 V 寄存器的其余高位。
  现有的"32 位写清零高位"规则不覆盖这种情况，必须显式处理。
- IGNIL 现有的浮点操作（FADD/FSUB/FMUL/FDIV/SITOFP/FPTOSI/FLT/FEQ/FUNO）只支持 64 位双精度。
  - 单精度（S 寄存器）运算：需要新增 IGNIL 操作（FP32 运算，或 FPEXT/FPTRUNC 转换），
    同时实现 SymbolicExecutor、LLVMLifter、Z3 三端。不想做就保持 UNDEF。
  - 不能用"转成双精度运算再截断"来近似单精度：存在双重舍入问题，结果可能不正确。
- fcmp 要正确设置 NZCV，包括 unordered（NaN）的情况；fcmpe 和 fcmp 在 IGNIL 层语义相同。
- fcvtzs 遇到溢出或 NaN 时的饱和行为要按 ARM ARM 实现。
- 在 difftest_a64.py 里加模板，输入要包含 ±0、±inf、NaN、非规格化数、溢出边界。
- 用 aarch64 样本跑 realblock_diff 和 realblock_llvm。注意：这两个脚本目前只给 x86 的
  XMM 填随机初值，需要补上 V 寄存器的随机初值和比较。
```

---

## 任务 2：x86 `rep movs/stos`（及 `rep cmps/scas`）

```
任务：lift x86 的串操作指令 movs/stos/lods/cmps/scas，包括无前缀和 rep/repe/repne 前缀的形式。

统计：sqlite x64 里有 84 处 rep stosq、14 处 rep movsq；i686 样本里有 200 处 rep stosd；
zstd 里也有几十处。

要点：
- 无前缀的单次形式：按 DF 决定地址递增还是递减，元素宽度 1/2/4/8 字节，更新 RSI/RDI。
  这部分直接实现。
- rep 形式的计数 RCX 是运行时值，不能展开成固定次数。研究现有的 liftLoop / jrcxz 的处理方式，
  以及 IGNIL 块和 CFG 对自环的支持，然后选一种方案：
  (a) 按硬件语义，把一次迭代 lift 成："RCX==0 跳到下一条指令；执行一次搬移；RCX--；跳回本指令地址"，
      并确保反汇编器、BasicBlock、CFG、LLVMLifter、BackwardSlicer 都能正确处理指令跳回自身；
  (b) 其他你认为更好的方案，在提交信息里写明理由。
- repe/repne cmps/scas 的提前退出条件看 ZF；cmps 要按 sub 的规则设置标志位。
- 32 位地址前缀（addr32）下使用 ECX/ESI/EDI。
- 目前各验证脚本都跳过了 rep 指令（搜索 'rep'），需要放开。
  difftest 用小的 RCX（0、1、2、5），DF 取 0 和 1，并包含源和目标区域重叠的情况。
- realblock_llvm 要确认含自环的 IR 能正确编译和运行。
```

---

## 任务 3：剩余的 SSE 和单精度浮点

```
任务：在现有的 liftVec 框架上补充剩余的 SSE/SSSE3/SSE4.1 指令。
liftVec 在 X86IGNILLifter.cpp 里，XMM 建模为 XMMn（低 64 位）+ XMMnH（高 64 位）两个半边，详见 AGENTS.md。

先用 unlifted.py 统计所有 x64 样本里剩余的 xmm 相关指令，按频率决定顺序。候选指令：
- 打包和饱和运算：packuswb/packsswb/packssdw、paddus/psubus/padds/psubs（b/w）
- 乘法：pmuludq/pmullw/pmulhw/pmulhuw/pmaddwd
- 其他整数运算：pavgb/pavgw、pminub/pmaxub/pminsw/pmaxsw、psadbw
- SSSE3：pshufb、palignr、pabs*、psign*
- SSE4.1：pextrb/d/q、pinsrb/d/q、pmovzx/pmovsx、ptest、pminsd/pmaxsd/pminud/pmaxud、pmulld、blendps/pblendw
- 浮点：cvtss2sd/cvtsd2ss/cvttss2si/cvtsi2ss、单精度标量和打包运算（addss/mulss/addps……）、
  maxsd/minsd（注意 NaN 和 ±0 的非对称规则）、sqrtsd、cvtps2pd/cvtpd2ps

要点：
- 整数类指令用现有的 lanes()/pack() 拼接即可，沿用 VecKind 表驱动的写法。
  内存源形式用 memSrc 处理。
- 单精度浮点需要 IGNIL 支持 FP32（或 FPEXT/FPTRUNC 转换），要同时实现
  SymbolicExecutor、LLVMLifter、Z3 三端。可以先做纯整数部分，单精度另起一个提交。
  不能用双精度近似单精度运算。
- maxsd/minsd：任一操作数是 NaN 时返回第二个操作数；两个 ±0 也返回第二个。
- difftest_sse.py 会比较全部 128 位，给每条指令加模板，输入要包含饱和边界值和 NaN。
  然后用 REALBLOCK_FILTER=xmm 在 x64 样本上跑 realblock_diff 和 realblock_llvm。
```

---

## 任务 4：x87 浮点（工作量最大，建议分阶段）

```
任务：给 x86 lifter 增加 x87 FPU 支持。32 位程序里未 lift 的指令绝大多数是 x87：
sqlite i686 里 fld/fstp/fxch/fldcw 等有约 2000 处。

阶段 1：只做数据搬移和栈操作（必须做到精确）
- 设计 FPU 状态：8 个 80 位寄存器（物理 R0-R7）+ TOP + 标记字 + 控制字 + 状态字。
  ST(i) = R[(TOP+i) mod 8]。
  建议先把 80 位寄存器当原始字节，用两个寄存器（64 位 + 16 位）存放，加进
  x86registers.def 和 registerFile()。
- 精确实现：fld m32/m64/m80、fld st(i)、fst/fstp（m32/m64/m80/st(i)）、fxch、fldz/fld1、
  fild m16/m32/m64、fist/fistp（按控制字的舍入模式）、fnstcw/fldcw、fnstsw ax、ffree、fincstp/fdecstp。
- 80 位和 64/32 位之间的转换只能用在精确的场景：加载总是精确的；存储到 m64/m32 需要舍入，
  这一步要实现正确的舍入（受控制字 RC 控制）。没把握就给 UNDEF。

阶段 2：比较
- fcomi/fucomi/fcomip（设置 ZF/PF/CF），fcom/fucom + fnstsw（设置 C0/C2/C3）。

阶段 3：算术（谨慎）
- fadd/fsub/fmul/fdiv 按 80 位扩展精度和控制字的精度位（PC）计算，结果和 64 位双精度运算不同。
  要么在 IGNIL 里实现 80 位浮点运算，要么保持 UNDEF。不允许用 double 近似。

验证：
- Unicorn 的 x87 基于 QEMU softfloat floatx80，可以作为参考。
- 新写一个 difftest_x87.py：先把 FPU 栈和控制字设成已知状态，执行单条指令，
  比较 ST 寄存器的原始 80 位、TOP 和 GPR、内存。
- 最后在 i686 样本上跑 realblock_diff（目前跳过所有以 f 开头的指令，需要放开）。
```

---

## 任务 5：剥离符号的二进制的函数入口识别

```
任务：对去掉了符号的二进制，从元数据里补充函数入口，改善递归遍历（strategy=1）的结果。

现状：去符号的 PIE 只能通过 GOT 找到 main，递归遍历在 ~/bench/corpus/sqlite_a64_O2_stripped
上只解码出 48 条指令。

要做的：
- ELF：解析 .eh_frame / .eh_frame_hdr 里的 FDE，把 pc_begin 作为函数入口种子；
  另外加上 .init_array/.fini_array 和指向代码段的重定位目标。
- PE x64：解析 .pdata（RUNTIME_FUNCTION），把 BeginAddress 作为入口；
  另外加上导出表和 TLS 回调。
- 入口种子的接入点看 lib/loader/ImageDisassembler.cpp（现在按符号添加种子），
  以及 x86/arm 分析里 AnalysisHints 的入口提示。
- 所有解析都要做边界检查。scripts/validation/fuzz_loader.py 要能通过，不能崩溃或卡死。

验证：
- 用同一程序的未剥离版本作为真值：sqlite_a64_O2 对 sqlite_a64_O2_stripped，
  sqlite_x86_64_O2.exe 对 sqlite_x86_64_O2_stripped.exe，lua_gcc_x64elf_O2 对 lua_gcc_x64elf_O2_stripped。
  用 scripts/validation/fstarts.py 统计函数入口的召回率和误报（误报指入口落在某个函数体内部）。
- corpus_check 不能出现回归；记录分析时间的变化。
```

---

## 任务 6：切片器的栈内存跟踪

```
任务：让 BackwardSlicer（lib/analysis/BackwardSlicer.cpp）支持"存到栈槽、再从同一栈槽读出"的值传递，
比如 store [sp, #N] 之后 load [sp, #N]。

动机：AArch64 Lua 样本里最后一张没解析出来的跳转表，基地址是从栈上重新加载的。

要点：
- 现有的 memoryState 已经实现了"同一基址值 + 不重叠的常量偏移 = 不相交"的规则，以它为起点：
  当某个 store 和 load 的地址、宽度完全匹配，并且中间没有可能别名的写入时，
  把 store 的值转发给 load。
- 遇到 CALL、不透明指令、无法证明不别名的 STORE，都必须让转发失效。
  遇到 join 点时，各条路径的值要一致才能转发。
- 现有的 [join-width] 测试和 guard_truth.py、loop_slice_diff.py、slice_diff.py、table_truth.py、
  truth_rel.py 都要通过；跳转表的解析结果不能出现错误目标（宁可解析不出来）。

验证：用 truth_rel.py 和 table_truth.py 在所有样本上统计跳转表的 verified/wrong/unresolved，
报告前后对比。wrong 必须保持 0。
```

---

## 任务 7：C shim 和 Python 暴露 AnalysisHints / callingConvention（小任务）

```
任务：AnalysisHints（入口提示等）和 callingConvention 目前只在 C++ 和 disassembleImage 里能用。
把它们通过 C shim（c_shim/）和 Python（python/dragon.py）暴露出来。

要点：
- C 接口沿用现有 dragon_* 函数的风格：opaque handle、显式的 create/destroy、错误码。
- Python 侧沿用 Configuration 等类的写法，注意对象生命周期：
  borrowed pointer 不能比所属对象活得更久，参考已有的处理方式。
- 在 tests/python/ 下加测试，并在 tests/CMakeLists.txt 里注册
  （参考 test_robustness.py 的注册方式，要设置 DRAGON_C_LIB 环境变量）。
```

---

## 任务 8：LLVM Lift 的跨函数控制流与调用边界

```
任务：修复 LLVM 降级层（lib/LLVMLifter.cpp）在函数级控制流和调用边界上的问题，
按 docs/llvm-control-flow.md 的六步计划逐步实施，每步单独提交。

先完整阅读 docs/llvm-control-flow.md：里面有问题描述、两个现成的失败用例（A：调用结果被忽略；
B：条件尾调用被降成 unreachable）、main 上已有的基础（LoadedImage::importSlots()、
x86 ret imm16 记录在 IGNIL RET 的 aux）、每一步的目标和验收要求。

要点：
- 第 1 步先写出状态和控制流契约（写进文档），再动代码。
- 函数级执行对拍框架从第 1 步就建立最小版本，每一步先加失败用例再修复。
- 默认保守，不假设被调函数遵守调用约定；给外部函数命名不等于实现了它的语义。
- toy-vm 测试用 LLVM JIT 执行 liftBlock 的结果，CALL/RET 语义变化后要重点回归。
```

