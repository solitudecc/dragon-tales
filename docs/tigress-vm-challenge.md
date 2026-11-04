# Tigress 虚拟化挑战测试

2026-11-04，后端 `4818f61`。脚本 [`tigress_vm_challenge.py`](../scripts/validation/tigress_vm_challenge.py)。
在 [Tigress 函数验证](tigress-function-validation.md) 之外，首次测试 Tigress 2.1（revision 2128）
的 **Virtualize** 系列、间接 Flatten 及多层组合。没有修改任何生产语义或接口。

## 被保护的程序

一个 kernel（`source()` 生成，种子 4242）同时包含：12 轮的稠密 `switch`（7 个分支，O2 下为
无守卫跳转表）、函数指针表分派（4 个函数）、递归 `fact_mod`、64 位乘法与除法/取模、
S-box 查表、嵌套 LFSR 循环、结构体局部变量以及 Guest `memcpy`/`memset` 调用。
独立 Python 规格给出返回值和 `out[0..7]`；24 个输入（16 个边界值 + 8 个随机）覆盖全部 7 个分支。

## 保护档

| 档 | Tigress 变换 |
|---|---|
| plain | 无（对照） |
| flatten-indirect | Flatten，`goto *` 间接分发，随机化并拆分基本块 |
| virt-switch / virt-indirect / virt-direct | Virtualize，switch / 计算 goto / 线索式分发 |
| virt-call | Virtualize call 分发——**Tigress 2.1 内部错误**：`Cannot find the new copy of local variable 's'` |
| virt-hard | 链表不透明结构 + Virtualize indirect + 超级指令（比例 2.0、最长合并 8）+ 重复/随机操作码 + VPC 不透明谓词 + 2 个虚假函数 |
| virt-double | 两层 Virtualize（switch 外再套 indirect） |
| kitchen-sink | 链表不透明结构 + AddOpaque×4 + EncodeArithmetic + Virtualize indirect（超级指令）+ 外层间接 Flatten |

各档分别用 clang-21 O0/O2 编译为独立 x64 ELF。

## 测试框架遇到的问题（都不是 Dragon 的错误）

- Tigress 把常量数组初始化移进 `SBOX_i$nit`，由它生成的 `main→megaInit` 调用。入口包装先执行
  真实的 Guest `megaInit` 再调 kernel（plain 链接空的弱符号）。修正前，凡是走到 S-box 分支的
  输入都与规格不同（每档 20/24），恰好是不走该分支的 4 个输入匹配。没有往内存注入正确字节。
- Tigress 2.1 拒绝对 `unsigned` 做 switch（`vSwitch.integerType`），源码中 switch 变量改为 `int`。
- `array` 型不透明结构声明 12 个元素只初始化 11 个，再以第 12 个（0）为除数，Unicorn 报 `idiv`
  异常；改用 `list` 型，并在 Guest 中提供真实的 bump `malloc`（堆在镜像 .bss）。
- `VirtualizeInstructionHandlerSplitCount`、`VirtualizeEncodeByteArray`+`ObfuscateDecodeByteArray`
  单独使用时 Tigress 崩溃无输出，组合使用时生成访问未映射内存的代码；从 virt-hard 中去掉。
- 虚拟化后的代码超出共享 64 KiB 布局：入口包装留在 0x1000（`compile_image` 依赖它），其余放到
  0x10000 起，两边 Guest 内存均为 1 MiB；DATA/STACK/SENTINEL 不变。Unicorn 指令上限和 V2 分发
  预算改为可配置（默认值不变）。

## 结果（线性 / 递归两种策略结果相同，除注明外）

每档 × C O0/O2：24 个 C 检查，6 个 LLVM 方案（V2 / 静态 / 产品管线 × O0/O2）× 24 次。

