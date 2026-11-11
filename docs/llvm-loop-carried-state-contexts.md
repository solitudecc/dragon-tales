# 循环携带值与多入边状态上下文

基线 `01c5abd`。主样本仍是冻结的 Tigress x86-64 `f-cff O2`，VM 路径为
`/home/cc/vmc-runs/crosscall-cff-combo-latest-20261111/f-cff/O2/fixture.elf`，
目标函数 `kernel` 入口 `0x10080`。回归同时保留 f-cff/f-combo 的 C O0/O2 四个 ELF。
这些路径、名称和地址只用于验收，不进入生产算法。

## 重新确认的旧能力

旧版本并非无法继续处理其它状态。对上一轮真实 IR 重复运行，已经能逐轮增加状态上下文；
首个已知状态 50 复制后，SSAUpdater 会把不少循环携带值修成其它块中的可用 SSA。
本轮真实收益应按一次调用处理多条原始入边、减少完整流水线重复来衡量，不能把旧版本
多轮已能恢复的状态当成新增可解析范围。

另一项独立缺口是：没有先行已知 seed 的合法循环，状态来自 body 的常量，但其它 PHI
直接携带原检查块内上一次执行的 LOAD、PHI 或 freeze。旧代码整体拒绝这类入边。
要扩大该域，必须保留旧执行值在前驱边上的身份，不能把它经 VMap 映成本次副本重算值。

## 本轮实现

对已经可证明的多条原始入边，在现有预算内批处理；新副本和新重入边不进入当前候选列表。
未知状态路径、外层条件、内存大小检查、实际表读取、目标 switch/default 与完整状态保持。
每次改写后重算支配树，不允许把原块的 PHI 清空；这不要求在已有重入边时仍保留一条
已证明的原前驱直连边。普通无下游收益的单块回退仍每次最多复制一条入边。

对前驱上可用的旧执行值，先通过边上的快照 PHI 保存，再交给本次副本；快照不执行额外
LOAD、freeze 或调用。原 LOAD/freeze 在本次副本中照常重新执行，旧值只按前驱边修复。
若条件证明需要把旧表达式当成本次状态计算，仍保持 Unknown。

新旧值处理只扩大到确有有用下游常量 PHI 上下文的域，避免无收益单块复制。
快照块、去重的快照 PHI 和分支计入每上下文 6 块、256 条复制指令，以及模块 64 块、
16384 条复制指令的现有上限。SSAUpdater 的修复工作另受 live-out、外部 use 和
262144 repair cells 的既有预算约束；256 不是修复后全部最终指令的上限。
截短路径若丢失实际常量 PHI 收益，则拒绝新的旧值域及后续批处理，不增加默认预算。
默认优化、Graph、机器指令、公共 ABI、跨函数范围和表内容假设保持原契约。

另修复已知分支直接跳回检查块自身时的无收益复制。旧版本每次重新优化都会增长一节
副本链；现在保留该原始分支。该修复只保证此类直接自环稳定，不是任意循环的通用不动点。

## 回归与完整测试

三个 agent 分别负责实现、最终独立回归及原生/真实样本对照；root 负责整合、完整构建和
证据绑定。最终同一测试二进制分别连接冻结旧/新库，不靠缺少新 API 产生编译失败。
新增测试源 SHA-256 为
`3fa4af71c84338745c3752ef8791878bdd56dd26d0674f30927b35b7eca7552f`。

| 验证 | 实际结果 |
|---|---|
| 最终 12 个用例接旧库 | 12 cases / 351 assertions；8 cases、91 assertions 失败 |
| 最终 12 个用例接新库 | 12 cases / 503 assertions，全部通过 |
| 新旧相关组件合计，main / no-Z3 各自 | 47 cases / 3301 assertions，全部通过 |
| main CTest | 963/963，80.91 秒 |
| no-Z3 CTest | 828/828，71.04 秒 |

覆盖 old PHI/LOAD/freeze/calculation、自携带值、多入边、外层条件、效果屏障、poison
敏感 live-out、零预算、六块边界、模块上限和重复优化。原有 35 个组件用例未改。
中间实现曾让旧单块回退用例失败，最终恢复原契约；失败输出保留。直接自环用例是
分析稳定性验证，没有执行该已知无限循环，也不将其算作 native 返回匹配。
新增历史值正例采用 i64；pointer/其它位宽、多种旧值去重组合，以及没有 unknown 入边的
全已知前驱形状没有本轮专门正例。它们是验证覆盖空白，不应扩展为任意输入正确性保证。

## 独立语义与精度验证

详细命令、完整字段、失败门槛和来源见
[独立验证记录](loop-carried-context-independent-validation-20261111.md)。

- 原九模式夹具在旧库和新库各 **5760/5760** 完整状态匹配，故障观察各 2240 次，
  预声明错误各 **604/604** 检出，比较器各 15843 个字段/长度控制通过。
- 原夹具的 phi/load/freeze 另携带来自后续 S 块的旧定义，本轮仍停在该边界。
  新库原精度门槛实际失败，零 native 执行；之后未改 producer、模型或门槛，以原工具
  已有纯语义模式完成上述完整矩阵。不能把此组报作完整 table 特化。
