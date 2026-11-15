# 同一大函数叠加 MBA、不透明谓词和平坦化的实测

本轮仅新增验证及交付记录，产品代码保持 `9404326`，没有修复、调预算或扩展 VM。
结论：**受测输入上的语义完整匹配，三重组合尚未完成循环去平坦化。**

## 样本

使用此前大 Flatten/switch 样本的同一份 `practical_holdout_source.source(2026111307)`：
嵌套循环、输入相关次数、动态数组索引、旋转、真实业务分支、字节覆盖及四字输出。
原始 C 字节与之前冻结的源文件相同。使用已安装 Tigress **2.1 revision 2128**，
同一种子 `2026111307`，顺序为：

1. InitEntropy / InitOpaque，array size16；
2. AddOpaque，true，count6；
3. EncodeArithmetic；
4. Flatten/switch，RandomizeBlocks=true、SplitBasicBlocks=true。

生成器输出完整保留，没有手工重写混淆表达式、移除分支或替换数据。没有 VM 或堆链表。
Clang 21 按原 freestanding 选项构建 x86-64 ELF，C O0/O2 均保留；编译命令在证据中。
O2 中实际中央分发为 `0x11d6: jmpq *0x2000(,%rdx,8)`，此前实际比较上界为0x5b；
表有 **92 个槽 / 86 个不同目标**。`kernel` 位于0x1010。

| 文件 | SHA256 |
| --- | --- |
| combined-O0.elf | `454ce92928f670bcb6887e6e36347d6f6c53e169ecbad990fe4607fb3b0a94e3` |
| combined-O2.elf | `72f3e6321c5ba5c40dbe18601ca2959fb92828c6a31231948cb0987176aae860` |

本地交付目录 `/Users/c/Desktop/deob/ida-review-20261115-large-combo/`，包括两个 ELF、
原始 / 生成 C、生成请求和日志、提升前及 product IR、pass 报告与循环拒绝报告。

## 先确认混淆程序本身有效

初版 array size4 输出的最后一个三元组除数为零，全部 16 输入在 C O0/O2 的机器码
参考执行中异常；O0 在0x3c77 `idivl 0x630c`，O2 在0x122a
`idivl _2_kernel__opaque_array+0x2c`。32 次 reference_error 均保留，没有算成
框架语义错误或成功跳过。同轮原版32次参考匹配单列，不并入下面最终配方的数目。

另试 AddOpaqueAtomic=true，输出与该初版相同，不重复执行。随后独立 array size16
配方生成有效程序，仍保留6个不透明谓词及相同 MBA / Flatten 配置。
该配方原版32次、混淆版32次 Unicorn 执行全部与独立 Python 业务规格的返回值及16字节
输出匹配。不同二进制只比较这些业务输出；没有比较不同编译产物的栈布局或寄存器分配。
新脚本首版在 Python3.10 使用旧 helper 的 `hashlib.file_digest` 时启动失败，尚无目标
执行；v2 使用同一 SHA256 算法读取实际映射库，原失败脚本和说明保留。

## LLVM 语义对照

复用公共 StaticAnalysis / FunctionStart API，按独立 ELF STT_FUNC 正长度范围和实际
解码 leader 建立 private decoded region。Graph 全文快照保持不变，不添加虚构 CFG 边。
分别执行 raw 和十步 product 配方，然后各以 LLVM O0/O2 生成本机代码；未知调用只在
最后验证链接阶段连接实际提升函数，优化时没有宿主效果桩。

| 后端及 C 档位 | 计划 / 实际完整状态匹配 | 故障比较器检出 |
| --- | ---: | ---: |
| main，C O0 | 64 / 64 | 192 / 192 |
| main，C O2 | 64 / 64 | 192 / 192 |
| no-Z3，C O2 | 64 / 64 | 192 / 192 |
| 合计 | **192 / 192** | **576 / 576** |

每个输出均对照该份 ELF 自己的原始 Unicorn 状态：全部512字节寄存器快照、65536字节
Guest 内存、七个 Exit 字段和剩余预算，共66112字节逐字节相等；实际输出协议另外有
24字节头。76个已建模字段、正常根 RET、Exit 字段和零预算也保留原检查。
每次匹配后分别破坏 RAX、Guest 输出字节、Exit，比较器必须检出。
全部12个模块无 unlifted、无 unexpected unreachable，IR verify 通过，源 Graph 不变。
无差异、未完成、目标错误、跳过、字段排除或归一化；实际源 / 输入 / 动态库 pins 前后
全等。保存结果再独立审核192帧、576个故障对照，无新增目标执行。