| 档 | kernel 指令 O0/O2 | 间接跳转（执行过） | Dragon 解析 | LLVM 结果（每级 144 次） |
|---|---|---|---|---|
| plain | 179 / 163 | 1 / 1 | 0 | incomplete |
| flatten-indirect | 437 / 256 | 2 / 45 | 0 | incomplete |
| virt-switch | 736 / 238 | 3 / 2 | **全部** | **match 144 / 144** |
| virt-indirect | 933 / 422 | 3 / 55 | 0 | incomplete |
| virt-direct | 888 / 366 | 3 / 56 | 0 | incomplete |
| virt-hard | 8885 / 4439 | 3 / 119 | 0 | incomplete |
| virt-double | 1086 / 491 | 3 / 62 | 0 | incomplete |
| kitchen-sink | 2587 / 1152 | 3 / 220 | 0 | incomplete |

每种策略合计：C 384/384 match；LLVM 288 match、2016 incomplete、**0 different**、0 基础设施错误。
动态 CFG 检查（Unicorn 实际执行的每个间接跳转目标 vs Dragon 已解析的目标集）：**wrong 0**。
16 个镜像的函数入口与 STT_FUNC 符号完全一致（含虚假函数），未 lift 指令 0。
线性扫描解码了全部执行过的代码；递归遍历在未解析的分发器之后漏掉执行过的代码
（每个镜像 60–8150 个 PC，virt-hard O0 最多）。

负对照（virt-switch，两级）：改 LLVM RAX 288/288 different；改 LLVM 内存 288/288 different；
C 结果改错 48/48 different（LLVM 仍与同一错误二进制的 Unicorn 一致）。

## 解读

- 能完整处理的：switch 型 VM 分发。分发器有界限检查，跳转表被证明并解析，整套 VM 解释器
  在全部 6 个方案下与 Unicorn 逐位一致。LLVM O2 把 kernel IR 从 45785 条降到 17277 条（O0 镜像），
  但解释循环本身不会消失：字节码在 Guest 内存中，不是编译期常量。
- 不能解析的有两类，都是**保守的未完成**，没有猜测目标：
  1. `goto *jumpTab[*vpc]`：索引是从字节码内存读出的字节，静态界限只有 0–255，不能当表长。
  2. 间接 Flatten 的 `goto *tab[state]`：没有边界检查，需要对状态变量做值集分析。
- **更正**：此前把 plain 的未解析归因于"无守卫 `% 7`"，这是错误的。switch 变量改为 `int`
  后，第 7 个值由 `default` 处理，编译器生成了 `cmp r13d,5; ja default` 守卫；真正原因是
  计算索引（魔数乘法）使 SMT 超时，已由下文修复 1 解决，plain 现在 O0/O2 全部 144/144 匹配。
  修复后线性策略重跑：LLVM 576 match / 1728 incomplete（此前 288 / 2016），C 384/384，wrong 0。
- 指令数膨胀：virt-hard 是 plain 的 50 倍（O0），kitchen-sink 14 倍。

## 复现

```sh
cd scripts/validation
DRAGON_C_LIB=.../libdragon_c.so TMPDIR=$HOME/vmc-runs/tmp ~/ucdev/bin/python3 tigress_vm_challenge.py \
  --out ~/vmc-runs/s0 --strategy 0 --tigress ~/bench/tools/tigress-2.1-github/run-tigress --trials 24
# 负对照：--control llvm-register|llvm-memory|c-result --profile virt-switch
```

每次原生执行写 2 MiB 快照，VM 的 `/tmp` 是 1.7 GB 内存盘：输出和 `TMPDIR` 放在 home 下。
产物在 VM `~/vmc-runs/{s0,s1,control-*}`。

## 函数级：MBA + 控制流平坦化 + 真/假不透明谓词（`--function-level`）

源码改为 `% 9` 加可达 `default`，原程序的 switch 有守卫，未解析可明确归因于混淆。

| 档 | Tigress 变换 |
|---|---|
| f-plain | 无 |
| f-mba | EncodeArithmetic |
| f-opaque | 链表不透明结构 + AddOpaque×6（call/bug/true/junk/question，复制代码轻度混淆） |
| f-cff | Flatten switch（随机化、拆块） |
| f-combo / -goto / -indirect | 不透明结构 → MBA → Flatten（switch/goto/indirect，ObfuscateNext，条件改 compute/flag）→ AddOpaque×6 → MBA |

