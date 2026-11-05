# LLVM 线性 MBA 化简

基线 `fafea17`，2026-11-05。这一步静态改写 LLVM 整数表达式，不执行目标程序。
Unicorn 和本机编译仅作为验证参考。原 Graph、IGNIL、间接跳转解析策略及 Guest ABI 均不变。

## 为什么链表谓词仍不能直接折叠

实际 Tigress 样本使用 `megaInit` 创建链表，条件包含全局链表指针是否为空。
独立函数的入口内存未知，不能直接假定初始化已经成功。现有 StaticAnalysis 接口还
允许从任意登记的 PC 恢复执行；生成的函数入口有 PC switch，并非仅接受自然函数入口。
这些条件使“之前一定走过某个分支”不能自动成为整个函数的前提。

额外运行 CVP/SCCP/JumpThreading 的实验仅有少量 CFG 清理，未解决这个前提问题，
没有将该实验管线加入产品。入口限定视图、初始化效果与堆不变量仍需单独建模。
本组件先处理不依赖这些前提的线性 MBA 区域，不把链表内容或测试快照中的数据当作常量。

## 改写规则

在同位宽 SSA 区域内识别加减、常量乘法/左移及 AND/OR/XOR 的混合运算，使用
0/1 输入的字长真值签名，构造合取基底的线性组合，并尝试缩放、平移的 Boolean 表达式。
方法参考 [SiMBA 的线性 MBA 论文](https://arxiv.org/abs/2209.06335)，代码为独立实现。
候选合成不是等价证明；只有原区域与候选的完整字长位向量不等式被 Z3 判为 UNSAT，才允许写回。
SAT、unknown、超时和资源耗尽均保留原表达式。

支持 1–128 位整数，默认最多 3 个独立 SSA 输入。不同 Load 事件始终分开；PHI、
freeze、类型转换等边界保持为未知 SSA 值，不假设路径、内存初值或调用效果。
位运算下的算术、非均匀字掩码等可以截断为已有 SSA 边界；不把有限真值样本推广为非线性恒等式。
例如 `x*x-x` 在 0/1 上为零，不能据此改成常量零；该控制样例保持原有行为。

候选的运算数必须减少，新增运算不得超过替换后可删除的运算数。共享中间值可以保留，
同时缩小返回值依赖；可观察的寄存器、标志位和 STORE 不能丢弃。成功后运行标准
InstCombine/DCE，再由现有谓词步骤处理条件。没有删除机器指令记录或修补二进制。

这不是完整的 SiMBA 最优解实现，不保证每个线性表达式都缩到最小；跨 PHI/转换的关系、
非线性乘积、依赖堆状态的条件以及去平坦化仍可能保持不变。

## API 与预算

```cpp
lifter.runMem2Reg();
lifter.runInstCombine();
lifter.runGVN();
const auto mba = lifter.runMBASimplification();
const auto predicates = lifter.runPredicateFolding();
lifter.runDCE();
```

```python
lifter.optimize()  # mem2reg → instcombine → GVN → MBA → predicates → DCE
stats = lifter.run_mba_simplification(
    timeout_ms=100, max_queries=16, max_expression_nodes=128,
    max_variables=3, resource_limit=1000000)
```

C API 为 `dragon_llvm_lifter_simplify_mba(lifter, timeout_ms, max_queries,
max_expression_nodes, max_variables, resource_limit, out_stats)`。空 lifter/out 返回 0，
正常调用返回 1；`dragon_mba_simplify_stats_t` 由调用者提供。C++ 对应类型是
`MBASimplifyOptions` / `MBASimplifyStats`，默认预算与 Python 一致。

查询预算由模块共享，递归深度最多 64，节点数上限钳制到 1024，输入数钳制到 3。
累计展开工作还限制在 `min(max_queries * node_limit * 16, 1048576)`。
零超时、查询、节点或输入预算禁止本步骤改写。无 Z3 构建保留接口，但返回
`available=false` 且不改变 IR；没有将未执行的证明算作成功。根枚举及标准 LLVM
优化仍受模块大小影响，SMT 的单次预算不是整模块的总内存或耗时上限。

统计字段是 `available`、`expressions_visited`、`candidates`、`expressions_simplified`、
`solver_queries`、`unknown_queries`、`rejected_candidates`、`budget_skipped`。
C++ 对应 camelCase。visited/skipped 包括普通代码、位宽边界和变量数量限制，不能按其数量
推算混淆强度；每次 simplified 均有一次完成的 UNSAT 查询。

## 回归和独立参考

8 项 C++ 回归 / 320 条断言覆盖六类恒等式、1/8/16/32/64/128 位、三输入真值函数、
共享计算、STORE 前后独立读取、非线性和字掩码控制、显式 unknown/不支持操作，以及证明预算。
宽位测试显式使用较大的证明预算；生产默认预算不因此扩大。资源预算为 1 的控制使证明
返回 unknown，原 IR 不变。无 Z3 的组件回归为 1 项 / 5 条断言。

新 C++ API 不能在旧头文件上编译，已保存真实编译失败日志。另有仅调用既有 `optimize()`
的 Python 回归，旧库在第一个返回值依赖规模断言失败，新库通过。

`mba_function_diff.py` 使用 36 份固定的 A64/x64 机器码，包含六类恒等式、非线性、
掩码、三输入和跨 STORE 的内存对照；32/64 位、V2/static product、O0/O2、8 个输入：
共 1152 次完整 Guest 返回匹配。独立 Python 规格先核对 Unicorn 的返回值，再逐项比较
原生 LLVM 的全部模型字段（A64 102、x64 76）、64 KiB 内存、PC/SP、退出记录和调用事件。
输入包括零、一、二、全一、符号位、最大有符号数、交替位以及 32 位操作数的非零高半部。
无标志位排除、跳过或 Legacy 归一化。

旧库的相同 1152 次语义比较也匹配；其 16 个 static 模块保留较大的加法/XOR 返回值
依赖，新库均完成缩减。64 位从 4 个运算降到 1 个，x64 32 位从 5 降到 2 个，
A64 32 位从 6 降到 3 个（计数包含宽度处理）。编译器已能化简的 OR/AND 等不记为新增成果。
机器码内存/三输入控制没有保证发生改写；独立读取及三输入合成的成功改写另由对应 C++ 回归覆盖。

两组永久负对照各 32 次，只破坏 LLVM 返回寄存器或内存，每次都只检测到指定字段差异。
基础设施错误不能算作负对照成功。测试记录包含实际映射的双共享库、脚本和固定机器码哈希。
主构建完整 CTest 568/568，无 Z3 444/444。

## Tigress 效果与限制

固定公共 Tigress 2.1 revision 2128、seed 4242，f-mba/f-opaque/f-combo、C O0/O2、
两种策略、product-O0/O2、24 输入：288 次 C 规格和 576 次完整 LLVM 返回比较匹配，
CFG 错误、未提升及 incomplete 均为零。真实 Guest `megaInit` 仍执行，没有注入正确堆字节。

同一批旧 ELF 另以冻结的旧库和新库生成单 kernel 产品 IR；输入哈希、机器块/指令数和
未提升计数相同。两种策略结果一致。下表为一次默认管线的计数，包含生成的 Guest 状态及内存检查：

| 档位 | C 优化 | 旧 IR 指令 | 仅追加标准清理 | 新 IR 指令 | 已证明的 MBA 改写 |
|---|---|---:|---:|---:|---:|
| f-mba | O0 | 4708 | 4708 | 4702 | 11 |
| f-mba | O2 | 3342 | 3342 | 3339 | 1 |
| f-opaque | O0 | 9134 | 9134 | 9134 | 0 |
| f-opaque | O2 | 4472 | 4472 | 4472 | 0 |
| f-combo | O0 | 18526 | 18522 | 18497 | 16 |
| f-combo | O2 | 11592 | 11590 | 11577 | 11 |

默认预算并未化简所有区域，尤其 f-combo O0 已用完 16 次查询预算。表中的单 kernel
统计不等同于整个镜像的 product 模块；改写数也不等于删除了多少个原程序谓词。
“仅追加标准清理”以额外 InstCombine/DCE 代替 MBA，保持后续谓词步骤不变。
f-combo 的条件分支数 O0 为 329→327、O2 为 176→175，标准清理对照也得到相同数量；
这 2/1 个分支的减少不能算作新证明的不透明谓词。MBA 的新增收益是等价表达式改写，
f-opaque 的链表条件仍未消除。

两个真实语料库已完成 **69/69 个输入、138 种策略、206649 个已恢复有效函数检查点**，
raw/optimized 各 **6530 个 StaticAnalysis 模块**验证通过。输入哈希和所有已记录的结构字段
与旧基线一致，提升失败、覆盖遗漏和新增结构差异均为零。32 函数/模块配置保持不变。

全量共 8 次等价查询、**8 次有完成证明的改写**、
0 次 unknown、0 次拒绝。unknown 没有授权改写。
每个模块的候选/查询/改写计数均经审计，实际映射的双共享库哈希与原生语义测试一致。
普通程序中的改写包含编译器表达式、标志位及生成的状态检查，不应全部称作去除了混淆。

IR 合法性不等于整个语料库的语义证明。此次未修改指令提升器、IGNIL 优化器或 Graph
分析，未另跑单指令/IGNIL 基本块全量差分；执行语义覆盖来自上述函数级参考与负对照。
原有未提升、函数入口误报和 objdump 对齐异常仍保留并按旧基线核对。

VM 产物：`~/vmc-runs/llvm-mba-20261105/`。`baseline/` 和 `final/` 冻结新旧双共享库
及接口快照，回放时同时设置 `DRAGON_C_LIB` 和同一完整目录的 `LD_LIBRARY_PATH`。
保留原生输入输出、IR、旧版失败记录、Tigress 与语料原始结果；该目录的 corpus.py 可断点续跑。
