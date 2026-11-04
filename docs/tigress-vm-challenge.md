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
- 不能解析的有三类，都是**保守的未完成**，没有猜测目标：
  1. 无守卫的跳转表（plain：`% 7` 让编译器省掉 `cmp/ja`）。仅凭索引表达式的取值范围
     读表并不可靠（Lua 的 `& 0x7f` 表只有 83 项），这是仓库既定规则。
  2. `goto *jumpTab[*vpc]`：索引是从字节码内存读出的字节，静态界限只有 0–255，不能当表长。
  3. 间接 Flatten 的 `goto *tab[state]`：状态变量跨循环回边，需要值集分析。
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