`junk` 谓词和 `flag` 条件会生成 x86 内联汇编；Tigress 用宿主编译器自检，VM 是 aarch64，
需 `CILLY_NATIVE_CC=x86_64-linux-gnu-gcc`。`compute/flag` 条件无法处理链表谓词的指针比较
（`branch2compare` 内部错误），因此谓词在平坦化之后插入。

### 本轮发现并修复的两个缺陷

1. **计算索引的守卫证明超时**（`GuardProof.cpp`）。`x % 9` 编译成魔数乘法后，三条 SMT 查询
   都在 50 ms 内返回 UNKNOWN，连未混淆的函数都无法解析。修复：UNSAT 查询改为把守卫与索引
   共有的值抽象成自由变量（只会增加行为，抽象不可满足即原式不可满足）；SAT 查询在 UNKNOWN 时
   用确定性的具体输入找证据（满足的赋值即证明）；不从样本推出 UNSAT，`x % 9 <= 8` 这种
   被计算本身蕴含的守卫仍被拒绝。
2. **寄存器状态的平坦化分发器**（`x86Analysis.cpp`）。O2 下状态在寄存器中，解析前只知道入口
   `mov ecx, K`，所有切片都看到常量，守卫无从界定。修复：最后再用只到守卫块的切片，索引是
   守卫块入口的任意值，此时证明的界限对所有入边成立（包括表解析后才出现的）。语料库真值
   exact 1084→1086（zlib i686/x64 O2），wrong 0。

两个回归测试在修复前的代码上都失败。CTest 507/507，无 Z3 356/356；guard_truth、slice_diff 无错误。

### 结果（修复前；两种策略的执行计数相同）

C 336/336；LLVM **1296 match / 720 incomplete / 0 different**；动态 CFG wrong 0。
修复前分别为 0 个 f-plain 匹配（第 1 个缺陷），以及 1008 / 1008（修复 1 之后、修复 2 之前）。
下表的机器指令及 IR 规模采用线性策略；未完成分析的两种策略可能恢复不同大小的 CFG。

| 档 | 机器指令 O0/O2 | 局部判定的假分支 | 分发解析 | LLVM（每级 144） | 优化后 kernel IR 指令 O0/O2 |
|---|---|---|---|---|---|
| f-plain | 173 / 157 | 0 / 0 | 是 | 全部 match | 4321 / 2901 |
| f-mba | 360 / 183 | 0 / 0 | 是 | 全部 match | 3535 / 2399 |
| f-opaque | 426 / 250 | 1/19、7/18 | 是 | O0 match；O2 未完成 | 8954 / 4373 |
| f-cff | 261 / 218 | 0 / 1 | 是 | 全部 match | 7292 / 3934 |
| **f-combo** | **5935 / 965** | 3/19、3/15 | 是 | **全部 match** | 17346 / 11500 |
| f-combo-goto | 5664 / 737 | 1 | 否 | 未完成 | — |
| f-combo-indirect | 10647 / 1042 | 1 | 否 | 未完成 | — |

剩余的未完成：
- **f-opaque O2：入口恢复与汇合边界。** `junk` 谓词放入垃圾字节，线性扫描将
  `0x102b5..0x102b9` 解码为 CALL，遗漏直接分支目标 `0x102b8`。
  递归策略已正确解码 `0x102b8`，但两个重叠路径在 `0x102ba` 汇合后没有拆出公共块，
  `0x102b0` 和 `0x102b8` 两个块都以同一条 `0x102db` 的 JA 结束。表跳转 `0x102e1`
  因此有两个守卫前驱，被单守卫证明规则拒绝。此前“两个策略都漏解码、需要修改 Graph”的
  归因错误：Graph 支持不同起点的重叠指令，BasicBlock 已按精确地址与指令长度遍历。
- **goto / indirect 分发**：无守卫的状态分发仍需要值集分析，但首个停点还包括内部调用
  写回的栈状态、栈上跳转表、常量表达式目标和固定地址指针加载，不能统一归因为寄存器汇合。

