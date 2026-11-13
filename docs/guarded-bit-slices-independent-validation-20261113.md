# guarded-bit-slices 独立参考验证（2026-11-13）

正式 final v2 的 main/no-Z3 准备、O0/O2 原生差分和全部保存帧复审现已完成，
结果见末节。下面先保留 before-only 校准及其当时的 pending 状态；这些历史
数字不计入正式结果。没有改动任何旧 checked-corridor source、tuple 或 gate，
也未声称真实 ELF 提升或完整 loop CFF 消除。

独立域在 `scripts/validation/guarded_bit_slices_oracle.py`，逐语句 C 参考在
`guarded_bit_slices_reference.c`。5200 个不重复 tuple，25 个 IR 形态：
32/64/128 位 LShr/AShr 的最高 1、2、3 位共 18 个形态，以及动态合法 count、
含 count=0 的部分候选、历史/当前 source、table LOAD 前后 STORE、外部 side
CALL 和 corridor 内 CALL。输入含 0、1、全 1、各宽度符号位、最大正数，
0/1/3/7 次迭代、Guest/regs 别名、Guest size 的表项边界、正常/错误实际目标。
每次循环重新 LOAD source，并翻转符号位及动态 count；callee 路径还会修改
source、Guest ledger、表和寄存器。真实输入来自独立元组，不来自优化后 IR。

所有比较都是返回值、callcount 加整个 73728 字节 arena；arena 包含完整
regs、Guest、描述符、config、7 个 Exit 字段和未触及字节。不归一化地址或
输出，不只比较 return。保存流使用该次运行的实际 Guest 地址重新建模。
C 参考用无符号整数显式 sign fill，避免依赖 C 的负数右移规则。

## 校准结果

| 原 IR 优化级 | LLVM fullstate | 独立 C fullstate | business / checked failure | fault 观察 / 指定差异检出 | comparator controls | 保存流 / frame |
|---|---:|---:|---:|---:|---:|---:|
| O0 | 5200 | 5200 | 3384 / 1816 | 2912 / 1561 | 57200 | 64 / 13312 |
| O2 | 5200 | 5200 | 3384 / 1816 | 2912 / 1561 | 57200 | 64 / 13312 |

`guarded_bit_slices_saved_audit.py` 已重新读取全部保存 frame；每个 frame
与冻结 Python 模型完整逐字节相等。14 个故障变体均有至少一个指定差异，
并先与其故障模型匹配，再与正常模型比较。各优化级的指定检出数为：

| 故障 | 指定检出 |
|---|---:|
| omit/double prefix | 208 / 208 |
| frozen source | 208 |
| logical 替代 arithmetic | 112 |
| signed32 错误 zero extension | 103 |
| 忽略 128 位高半 | 182 |
| 错误 count / frozen count | 196 / 50 |
| stale history | 66 |
| table STORE 后重读 selector / 丢 table STORE | 50 / 40 |
| 丢 case45 / 丢 side CALL | 30 / 68 |
| 错误 root history PHI arm | 40 |

shift=count=width、超过 width、直接 undef/poison 是另外四个**仅 IR 拒绝**
输入，不运行 native。所有 `opt`、编译和 public driver 子进程仍使用原
180 秒上限。跳过、排除、归一化均为 0。

新 public driver 在官方旧 checked runtime
`48b9b76481c93fd6f18f0a7aca6a913e44e72d8becdc1e6a948a25369d4acc85`
下编译运行，显式 `LD_LIBRARY_PATH`。旧库拒绝 `guarded-bit-slices`，exit=3，
Module/Function/Argument/global/metadata 身份及 IR 保持。这是缺少 builtin 的
API 拒绝，不是旧库错误 native 执行。

## 保留的失败

v1 helper 把 `0xcafe+i` 写成一个 C preprocessing number，编译失败，未执行
native；v2 只插入分隔空格。v2 正常态 C/LLVM 全部通过，但第 14 个 fault
model 错把 callee45 返回 arm 也当作损坏的 dispatch43 arm，遇到保留输入
失败。v3 把 fault 条件精确限定为 selected==43，并预计算相同 arena 初值以
加速。v3 所有 tuple、原/故障/拒绝 IR、helper、C 参考与 v2 逐字节相同；
完整重跑 O0/O2，没有删除失败、改输入或放宽比较。最终数字只计 v3，v2
部分执行不重计。

## 冻结与边界

VM 根：`~/vmc-runs/guarded-bit-slices-independent-20261113/`。冻结正常输入和
模型为 `dragon-bit-slices-freeze-v3/`；public driver/prepare 的全部 source 和
输入另在 `dragon-bit-slices-public-v1/PREDECLARED.json` 冻结，尚未用候选
runtime 执行。

before 归档 `guarded-bit-slices-before-v3-20261113.tar.gz`：
SHA256 `7b34709f89aba940ca431af6c55886b4f0dd52f148dc0bce71a10b7b7a1c84ea`，
658 个 payload 文件，4020190 字节。VM 和 macOS 均验证完整文件集合、大小和
SHA256；Mac 权威副本在
`/Users/c/Desktop/deob/validation-artifacts/guarded-bit-slices-20261113/`。
v1/v2 原始失败均在归档内，未退役文件。

