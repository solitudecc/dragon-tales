# 有限值集与显式分发器恢复 Pass

2026-11-06，基线 `1907235`。本组件补充可选的前向有限值集分析和 x86 Graph Pass，
在所有已知入边的状态值可被有限集合覆盖时恢复间接跳转目标。原有常量切片接口和默认
反汇编路径保持兼容。分析不执行目标程序；Unicorn 和原生执行只用于验证。

这一步恢复 CFG，供后续提升和优化使用。它不自动移除分发器、不生成重编译后的可执行文件，
也不提供内部调用摘要、镜像数据不变性证明或初始化堆模型。

后续新增可选的[叶函数效果证明](leaf-call-effects.md)，可跨过已证明无 STORE/无 SP 写的
直接调用；本文的全 CALL 屏障及数字描述原始阶段和当前默认配置。

后续 [CET 函数边界修正](cet-function-boundaries.md) 处理了本报告中 GCC 的额外入口。
它改善 goto 的静态执行，同时保留 indirect 的覆盖下降；下文数字是本阶段冻结记录。

## 值域与入口契约

`dragon/analysis/FiniteValues.hpp` 提供 `finiteValues(graph, entries, queries, limits)`。
每个值有位宽及三种状态：`Unreachable`、`Finite`、`Unknown`。有限集合是完整的保守覆盖，
可能包含不可行值；超过集合容量时整个值变成 Unknown，不截断候选。全局操作、块或事实预算
耗尽时，结果 `complete=false`，所有查询和跳转目标都变成 Unknown。
`complete=true` 仅表示已对当前 CFG 求得不动点，不表示 CFG 已完整或混淆已消除。

- 汇合点取集合并集；任一路径没有可用值时保留 Unknown。循环通过有容量限制的单调传播收敛。
  条件分支的两条边都传播，不用未证明的路径条件排除值。
- 寄存器按字节别名处理，32 位写保留既有父寄存器清零规则。整数 COPY、算术、逻辑、比较、
  截断/扩展和移位遵守 IGNIL 位宽语义；当前只保存不超过 64 位的有限值。
- 支持共享全零/全一掩码的整数 select 表达式，避免把相关掩码独立组合而制造额外跳转地址。
  不依赖某个混淆器的标签、函数名或固定地址。
- 每个独立入口有任意寄存器/内存输入，以及独立的符号 SP 基址。SP 的有限仿射偏移可以
  识别显式写入的栈表，SP 本身不会作为已知数值输出。
- 只有前面的 Guest STORE 提供 LOAD 的值。镜像文件中的表字节、动态观察快照和常见 API 名称
  都不自动成为静态内存事实。不同符号基址、符号地址与具体地址之间可能别名；未知或多地址
  STORE 保守清空内存事实，部分覆盖保留已证明不相交的字节。
- CALL 清空寄存器和内存事实，已计算的 Temp 快照仍有效。unlifted 或块内局部控制流按未知
  处理，不猜调用约定、callee 保存寄存器或初始化函数效果。
- 显式 `entries` 必须包含所分析区域的所有独立入口。空列表使用 Graph 的有效函数入口，
  若没有则使用无前驱块，再没有则对所有块开放输入；不会把无根循环误认为全不可达。
  直接被调函数始终具有独立输入。错误或不完整的入口契约会影响任何基于入口的证明。

默认每次分析的限制：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `max_values` | 64 | 每个值的最大有限集合，硬上限 256 |
| `max_blocks` | 4096 | 参与传播的块数 |
| `max_operations` | 1000000 | 转移、枚举等操作预算 |
| `max_facts` | 262144 | 寄存器/内存状态及块内 Temp 的预算 |
| `max_rounds` | 256 | 仅用于分发器恢复；硬上限 4096 |

增加集合容量不一定改善结果：循环计数器会产生更大的中间集合，可能先耗尽操作预算。

## Graph Pass 与最终复验

`finite-values` 是内置只读步骤，将 `FiniteValueResult` 存入本轮 C++ 分析缓存的同名键，
报告块访问、操作和有限/未知/不可达跳转数量。Python 可通过 `Graph.finite_values()`
直接查询结果；语言之间不自动互换分析缓存对象。

`dragon/passes/FiniteDispatch.hpp` 的 `FiniteDispatchPass` 必须显式注册和运行。
它复制调用者提供的代码字节、基址与入口列表，目前只支持 x86-32/x86-64 的 CFG 恢复；
AArch64 可使用只读有限值集分析。代码范围必须非空且不回绕，字节必须与 Graph 对应区域一致；该参数不是代码修补接口。
镜像有多个可执行区域时，
应逐区注册；本版的新目标须在对应代码范围内，跨区域恢复留给其他分析。