去混淆效果：f-combo 恢复了完整 CFG 且执行逐位一致，但 LLVM O2 后 IR 仍是 plain 的 4 倍
（17346 vs 4321）：平坦化分发器与 MBA 没有被还原，多数链表谓词（依赖堆状态）也无法局部判定。

### 修复路线与验收（2026-11-04 开始）

目标是静态分析与 IR 的正确恢复；Unicorn 只作为独立验证参考。每阶段单独分支、回归、验证和提交，
不从一次运行观察到的目标集合推断静态完备性。

| 阶段 | 范围 | 完成标准 | 状态 |
|---|---|---|---|
| 1 | 明确控制流入口的补充解码、重叠路径公共后缀分块、受影响 IGNIL 重建 | i386/x64 最小回归先在旧代码失败；f-opaque O2 两策略 × 六方案 × 24 输入完整返回并全状态匹配；相关测试及真实语料库无新错误 | 已完成 |
| 2 | 常量表达式跳转目标、已证明稳定的固定地址控制流指针 | 常量求值不依赖可选目标枚举；写入、别名、未知入口不能被当成固定镜像值；分别报告首个及后续停点 | 待实施 |
| 3 | 有限值集、栈上表、内部调用的内存效果 | 每条入边都有保守结果；未知路径保留未知；候选单调增长及最终完备性复验须先明确设计 | 待设计 |
| 4 | kernel 内动态构造的链表及依赖堆的状态 | 不注入正确堆快照，不按样本名称建模；完整函数与独立规格、Unicorn 逐层比较 | 待实验 |
| 5 | 谓词写回 IR、线性 MBA 化简、去平坦化 | 每次改写有等价证明；真实分支和无法证明的路径保留；同时衡量 IR 简化与完整执行 | 后续 |

现场位于 VM `~/vmc-runs/fl-{s0,s1}`；诊断原始记录位于
`~/vmc-runs/context-audit-20261104`。f-opaque O2 的两个镜像相同，SHA256 为
`9995b60ca0d5a3643ea6d3af0b7dbeab40acce1394f6caa6f1f21c85771ddc20`。
重新做了 24 输入 × 两策略的 48 次 Unicorn / 独立规格检查，全部匹配；重放原有六方案
共 288 次，输出与原记录逐字节相同，仍是未完成结果，不计为修复通过。

汇合点诊断采用通用规则：找出两个顺序解码路径具有相同精确后继的位置，在内存中的图添加
该边界并重新分析。发现 `0x102ba` 后，四个切片深度（5/16/64/1）都从 0 案例变成 6 案例；
仅重复分析的对照仍为 0。独立读取表及 Unicorn 观察的六个目标一致。
该干预实验只证明分析层面的原因；修复后的完整执行数据另列于下文。

后续阶段必须保留的具体现场：

| 样本 | 首个未完成跳转 | 当前证据 |
|---|---|---|
| f-combo-goto O0 | `0x1574e → 0x118d4` | 切片仍含栈状态 LOAD 与栈表 LOAD；状态经 `kernel_QUESTION_2_1/COPY` 的指针参数写回，CALL 阻止内存转发 |
| f-combo-goto O2 | `0x103d8 → 0x106be` | 入口切片为 `ZEXT(0x106be:i32)`；默认未启用自由目标枚举。诊断开启枚举后先建立候选，扩展后复验不能证明完整集合，候选被撤回 |
| f-combo-indirect O0 | `0x1845f → 0x11c88` | 仍有栈状态 LOAD 和栈表 LOAD，未获得有限常量集合 |
| f-combo-indirect O2 | `0x1017a → 0x103e0` | `mov eax,0x12200; jmp [rax]`，目标切片为固定 `.rodata` 地址的 LOAD，尚未读取指针建立边 |

组合样本中的链表由 kernel 内部 malloc 循环构造；`megaInit` 仅初始化 SBOX。
因此仅分析 megaInit 执行后的初始快照不足以覆盖它们。

