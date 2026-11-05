# LLVM 整数谓词折叠

基线 `249e53e`，2026-11-05。这个组件把“可证明条件恒定”接到实际 LLVM 改写，
属于原计划中的谓词折叠阶段。它是静态分析，不运行目标程序；Unicorn 和原生编译只用于验证。

## 行为与范围

对非恒定的标量 `br` / `select` 条件构造整数位向量公式，不添加路径假设，分别查询
“条件不等于 false/true”是否存在反例。仅 UNSAT 允许写入对应常量；SAT 不是折叠依据，
unknown、超时、资源耗尽、不支持的表达式均不触发 SMT 改写。精确符号简化得到的常量
和 LLVM 已有的字面常量条件也可以清理。

随后运行标准 LLVM SimplifyCFG、InstCombine、DCE，修复 PHI、清理不再需要的分支、
选择和计算。后续标准 LLVM 优化仍可正常改写其他表达式。原 Graph、IGNIL、反汇编入口、
跳转表目标集合不改变；输出为 LLVM IR，没有二进制补丁或独立可执行文件重建。

支持 1–128 位标量整数的 ADD/SUB/MUL、AND/OR/XOR、有符号/无符号比较、
ZEXT/SEXT/TRUNC、select，以及范围内的常量移位。不同 SSA Load、PHI、参数和 freeze
作为独立未知输入；同一 SSA 值保留身份。不按内存地址合并两次读取，不使用镜像里的
初始数据，不跨调用假定内存或寄存器不变，也不利用前驱守卫缩小输入范围。

