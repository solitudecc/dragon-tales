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

### 结果（两种策略相同）

C 336/336；LLVM **1296 match / 720 incomplete / 0 different**；动态 CFG wrong 0。
修复前分别为 0 个 f-plain 匹配（第 1 个缺陷），以及 1008 / 1008（修复 1 之后、修复 2 之前）。

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
- **f-opaque O2：指令重叠。** `junk` 谓词在假分支里放入 `26 36 14 57 ab e8 a5 f2`，真实代码从其中
  第 8 字节（0x102b8）开始。两种策略都把垃圾字节解码成一条 `call`，吞掉了 0x102b8，于是这个
  跳转目标没有指令。Dragon 每个地址只有一种解码，支持重叠需要改 Graph/基本块结构，未做。
- **goto / indirect 分发**：`goto *tab[state]` 没有任何边界检查，需对状态变量做值集分析。

去混淆效果：f-combo 恢复了完整 CFG 且执行逐位一致，但 LLVM O2 后 IR 仍是 plain 的 4 倍
（17346 vs 4321）：平坦化分发器与 MBA 没有被还原，多数链表谓词（依赖堆状态）也无法局部判定。