下一阶段的设计约束：

1. 先处理纯常量表达式，明确每步整数位宽、截断和扩展；不依赖开启自由目标枚举来识别
   `ZEXT(Const)`，不把 LOAD 或 UNDEF 当作常量。
2. 固定地址指针的字节可由 `readMemory` 提供，但该回调本身不是“该内存不会变化”的证明。
   必须先明确稳定内存的契约和写入/别名/调用失效规则，再接入目标解析。
3. 有限值集宜用独立接口表示，保留现有 `slice()` 在常量不一致时返回未知的约定。
   域需区分不可达、完整有限集合和未知；达到容量/迭代/求解预算时返回未知，不公布截断集合。
   CFG 扩展后重新计算入边，单调增长的候选仍须在最终闭包上复验完整性。
4. 内部调用摘要先覆盖已经定位的状态指针写回与栈上表，记录实际内存效果与别名条件。
   未分析或不满足条件的调用继续作为屏障。动态链表再单独建立分配对象和循环不变量模型。

### 阶段一实现与回归

`X86Analysis` 沿已有直接跳转、调用及显式入口恢复缺失的解码路径，保留原来的重叠指令。
对具有相同精确顺序后继的路径建立公共块；后续表解析扩展图时也应用同一规则。
新边界的 IGNIL 重建沿精确顺序前驱回溯所有受影响的旧块，不能只重建数值地址最近的块。
单守卫证明、推测隔离、候选撤回和最终完整复验均保持原有规则。

回归与负对照：

- 仅将五个新 C++ 测试加入 `773196f`，5 个测试中 4 个失败，17 条断言失败；
  不凭控制流证据解码立即数字节的负对照通过。修复后 5/5，209 条断言全部通过。
- 中间版本只补入口和汇合块时，延迟发现的重叠路径仍使旧 IGNIL 残留公共后缀，
  新增的该项测试报出 20 条过期操作；加入精确前驱重建后通过。
- i386/x64 的两个持久完整函数夹具加入 `llvm-guest-flow.json`，没有新增排除字段。
  在旧后端上，32 次 V2 比较中 12 different / 20 match（漏解码路径的退出 PC、状态与参考不同）。
  新后端 V2 32/32、静态产品管线 32/32，均正常根返回，全寄存器和 64 KiB 内存一致；0 跳过。
- CTest 512/512；无 Z3 且启用 Unicorn 的独立构建 409/409。后者比此前关闭 Unicorn 的
  无 Z3 配置包含更多测试。新构建初次选到系统 Python，`RET 0xffff` 的参考检查导致两个
  CTest 失败；保留原日志，改用仓库规定的 `~/ucdev/bin/python3` 后完整重跑通过，未改比较规则。
- `guard_truth` 有/无 Z3 各 27 CFG / 20736 次 Unicorn 执行，0 错；
  `slice_diff` 3000 match，内存切片 448 次 / 192 独立切片、循环切片 128 次 / 4 独立切片，0 错。
- 标准 Lua 三样本的未提升记录及完整形式统计逐项一致：i386 188、x64 49、AArch64 90。
  未改变指令执行语义、公开 API、CALL/RET 抽象或参考排除规则。

真实 f-opaque O2：两策略 × 六 LLVM 方案 × 24 输入的 **288 次完整返回全部匹配**，
0 差异、0 未完成、0 跳过。执行但未解码的 PC 在线性策略从 1 降至 0，递归从 61 降至 0；
完整 kernel 的产品 O2 IR 均为 4361 条。递归旧结果 3174 条来自不完整 CFG，不能当作更好的简化。

失败输出要点（完整原始日志在诊断目录）：

```text
old-regression.txt:
  REQUIRE( g.get(0x1006) )                     with expansion: nullptr
  REQUIRE( g.isValidBlock(0x1009) )            with expansion: false
  test cases: 5 | 1 passed | 4 failed
  assertions: 66 | 49 passed | 17 failed
join-only-regression.txt:
  CHECK( op.address < 0x110a )                with expansion: 4364 (0x110c) < 4362 (0x110a)
  test cases: 5 | 4 passed | 1 failed
  assertions: 228 | 208 passed | 20 failed
ctest-fixed.log (system Python/Unicorn):
  x86_64_root_ret_max: RSP got 0x1c807 expected 0xc807
  x86_32_root_ret_max: ESP got 0x1c803 expected 0xc803
  2 tests failed out of 512
```