这是 LLVM 函数级完整状态差分，不覆盖机器指令 decoding/IGNIL/Unicorn、
全 corpus 或真实 ELF。候选精度、全部候选原生执行、真实输入、完整 CTest
仍由后续明确冻结的工作提供，不能用 before 校准数字替代。

## 追加全部高位图案的 v4 域

独立覆盖盘点发现：5200 个边界 tuple 对 top3 的实际图案只覆盖了
0/3/4/7，不能据此声称全部八种图案。v4 **保留原 5200 个 tuple 作为完全
相同的前缀**，另追加 504 个明确的图案输入：每个 32/64/128 × LShr/AShr ×
top1/2/3 形态都枚举全部 `2^bits` 图案，组合 alias=0/1 和实际目标43/44/45。
总数为 5704 个不重复 tuple。原/故障/拒绝 IR、helper、C 参考与 v3 全部
逐字节相同，没有删除输入或放宽 gate。

| v4 原 IR 优化级 | LLVM fullstate | 独立 C fullstate | business / checked failure | fault 观察 / 指定差异检出 | comparator controls | 保存流 / frame |
|---|---:|---:|---:|---:|---:|---:|
| O0 | 5704 | 5704 | 3888 / 1816 | 3104 / 1713 | 62744 | 64 / 14512 |
| O2 | 5704 | 5704 | 3888 / 1816 | 3104 / 1713 | 62744 | 64 / 14512 |

两级 saved audit 均重新读取全部 frame 并通过。v3 数字作为原先独立校准的
历史结果保留，不能与 v4 相加计作新增覆盖。v4 的 14 个故障指定检出依次为
220、220、220、144、119、226、216、50、66、50、40、34、68、40，与上表
故障顺序对应。skip/exclusion/normalization 仍为 0。

最新候选公共驱动根为
`~/vmc-runs/guarded-bit-slices-independent-20261113/dragon-bit-slices-public-v2/`，
其 source/inputs/gates 在候选执行前冻结。driver C++ source 与 v1 相同，复用
已绑定 SHA256 的 driver；显式 `LD_LIBRARY_PATH` 将选择正式候选 runtime。
旧官方48b9 runtime下，v2 的 checked/state/store 三个现有 profile 均通过，
输出与各自 parsed-before 逐字节相同。这些 public 调用的 native 执行数为 0。
候选 pass/原生验证仍待正式 runtime，不得把上述数字当作候选结果。

v4 before 总归档 `guarded-bit-slices-before-v4-20261113.tar.gz`：
SHA256 `c9669f5a56a461c02efbf8ab2787400bc442087225e605e1185e1457bbe330c2`，
1084 个 payload 文件，6933097 字节；含全部 v1/v2 失败及 v3/v4 原始记录。
VM 和 Mac 副本均完整核对文件集合、大小和 SHA256。没有退役任何证据。

## 正式 final v2 原生结果

冻结的 public-v2 source、5704 个 tuple 和全部 38 个 prepare gate 未改。
正式 main 和 no-Z3 各自重新生成 IR，再分别编译执行 O0/O2；没有复用 proto
IR 或其通过结果。每个 backend 的 38 个 recipe 全过，24 个正常形态各有一个
实际 loop context；`call_inside` 保持原 IR，四个 undef/poison/非法 count
输入仍仅检查拒绝，不运行 native。

| 正式 backend / 优化级 | LLVM normal fullstate | 独立 C fullstate | business / checked failure | fault 观察 / 指定差异检出 | comparator controls | 保存流 / frame |
|---|---:|---:|---:|---:|---:|---:|
| main O0 | 11628 | 5704 | 7928 / 3700 | 3104 / 1713 | 127908 | 90 / 20436 |
| main O2 | 11628 | 5704 | 7928 / 3700 | 3104 / 1713 | 127908 | 90 / 20436 |
| no-Z3 O0 | 11628 | 5704 | 7928 / 3700 | 3104 / 1713 | 127908 | 90 / 20436 |
| no-Z3 O2 | 11628 | 5704 | 7928 / 3700 | 3104 / 1713 | 127908 | 90 / 20436 |

每行 normal 数为 5704 个 before、5704 个 after 和 220 个错误 hint 的实际
原生执行；独立 C 另列，没有混作候选执行。每个 backend 合计 23256 normal
完整状态匹配、11408 C 匹配、6208 fault 观察/3426 指定检出、255816 comparator
controls，以及 180 个流/40872 个 frame 全部复审。两 backend 总共实际复审
360 个流/81744 个 frame；distinct tuple 仍为 5704，历史校准/proto 不重计。
14 个故障在每行均有指定差异并全部检出。skip/exclusion/normalization 均为 0。