恢复在 Graph 深复制上进行。每轮重新分析 CFG，允许目标集随新路径单调增长，解码新块、
拆分共享后缀并重新提升相关块。新路径暴露未知值时撤销依赖该候选的扩展，已有间接边也须
重新复验；若旧边不能重新证明或预算耗尽，则丢弃整个本次扩展。
扩展中发现的直接 CALL 目标始终是独立入口，同步加入旧切片器与新值集分析的边界；
不会用尾跳路径上的 STORE 约束同一函数的 CALL 输入。
最终固定点检查通过后才发布 Graph。原有守卫跳转表识别仍可提供目标，有限内存分析不会
把无界镜像 LOAD 当成已初始化的常量表。线性扫描仍要求精确指令边界。

分析预算每轮重新取得，`analysis_operations` 累加各轮用量；轮数预算限制总轮次。
报告计数器包括 `rounds`、`analysis_operations`、`new_instructions`、`new_edges`、
`unresolved_jumps`。回滚时新指令/边为零，未解析跳转计数来自原图。
未知跳转可以使步骤返回 Incomplete；这不是执行错误。Success 也不证明函数划分正确、
所有指令都有语义或去平坦化完成。流水线的 `ok`、`converged` 含义仍见
[调度契约](pass-pipeline.md)。该原生 Pass 自行事务化；任意第三方 Pass 不自动回滚。

## API 示例

```python
# code 是单个连续可执行区域，entries 包含所有独立入口。
with dragon.PassPipeline() as pipeline:
    pipeline.register_finite_dispatch(code, base, entry_points=entries)
    pipeline.add('finite-dispatch', max_values=64, max_rounds=256)
    report = pipeline.run(graph)
# 重新取得 Graph 视图；运行前借用的视图已经过期。
facts = graph.finite_values(entry_points=entries)
for block, value in facts['jump_targets'].items():
    if value['kind'] == dragon.ValueSetKind.FINITE:
        print(hex(block), [hex(x) for x in value['values']])
# 可选：query_register=(block_start, register_byte_offset, width_bits)
```

```cpp
#include <dragon/passes/FiniteDispatch.hpp>
using namespace dragon::passes;
PassPipeline pipeline;
pipeline.registerGraph("finite-dispatch", FiniteDispatchPass{code, base, entries},
                       false, {}, FiniteDispatchPass::validator());
pipeline.add("finite-dispatch");
auto report = pipeline.run(graph);
```

C 接口 `dragon_pass_pipeline_register_finite_dispatch` 同样复制 code/entries，注册后仍须 add。
`dragon_graph_finite_values` 返回自有 JSON 字符串，用 `dragon_cstr_free` 释放；
`query_width=0` 不查询寄存器。JSON 中地址和值使用十六进制字符串，避免 uint64 精度丢失；
Python 包装转换为整数。查询发生在块末。数值 kind 对应 C 枚举和 Python `ValueSetKind`。

## 验证与实际边界

- C++ 15 项回归、25282 条断言；6250 个混合位宽整数计算与独立 SymbolicExecutor
  参考一致。CTest 597/597，无 Z3 472/472，Python API 两个构建均通过。
- 新 API 测试在旧 `1907235` 上因缺少 `FiniteValues.hpp` 无法编译，原输出保留。
  六组永久机器码夹具的真实旧库对照补充行为证据：x86-32/x64 的候选增长、CMOV 和显式
  栈表，两个策略 × V2/static/product × O0/O2，共 576 次新版完整根返回全部匹配；
  旧版对应 576 次均提前退出，不能计为完整语义错误或成功。无 Z3 另外 96 次完整匹配。
  比较全部 52 个 i386 / 76 个 x64 建模字段及全部 64 KiB 内存，零排除、跳过和归一化。
- 返回值和内存对照各发现 24/24 个单字段差异；缺失目标对照发现 16 条实际遗漏边，
  同组 24 次执行的完整状态仍匹配。每个正例的所有预期目标都被 Unicorn 实际执行覆盖。
- 新被调函数入口回归在中间版本失败 4 条断言；64 次 Unicorn 执行、160 条实际转移
  发现 18 条错误已解析边，最终有/无 Z3 均为 0。另一个删根目标的对照发现 16 条错误。
  这是可选组件的开发期反例，原 `main` 的常量策略没有该新组件，不把它记作旧版错误。
- 开发期还抓到混合位宽移位的 40 个错误计算、JSON 顶层类型、32 位符号指针差扩大位宽时
  的错误常量（1 条断言）、预算退出把未解析数记成零（2 条断言）；失败输出均保留。
- 原有守卫与切片复验：27 个 CFG / 20736 次 Unicorn 守卫执行；3000 个寄存器切片匹配；
  内存 448 次执行 / 192 个独立切片；循环 128 次执行 / 4 个独立切片，均无错误。

普通 Tigress 2.1 seed 1901 的默认预算对照，两个源变体分别得到以下结果；每行每版本
包含 64 次 C 规范匹配、384 次 LLVM 尝试（两种 dispatch × C O0/O2 × 六计划 × 16 输入）：