- 单独隔离 old-B 的变体保留原夹具，且只将一个无标志循环计数 add 的 incoming 改为
  body 内等价独立计算。同源变体在旧/新库各 **1920/1920** 完整状态匹配、
  **604/604** 故障检出、5283 个比较器控制通过。旧版三个模式均零特化，新版均获得
  一条 body 入边上下文和实际 index16 target LOAD；旧库用同一新精度门槛确实失败。
- 未改的上一轮多块矩阵在新库 **6912/6912** 匹配，**486/486** 故障检出，
  19011 个比较器控制通过。这些是兼容验证，不增加真实样本覆盖。

全部正常比较包含返回值、调用次数、512 字节寄存器、64 KiB Guest、内存 descriptor
及七个 Exit 字段，无排除、跳过或归一化。两种 native 编译级别为 O0/O2；冻结候选库
为 main 构建。no-Z3 本轮执行完整 CTest 和聚焦组，没有新跑该原生矩阵。

## 真实样本收益与边界

四个冻结 ELF（f-cff/f-combo，各 C O0/O2），线性策略、StaticAnalysis/FunctionStart、
原生 product-O0，每个使用四个固定边界整数。raw 与 post-GVN 两条流水线各完成
**C16/LLVM16** 根返回全状态匹配：全部寄存器、1 MiB Guest、七个 Exit 及相应错误
控制均保持。Graph 未改，没有递归策略、新输入或 product-O2 的新增真实覆盖声明。

f-cff O2 两条流水线均一次得到 **16 个上下文、13 个不同状态**，七条非字面常量入边
仍保留。实际 table LOAD 的索引必须连到 target switch 才计为收益；每份仍有原未知
bounds、49 个 cases 和 default。CFF switch **1→17** 是复制，不是删除。

对同一旧真实 IR 的分析探针，旧版 16 次调用与新版一次调用都获得 16 个实际常量索引
LOAD、同一 13 个不同索引。因此这里提高的是单次有界批处理能力，不声称新增真实状态
可解析范围，也没有测量或声称 16 倍运行速度。

f-cff O2 新版 conditionsVisited=155、branchesRewritten=16、budgetExhausted=1，
complete=false，触及原 64 块复制上限。按 f-cff O0/O2、f-combo O0/O2 排序，改写数
为 **0/16/0/3**；四个模块 dispatch 均 incomplete，GuestSSA 为 raw2/post4 incomplete。
其余模块没有新增完整 target 上下文收益。静态复制后的可执行结果一致，不证明分析完备。

尚未解决：future-S 的旧执行依赖、超预算剩余状态、O0 的跨检查内存关系，以及实际表
内容的证明。不能把 `.rodata`、文件字节或一次成功运行当作不可变表证明，也不能把该
阶段称为完全去平坦化。没有扩展 VM、堆链表、未知调用副作用或跨函数状态传播。

## 冻结与证据

VM 的 `~/vmc-runs/loop-carried-root-20261111/final-runtime/` 指向已冻结 main 库。
main backend/shim SHA-256 分别为：

```text
3ad2fc5cec5dd709936788d786b03eeb7c94e9a4ccef92ab2e645949ddc6d9de
475286bd5ffcdac337306c3a1a8b559232ed4757fe23da49c7391a532f84bb5a
```

no-Z3 双库经 gzip 保留并解压复核，backend/shim 原始 SHA-256 分别为：

```text
461b47dfbc6b0d59d17a6d5d6cae547aa264a6e7240b4bf4bf1aef32bc96dbca
8c85ea41281b97f2239d47c2eb21fd7ba4e7417d624520e42395214dd3c58784
```

root 对 Mac 与实际 VM 的 **381 个生产/测试/验证源文件**逐项绑定，并独立绑定五组
native 报告及 producer/model 源快照。未重跑单指令/IGNIL 和完整 69 输入语料库；生产
变化仅在 opt-in LLVM 分析辅助实现，不把 IR verify 或这些矩阵称为全程序通用语义证明。

已完成的分工归档位于 VM `~/vmc-runs/` 与 Mac
`/Users/c/Desktop/deob/validation-artifacts/` 下的对应子目录：

| 归档 | SHA-256 | 实际普通文件数 |
|---|---|---:|
| `multistate-impl-20261111-artifacts.tar.gz` | `519a694452760714777d9f58edb82cf48382825322275822801ae10624450fbe` | 62（含 manifest） |
| `loop-state-unit-tests-20261111-artifacts.tar.gz` | `d03dac03d870f0e5df58869db99d7d3e12d016f237d72b09502dd797862e3585` | 61（含 manifest） |
| `loop-carried-reference-20261111.tar.gz` | `d59b5293accf773d2bfeb4ddec45a8836e4d75bdace7c08e9c7a6a979ced6024` | 1012 |

独立证据归档双端 SHA 一致，1060 个 tar members 完整读取；20 条正常 before/after
snapshot streams 经只读全字节复审。expanded reference 目录为释放 VM 空间已移除，
完整双端 tar 保留，VM 同名 `-ARCHIVED.txt` 含恢复命令。root 的构建/绑定归档在最终
交接提交中记录。