main/no-Z3 的 116 份 prepare IR 产品（含 parsed-before、各 round 和最终 IR）
全部逐字节相同，未归一化。checked/state/store 三个旧 profile 在两个 backend
均与已冻结的旧48b9输出逐字节相同，统一 SHA256 为
`47f6e5e49748b3ba84f53188c07ea10161ece159652d3ca23dfc936e1bec36f3`。

正式执行使用的 runtime 为：

| backend | libdragon.so SHA256 | MANIFEST.json SHA256 |
|---|---|---|
| main | `02d730ce862ec7bf60ccaccb05bd1b61cd3b971b8b444b56d3a0e108b60992e3` | `17c3831dbe81c46194f3bc459f01bb023069dfe32b87fb13b944cb5758c6fe9f` |
| no-Z3 | `be804e7bef457bcd3fce984c2e16ed504b96152da8961a3da026e2f9c0e604e4` | `f162ebfdf9fb5bf9645a9eb3d4246a0fb209106b5b3257ebf103a1c3ca0da76e` |

目录为
`/home/cc/vmc-runs/guarded-bit-loop-discovery-20261113/runtime-{main,noz3}-v2/`。
每个子命令都显式设置该 backend 的 `LD_LIBRARY_PATH`，记录实际 ldd 和前后
source/driver/backend/clib/MANIFEST pins。十个正式 prepare/native/audit 阶段
的 pins 均稳定。编排仍要求开始至少 512 MiB、运行至少 64 MiB，单阶段及
原内部子命令上限均为180秒；最小开始空闲729239552字节，运行观测最小
729210880字节，最长阶段20.345秒。没有并行运行两 backend。

`guarded_bit_slices_campaign.py` 与归档中实际执行的
`candidate-driver-final-v2b.py` 逐字节相同，保留正式路径、backend/MANIFEST
hash、资源门槛和不覆盖已有 label 的行为。正式 label 为
`main-final-v2`、`noz3-final-v2`，所有正式 receipts 在
`candidate-controls-final-v2/`。

### 保留的 v1 精度失败与 proto

正式 v1 main 的第一次 prepare 在 `l32t1` 失败：原 index successor slot0
仅进入 `dispatch.transition`，新增实际 LOAD 的短 guard 和 full fallback，
但 `loop_contexts=0`，未满足冻结的正向 loop gate。原 fixture 没有 Generated
seed，`max_rewrites=32`，也没有耗尽预算；位移结果无需依赖 root PHI 就能在
最短 suffix 预测，首次成功后搜索停止。这不是错误原生执行的证据；v1 没有
运行候选 native。原失败、raw IR、counters 和只读诊断完整保留。

生产修复让 Bit 专用短 suffix 成功后继续原有有界搜索，优先保留实际 case
回 root 的 loop 候选，保持同级顺序、共享 cap 和工作记账。未改变 source
运算、flags、实际 LOAD、完整 fallback 或 core。旧 c23/58 两构建的新回归
各以 0 loop 对预期1失败；正式源码 SHA256 为
`a82d9a337f45dc7a743b19b152f75a78cb66307d66d88977cce815d9b11d0177`。
协调者已完成 main 1163/1163、no-Z3 1028/1028 CTest，各 focused 98 cases /
8105 assertions；对应339 source pins、patch和完整日志副本在 native包内。

9dd884 proto 的38项 API/IR preflight 全过，但执行数为0，只作早期诊断；
其结果未计入上述正式结果。正式启动编排一度漏替换历史 runtime 路径，pin
预检在任何 candidate 调用前 FileNotFound；旧脚本及错误记录保留，修正仅在
新 `final-v2b` 编排文件中，冻结 source/tuple/gate 没有修改。

### 正式封存与范围

最终 native 归档：

- VM：`/home/cc/vmc-runs/guarded-bit-slices-native-final-v2-20261113.tar.gz`
- Mac：`/Users/c/Desktop/deob/validation-artifacts/guarded-bit-slices-20261113/guarded-bit-slices-native-final-v2-20261113.tar.gz`
- SHA256：`40bff338e3a83f25838cd8890b90ebb13fd1e60ccbd20199033ef0514cf91ff4`
- 2920 个 payload 文件，15329611字节；两端均完整核对文件集合、大小和 SHA256。

归档包含全部历史失败、原始模型/输入、C/LLVM 产品、保存流、正式 source/
runtime/LD pins、资源 receipts，以及生产构建出处。为避免重复约400MB backend，
实体 runtime 由协调者正式 production 包保存；native包收入其精确 MANIFEST
副本和全部 runtime 文件大小/hash引用。协调者后续可另行绑定 production
archive SHA，不修改已冻结的 native payload。本 agent 未退役任何证据。

这是给定独立域上的 LLVM 函数级完整状态差分，不是完整语义证明。本轮没有
新机器指令/IGNIL/Unicorn 或全 corpus 运行，没有真实 ELF 精度/native结果，
也未观测 fast-loop 迭代次数或声称完整 CFF 消除；真实输入由后续独立 campaign
提供，不能由这里的正向 loop fixture 推断。