`corpus2/cxx_mingw32_O2.exe` 的冻结旧库 / 新库对照：线性指令数 73355→73357，
块数 13252→13253，缺 IGNIL 块 2→1，未解码边 14→13，函数数仍为 1967；递归结构不变。
消失的是块边 `0x439fef→0x43a004`（跳转指令在 `0x439fff`）。该位置属于 `.text` 中嵌入的
`.gcc_except_table` 数据，新增解码不代表真实代码覆盖改善，不能计入语义成功数；
其余 13 条未解码边和 1 个缺 IGNIL 块仍按已有基线异常保留。

### 阶段一最终矩阵与真实语料

重新生成全部七档、两个 C 优化级、两种策略、六个 LLVM 方案，每种策略 C 336/336；
LLVM **1440 match / 576 incomplete / 0 different**，动态 CFG wrong 0。两策略合计
672 次 C 检查、2880 次 LLVM 完整匹配、1152 次未完成，0 差异、0 基础设施错误、0 跳过。
剩余未完成均为 f-combo-goto / f-combo-indirect，不能将整个矩阵称为通过。
两种策略的 f-opaque O2 ELF 与原失败样本的完整 SHA256 一致。

| 语料 | 样本 | 策略记录 | 函数检查 | V2 原始 IR 模块 |
|---|---:|---:|---:|---:|
| corpus | 34 | 68 | 91517 | 755 |
| corpus2 | 35 | 70 | 115132 | 931 |
| 合计 | 69 | 138 | 206649 | 1686 |

全部有效函数均提升并通过 `opt -passes=verify`，0 提升拒绝/失败，0 缺失记录或超时；
69 个输入的 SHA256 均与此前语料记录一致。本轮全部重新执行，没有复用 ARM 结果。
两个语料的跳转表真值统计与冻结前记录一致：exact 1086 / wrong 0 / unresolved 190 /
nobound 526 / unreadable 38；后面三类不计为验证成功。

较早的 Tigress 语料记录与当前后端有 24 个样本的结构差异。对这些样本的两种策略
另用冻结的完整旧库重跑，共 48 组对照；唯一的本次变化为上文 C++ 异常表数据的补充解码。
例如剥符号 SQLite x64 递归函数数，历史为 2086，本次前后均为 2084；不是本次漏掉两个函数。
保留原有告警：Lua 三个 x64 PE 的线性 objdump 地址缺失各 2；libcrypto 线性缺 IGNIL 块 4、
objdump 地址缺失 70；C++ i386 的缺块及未解码边见上文。没有把这些基线告警抹掉。

产物在 VM `~/vmc-runs/context-audit-20261104/`：`fixed-s{0,1}/` 为完整混淆矩阵，
`corpus{,2}-json/` 为真实语料，`tables-fixed.json` 为跳转表统计；
`controlled-structure-summary.json`、`final-summary.json` 保存对照、输入和两个库的校验值。
旧库快照同时包含 libdragon_c.so 与 libdragon.so，使用对应 LD_LIBRARY_PATH 隔离。
开发与验证使用独立工作区 `~/dragon-tales-overlap`；原 VM 工作区及其未提交修改保留。

覆盖边界：完整函数语义来自本轮具体输入的 Unicorn 对拍，未穷举所有输入；真实语料这里只验证
V2 原始 IR 合法性，不能据此证明全语料语义或任意剥符号函数恢复正确。未重复完整无 Z3 Tigress
矩阵、全套单指令/真实块对拍或真实语料的全部静态优化模块；本轮未修改指令语义，新增的 i386/x64
完整函数夹具和上述矩阵用于覆盖实际改变的 CFG。没有新增参考排除或成功跳过。