本轮没有重跑完整 CTest 或单指令差分；产品代码未变，完整1332/1186结果仍属于上一轮，
不作为本轮新增执行数。这里也未验收未知外部 API、多个随机种子或任意输入的等价性。

## 去混淆收益与未通过项

十步显式配方为 mem2reg → instcombine → gvn → guest-memory → mba → predicates →
dispatch-specialize → dispatch-deflatten → control-flow → dce，固定一轮 / 最多16步。
预算与此前 heldout 十步配方相同，没有因失败扩大预算或循环重试。
下表化简统计取各 C 档位的 product-O0 调用；product-O2 的独立调用报告另存。

| main 指标 | C O0 | C O2 |
| --- | ---: | ---: |
| MBA expressions_simplified | 27 | 1 |
| predicates conditions_folded | 901 | 14 |
| kernel 基本块 raw → product | 2871 → 1068 | 432 → 408 |
| kernel switch raw → product | 2 → 1 | 2 → 1 |
| dispatch-deflatten 改写边 | 0 | 0 |

条件数量包含提升器的地址 / 除法等检查及其它条件，**不能把901或14解释为消除了多少
个源级不透明谓词**。原始两个 switch 中也包含入口分派；剩余中央分发 switch 仍在。
Guest-memory 报预算不完整；MBA 也有 budget_skipped，谓词有未查询条件。Pipeline 的
ok 只表示运行无错误，不表示所有分析完整。no-Z3 配方缺少求解器能力，不能借其语义
匹配宣称 MBA / 谓词化简能力与 main 相同。

另从原 ELF 独立运行统一 CLI，使用静态表目标和89个 leader 候选，以及 SP、RSI、
表、opaque array、entropy 的显式入口字节检查。没有将 image 数据当不可变内存。
main / no-Z3 都在原 loop 预算内拒绝：

```text
loop batch refused: selector lacks a complete safe finite state proof
available=1 complete=0 closed=0 budget_exhausted=0 work=448500
```

拒绝时 guarded IR 与 loop 输出逐字节不变，没有执行后续清理或宣称闭合。
本轮只定位到选择器有限状态证明的拒绝，尚未锁定其内部第一个未证明子表达式。
下一步应沿该证明诊断明确区分 opaque/global 读取、条件相关性与循环内状态汇合；
普通十步配方的预算耗尽应另行分析，不能直接解释为同一次 loop 拒绝的原因。

## 封存与复验

证据包 `large-combo-complete-evidence.tar.gz` 位于：

- Mac：`/Users/c/Desktop/deob/validation-artifacts/large-combo-evidence-20261115-v1/`
- ROCK：`/home/ubuntu/vmc-runs/large-combo-evidence-20261115-v1/`

压缩大小55,788,115字节，SHA256
`734234dce02c65b5dab8fc8973f4171f3ede9a4657c4328e7da0505c2ca642b3`。
含1008个 payload文件 / 146,811,726逻辑字节，加一个 manifest，共1009个regular成员。
完整保留两个实际参考配方、首版异常、生成相同输出的 atomic 配方、12个原生模块的
全部输入 / 参考 / 输出、完整IR、pass报告、两套 CLI 拒绝结果、实际运行脚本及依赖源。
动态库及系统依赖不重复嵌入，实际加载路径与 SHA 在计划中绑定；不称为完整运行环境备份。

两端独立核对完整成员集 / 大小 / SHA、单gzip CRC / EOF、tar零填充及无链接；
归档输入的大小 / mtime / ctime / SHA 前后相等。双端同目录下的
`DUAL-HOST-CERTIFICATE.json` 为1374字节，SHA256
`afd1ce3b72febadfc09d79075b32bc7315326370ef87d89edae8e7fc1e1e28f7`；
manifest SHA256
`b3cab351cdc1d2073998cef2006f0e7dc912cc6f6e1b13a0b38b041f4e3d36c0`。
封存与保存结果审计均无新增目标执行、跳过、归一化或删除。

实际测试脚本为
[`large_combo_reference.py`](../scripts/validation/large_combo_reference.py)、
[`large_combo_replay.py`](../scripts/validation/large_combo_replay.py) 与
[`large_combo_saved_audit.py`](../scripts/validation/large_combo_saved_audit.py)。
reference/replay 拒绝已有输出目录；replay 是诊断工具，必须读取 RESULT / pass / loop
状态，不能只根据进程退出码判断已完整去混淆。保存审计器按本轮精确路径和全部192帧
独立复验，不能用另一个 ELF 的快照替换参考。