| 编译器 / 策略 | 旧版完整匹配 / 不完整 | 新版完整匹配 / 不完整 |
|---|---:|---:|
| GCC / 线性 | 192 / 192 | 192 / 192 |
| GCC / 递归 | 96 / 288 | 128 / 256 |
| Clang / 线性 | 96 / 288 | 96 / 288 |
| Clang / 递归 | 96 / 288 | 96 / 288 |

源变体是保留 helper 调用和显式等价内联，分别冻结；16 个不同的混淆 ELF，前后逐项
校验输入与库哈希。合计每版本 512 次 C 匹配；新版 LLVM 1024 次完整匹配 / 2048 次
不完整，旧版 960 / 2112；完整语义差异、基础设施错误、未执行比较及 unlifted 都为零。
新增收益仅是两个变体各 32 次 GCC goto O0 递归 V2 返回：新增一条边及 84 条指令。
额外函数入口总数在该 GCC 递归组由 1 增至 15，静态计划仍受函数边界启发式阻碍；
GCC 线性组原有的 42 个额外入口也保留。Clang O0 的表经真实 Guest memcpy 初始化，
CALL/镜像内存屏障仍阻止本组件解析它；没有用 API 名称猜测其效果。

早期 `max_values=128/max_operations=2000000` 的参数试验和入口补强前的矩阵在
`pre-entry/` 等目录归档，不计最终默认配置覆盖。`pre-entry-relocations.json` 记录快照
移动路径，历史库记录以 SHA256 对照。旧数组型 opaque 重生成尝试另有 56 次 C 匹配、
40 次参考异常；只执行 336/576 次 LLVM 比较，240 次未执行。这批失败输入和输出完整保留，
未排除后改报通过；组合覆盖使用另列的真实 megaInit 冻结样本。

历史冻结的 Tigress 2.1 seed 4242 样本另做严格复验：f-mba、f-opaque、f-combo、
f-combo-goto、f-combo-indirect，C O0/O2、两种扫描策略、V2-O0 / product-O2、16 输入。
10 份 ELF 的哈希前后相同，执行真实 Guest megaInit 和分配器，比较所有建模字段及完整
1 MiB 内存。每版本 320 次 C 匹配；LLVM 384 次完整匹配 / 256 次不完整，完整差异、
错误已解析 CFG 站点及 unlifted 均为 0。384 次完整匹配来自前三档，goto/indirect
组合档仍不完整；新 Pass 的 120 次调用均未改变这些图，不能称为新增去混淆收益。
参考执行到未解码 PC 的数量按 case/策略累计仍为 17123，前后相同，全部保留在原记录中；
这不是新增唯一指令数，也不把未知站点算作 CFG 成功。该矩阵整体仍不能称作通过。

两个真实语料库使用最终库与冻结 `1907235` 双库逐项对照，全部重新检查最终新版：

| 语料 | 输入 | 策略记录 | 每版本有效函数检查点 | 每版本原始 V2 模块 |
|---|---:|---:|---:|---:|
| corpus | 34 | 68 | 91517 | 755 |
| corpus2 | 35 | 70 | 115132 | 931 |
| 合计 | 69 | 138 | 206649 | 1686 |

所有有效函数都提升并通过 `opt -passes=verify`，提升失败、覆盖缺口及跳过为零。
逐指令字节/分类/边、块与函数归属、IGNIL 操作与 unlifted 记录的流式指纹，以及每份
原始 V2 模块的完整文本 SHA256，前后全部相同；结构统计也逐项一致，已有告警保留。
输入、实际映射的两个库及核心源码均校验哈希。旧版基线的早期已完成记录可复用，
新版在入口补强后全部重新执行；中断的输入没有计入完成覆盖。
`final-summary.json` 对所有最终记录、库、输入和正负对照做完整前向审计。

验证脚本 `scripts/validation/finite_dispatch_diff.py` 重用严格完整 Guest 状态参考，
`scripts/validation/finite_tigress_diff.py` 重用独立 C 规范、Unicorn 与六种 LLVM 计划。
`finite_vm_replay.py` 对历史冻结组合样本执行真实 Guest megaInit，并复用动态 CFG 及完整
1 MiB 内存参考；冻结 ELF 不接受 C 源码改坏对照。
旧库同时隔离 C shim、分析库和 Python 接口，冻结输入逐项校验 SHA256。
Tigress 的额外函数入口、提前退出和未执行比较全部保留；不能把整个矩阵称作通过。

本轮没有改变指令 lifter、Guest 执行器或 LLVM 优化器，未重跑完整单指令/真实块语义活动，
也没有重跑两个真实语料库的全部静态优化模块。新 CFG 通过机器码与真实 Tigress 函数执行
对拍验证；全语料检查覆盖默认分析兼容性和原始 V2 IR 合法性，不能代替全程序语义证明。

完整产物：VM `~/vmc-runs/finite-dispatch-20261106/`。开发和构建只使用
`~/dragon-tales-overlap`，原始脏工作区保持原样。
