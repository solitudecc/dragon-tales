# guarded-bit-slices 独立参考验证（2026-11-13）

当前里程碑是 **before-only 参考校准完成**。本报告尚未声称候选 pass 的
native 正确性、真实 ELF 提升或完整 loop CFF 消除；候选 runtime 正式冻结后
再追加结果。没有改动任何旧 checked-corridor source、tuple 或 gate。

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
SHA256；Mac 副本在 `validation-artifacts/guarded-bit-slices-20261113/`。
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