首版不展开 PHI，不处理除法/余数、FP、调用结果、原子或 volatile load、变长/越界
移位、带 exact 的移位，以及带 NSW/NUW 的相关算术。直接遇到 undef/poison 时拒绝
降低；freeze 的结果则是已固定的任意输入。使用 LLVM 的值与优化语义，不修改 Guest
UNDEF 或 CALL/RET 契约。有关 undef/poison/freeze 的区别见
[LLVM UB 文档](https://llvm.org/docs/UndefinedBehavior.html)。

此处能处理乘法参与的简单恒定谓词，例如 `((x*(x+1))&1) == 0`；这不等于通用非线性
MBA 化简。依赖 Tigress 链表、初始化后的堆内容、状态变量值集合或跨块路径约束的条件，
仍可能保持未知。本组件不实现去平坦化或通用 MBA 表达式重写。

## API 与预算

```cpp
lifter.runMem2Reg();
lifter.runInstCombine();
lifter.runGVN();
const auto stats = lifter.runPredicateFolding();
lifter.runDCE();
```

```python
lifter.optimize()  # mem2reg → instcombine → GVN → MBA → predicate folding → DCE

# 或单独运行，查询统计、调整预算：
stats = lifter.run_predicate_folding(
    timeout_ms=25, max_queries=64, max_expression_nodes=256,
    resource_limit=200000)
```

C 接口是 `dragon_llvm_lifter_fold_predicates(lifter, timeout_ms, max_queries,
max_expression_nodes, resource_limit, out_stats)`。空 lifter/out 返回 0，正常调用返回 1。
C++ `PredicateFoldOptions` 与上述四个 Python 默认值一致；C 接口逐项传入这些预算。
后续加入的 [MBA 化简步骤](llvm-mba-simplification.md) 位于 Python 管线的 GVN 与
谓词折叠之间；本页历史测试数字仍记录谓词折叠单独引入时的结果。

查询数由整模块共享，递归展开深度限制为 64、宽度最多 128、每个条件最多 256 个节点；
总节点工作还限制为 `max_queries * max_expression_nodes`。每个候选 solver 使用指定
的 Z3 resource limit，另有逐查询超时。零超时或零查询预算禁止 SMT 查询，但已有的
字面常量条件仍可清理。资源耗尽没有“猜一个结果”的后备路径。

统计字段为 `available`、`conditions_visited`、`conditions_folded`、`solver_queries`、
`unknown_queries`、`unsupported_conditions`、`budget_skipped`，C++ 对应 camelCase。
folded 计数包含已存在的常量条件；查询为零的折叠不能全部称作 SMT 新证明。
同一条件的多个消费者共用证明，但分别计入 folded。

无 Z3 构建仍保留 C++/C/Python 接口，返回 `available=false`、零统计且该步骤不改 IR；
普通 mem2reg/InstCombine/GVN/DCE 仍执行。这不是把没有执行的 SMT 比较算作成功。

## 回归与独立参考

十项 C++ 回归 / 395 条断言覆盖正反条件、branch/select、1/8/16/32/64/128 位、
变化条件、同址不同 load、预算、显式 unknown、不支持的运算、共享证明、位运算
恒等式和嵌套 select 的分支方向。
`resourceLimit=1` 的负例确定产生两次 solver unknown，IR 保持不变；未放宽判断。
有符号字节范围、零/符号扩展、截断、固定移位、位运算恒等式和 select 另有 250 次
A64 Unicorn 条件值计算，全部匹配，零跳过/排除；这是条件数值参考，不计作完整函数
状态比较。128 位常量还回归了最高位非零的无符号上界。

宽位恒等式 `(x|y)-(x&y) == (x^y)` 的首次测试在 64 位时得到 solver unknown，
按设计保留了原 IR。该压力测试显式使用 5000 ms / 2000000 资源预算后，所有宽度的
原有断言通过；没有删除模板或放宽比较，生产默认预算仍为 25 ms / 200000。

这些 C++ 测试使用新 API，旧头文件不能编译。已保存真正的旧头文件编译失败日志。
另有可在旧版本运行的 Python 回归：只通过已有的 `optimize()` 调用真实 A64 程序，
旧版在第一个“恒定分支必须消失”的断言失败；新版本与对应 C/Python 镜像检查通过。

`scripts/validation/predicate_fold_diff.py` 使用六个真实 A64 函数，覆盖 32/64 位的
恒真、恒假分支和仍随输入改变的平方奇偶谓词。后者必须实际走到两个结果路径。
每个函数的 V2/static product × O0/O2 × 八个输入，共 192 次完整 Guest RET。
独立 Python 奇偶规格先检查 Unicorn 的返回值及中间低位，再将原生 LLVM 输出与
Unicorn 的全部 102 个状态字段、完整 64 KiB 内存、退出记录和调用次数比较。
零排除、零跳过、零归一化。

旧库 192 次语义比较也匹配，但八个静态恒定分支模块未完成折叠；新版 192 次仍全部
匹配，恒定分支消失，变化条件保留。旧库不是语义错误基线；这里测量新增的化简能力。
分支形状的对照点是 `LLVMLifter.optimize()` 结束、外部 `opt-21` O2 之前。
负对照只破坏 LLVM 输出寄存器或内存，要求每次都报告对应差异，基础设施错误不能算通过。
寄存器与内存两组负对照各 16 次均检测到对应差异。新旧原生记录包含实际映射的双库哈希。

主构建完整 CTest **556/556**，无 Z3 **439/439**。后者的直接组件回归为 1 项/5 条
断言，并验证 C/Python 的不可用标志和无修改行为。两套完整测试均包含五项原有 UBSan。

## Tigress 与归因

使用固定公共 Tigress 2.1 revision 2128、seed 4242，三档 f-mba/f-opaque/f-combo、
C O0/O2、两种反汇编策略、product-O0/O2、24 个输入：共 288 次 C 规格检查和
576 次完整 LLVM 返回匹配，零 CFG 错误、零未提升、零 incomplete、零差异。真实 Guest
megaInit 仍执行；没有注入正确堆/表字节。这批记录验证回归，不代表所有形式都能化简。

另对每个 kernel 单独生成静态模块，在 mem2reg/InstCombine/GVN/DCE 后运行本组件。
两种策略得到相同计数，下表是单次组件调用的结果：

| 档位 | C 优化 | 调用前条件分支 | 调用后条件分支 | folded 计数 |
|---|---|---:|---:|---:|
| f-mba | O0 | 161 | 96 | 65 |
| f-mba | O2 | 57 | 55 | 2 |
| f-opaque | O0 | 235 | 187 | 48 |
| f-opaque | O2 | 85 | 81 | 4 |
| f-combo | O0 | 1419 | 329 | 1083 |
| f-combo | O2 | 189 | 176 | 13 |

必须区分来源：同一份输入 IR 以零查询预算重跑后，folded 计数完全相同。
这些 Tigress 样本中的下降来自既有常量的 CFG 清理，默认预算内新增 SMT 证明为 **0**。
单 kernel 计数也不等同于整镜像 product 模块的计数；这些是 LLVM 条件数，包含生成的
Guest 状态和内存边界检查，不能按减少条数推算原程序移除了多少个不透明谓词。

进一步先把字面常量清理至固定点，再将查询/节点预算提高到 1024：六个单 kernel
仍没有新增证明，f-opaque O2 有一次 unknown，其余查询正常结束。组合档清理第二轮
又消掉的 2/1 个条件也是普通常量，不应计为新 SMT 发现。因此本组件的新增代数证明
能力由独立的真实 A64 程序及条件/负对照验证；当前 Tigress 链表型条件仍不在能力范围。

两个真实语料库已完成 **69/69 个输入、138 种策略、206649 个函数检查点**，
原始和优化后各 **6530 个模块**均通过验证；所有已恢复的有效函数均已覆盖，提升失败为零。
全部输入哈希与原基线一致，138 份策略结果中已记录的结构字段均保持原基线值，原有异常仍保留。
函数数量与按 32 个函数分组的模块数量也已交叉核对；没有超时、遗漏或被算作成功的跳过。
运行进程实际映射的双共享库哈希与被测试版本一致。

VM 中途重启后，审计并复用 25 份完整语料记录，补齐剩余 44 份。损坏的无 Z3 测试
日志另行保存并重跑完整 439 项，结果全部通过；损坏日志没有计作通过证据。
全量采用 StaticAnalysis 及现有的 32 函数/模块配置。最初尝试的 V2 全量优化在 sqlite
上达到约 2.2 GiB RSS、2 GiB swap，主动停止该资源密集流程；已完成的记录保留于
`v2-optimized-partial/`，不称为 V2 全语料通过。SMT 预算不限制标准 LLVM 优化的内存
和总耗时；V2 的完整执行覆盖使用前述独立原生回归。

整库 IR 合法性不代表整库语义等价，也不证明所有输入路径均正确。此次没有更改
指令提升器或 IGNIL 优化器，未另跑单指令和 IGNIL 基本块全量差分；新 LLVM 改写的
执行语义覆盖来自上述函数级全状态参考与负对照。原有未提升指令、函数入口误报和
objdump 对齐异常仍按原基线保留，不视为本组件修复的结果。

产物位于 VM `~/vmc-runs/llvm-predicates-20261105/`，保存新旧双共享库（`baseline/` 与 `final/`）、对应 API/Python
快照、原生输入输出与 IR、单元/负对照、语料与 Tigress 原始记录。首次 Tigress 运行因
SSH 环境没有 TIGRESS_HOME 而在生成阶段退出，保留在 `tigress-initial-s*`；没有执行
C/LLVM 比较，没有计作成功。设置为已固定的 Tigress 2.1 目录后再运行。
回放库快照时，同时设置 `DRAGON_C_LIB` 和指向同一完整快照目录的
`LD_LIBRARY_PATH`，避免 shim 从其他构建目录加载 backend。

```sh
./build/bin/dragon_tests '[predicate-fold]'
~/ucdev/bin/python3 scripts/validation/predicate_fold_diff.py --out /tmp/predicate-check
~/ucdev/bin/python3 scripts/validation/predicate_fold_diff.py \
    --out /tmp/predicate-control --case even64 --case square64 --trials 2 \
    --control result --expect-control
```
