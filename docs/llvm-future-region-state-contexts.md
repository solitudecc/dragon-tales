# 后续块历史值的入口快照

基线 `0b5c2ec`。本轮扩展 `dispatch-specialize` 的循环历史值处理，没有修改默认流水线、
Graph、机器指令、公共 API/ABI、输入内存约束或复制预算。

## 具体缺口与实现

原独立九模式夹具中，body 回到首块 B 时把后续 S 的 `nextIt` 作为 B.iteration 的
incoming。它来自上一次执行，且 S 的定义支配 body 的末端，是合法 SSA。旧代码在
`observesEarlierExecution` 发现此关系后直接停止扩展，因此 phi/load/freeze 三种模式
都达不到实际 target LOAD。此前另设的 isolated 变体没有替代或修复原夹具。

现在仅对**首块选中 PHI 的直接 incoming**放行：定义必须支配原前驱，且原有类型、
可复制块和条件证明门槛全部通过。若其定义属于本次实际复制路径，统一经边上的 snapshot
PHI 保留历史身份；后续 S 的新 LOAD/freeze/计算仍按原位置重新执行。SSAUpdater 修复
snapshot 的 predecessor use，取该边上上次执行的实际定义，不在首块重算。

普通 instruction operand 对未来块的旧引用、下游 PHI 的自携带或未来历史引用仍不放行。
未知/poison 条件、CALL、STORE、volatile/atomic 和同步屏障继续保留原边界。

预算截短时重新收集快照输入：若 S 不再被复制，S 中的旧定义作为已有外部 SSA 保留，
不再占一个多余的快照块。实际 snapshot 块、去重 PHI、branch 和复制工作计入原上限；
SSA 修复仍另受既有 live-out/use/repair cells 限制，没有提高默认预算。

## 验证与结构断言更新

原有 corridor 用例 “does not reinterpret an old future-region value as a fresh
execution” 的 IR 输入保持相同。其两条旧结构断言要求不复制 S、freeze 直接取原 LOAD，
在合法 snapshot 扩域后不再描述唯一安全形状。v1 集中运行保留实际失败：47 cases 中
46 通过，仅这两条失败；其余 3299 assertions 通过。

最终用例保留名称、输入和历史值语义，升级为检查：body-edge snapshot、支配 body 边的
历史 LOAD 来源树、新 LOAD 的真实重执行、原 guard/unknown/cases/default/重入及完整
verifier。没有删除或跳过该用例，也没有靠形状特判拒绝合法 LOAD。

新增专门回归覆盖 S PHI/LOAD/freeze/add、pointer/窄整数、重复及不同历史输入、外层
条件、实际 target LOAD/switch 连接、六块边界及截短、模块/零预算、效果和 poison。
最终同一测试二进制 SHA-256 为
`4a44162b6efe1806e0d876183bf54e14fe653c86f1841d03628d68f49ae79ab5`。

| 验证 | 最终实际结果 |
|---|---|
| 新 12 用例接旧 3ad | 12/257；7 cases、38 assertions 失败 |
| 新 12 用例接新 ec77 | 12/753，全部通过 |
| 最终组合接旧 3ad | 59/3556；8 cases、40 assertions 失败 |
| 最终组合接新 ec77，main/no-Z3 各自 | 59/4087，全部通过 |
| main 完整 CTest | 975/975，93.74 秒 |
| no-Z3 完整 CTest | 840/840，81.95 秒 |

动态子节保证旧版发生 fatal REQUIRE 后，PHI/LOAD/freeze/add、pointer/i16 等其它正例
仍实际执行。原 corridor fixture 声明、此前 helpers 和该用例的构造代码均逐字保持，
构造片段 SHA-256 为 `d828fb846845bbd279979ab31829f1e8247c3f8b5678b8a206cfb88a352cbefc`。
初版 fixture 的 C++ 类型编译错误、过强的“每条 body 都必须新建 snapshot”布局断言
失败均保留；后者忽略了第一次修复已使后续入边旧值可作为支配前驱的外部 PHI 使用。
最终要求每条边的历史身份、支配关系和实际 target LOAD 连接，不指定多余快照的布局。
中间尝试不累加到最终数字。

## 原独立矩阵的真实新增域

未修改 `loop_carried_context_native.cpp`、字节模型或 `--require-gains` 精度门槛。
冻结旧库 3ad 实际在同一门槛失败，normal=0；新库 ec77 的原九模式矩阵通过精度门槛，
5760/5760 全状态匹配、2240 个 faulty 观察中的 604/604 预声明错误检出，15843 个
比较器字段/长度控制通过。phi/load/freeze 的 body_redirected 和实际 index16 均为 1，
五种效果屏障仍零改写。此处是原始 future-S 域的新增精度，不再借隔离变体表达该收益。

上轮 isolated 1920/604 与 corridor 6912/486 也保持完整状态匹配，原 gate 和模型未改。
所有正常 native 比较涵盖返回值、调用次数、512 字节寄存器、64 KiB Guest、内存
描述符及七个 Exit；O0/O2 两编译级别，无新增排除、跳过或归一化。
上述独立 native 使用 main 冻结库；no-Z3 本轮运行完整 CTest 和聚焦组，没有另跑该
native 矩阵。pointer/i16 等新增正例是结构/SSA 验证，不冒称已获得对应类型的原生矩阵。

## 真实样本与后续阻塞

仍使用 f-cff/f-combo 的四个冻结 C O0/O2 ELF、线性策略、StaticAnalysis/FunctionStart、
相同四个边界整数和 product-O0。每轮区分 C/LLVM 根返回完整状态、实际 target 上下文
收益与分析 incomplete；不把执行匹配当作完整去平坦化。

本轮 raw/post 两组各 **C16/LLVM16** 完整根匹配；各 16 个寄存器、16 个内存及
112 个 Exit 字段错误控制均检出，Graph 不变，冻结 runtime/输入 hash 均保持。
f-cff O2 仍为 **16 个上下文、13 个不同状态、七条未知入边保留**；两流水线模块改写数
仍为 **0/16/0/3**，dispatch 四模块 incomplete，GuestSSA raw2/post4 incomplete。
没有新增真实 CFF 消除或真实状态覆盖；本轮新增精度由原 future-S 独立夹具实际证明。

本轮只读调查确认 f-cff O0 的两个具体 blocker：

1. `memory.valid9` 的 state PHI 有已知 50 入边，但检查块内有真实 Guest STORE；
   `StateGuardEdges::safeBlock` 因而拒绝整个复制。它在原遍历的条件 #84，两个 target
   switch 在 #86/#162，都早于条件预算 256；当前此处并非遍历预算没有触达。
2. 回边常量 STORE 在 `memory.valid67/93/117/205/237`，经 PHI hub `bb_67382` 再到
   `bb_66234` 和 `memory.valid7` 的 state LOAD。当前 nearestStore 只检查 hub 本块，
   无 STORE 就停止；其它直达前驱为 switch，也不作为 selectedEdge。选定精确 hub 入边
   后 RBP PHI 的替换可使 LOAD/STORE 地址树相同，不需要假定全局 RBP=entrySP-8。

后续最小改动是保留 simple STORE 原顺序/次数的显式上下文复制，以及能够逐条选边、修复
所有 PHI 的多级 checked-memory hub。不能只把 nearestStore 向前多扫一块而丢失路径身份。
若新选项扩展 C++ 接口，应增加 overload 保留原符号和旧对象大小；直接给按引用传入的
public options struct append 字段可能让旧客户端触发越界读取。

实际表项仍是任意合法 Guest 内存的一部分。LOAD 后再加 target==candidate 并回退原
switch 只是剥离原 switch 的一个 case，没有新增目标证明，也不计去平坦化。无条件消除
仍需路径内已证明的 STORE 或显式受限入口的内存事实，并在未知写/别名/调用后失效；
文件表项、只读 section 名称和一次成功重放都不能升级为内容不变性证明。

## 冻结与证据

main 冻结双库在 VM `~/vmc-runs/future-region-root-20261111/candidate-runtime-v1/`，
backend/shim SHA-256 为：

```text
ec77a3960eab53eb9e8bf93cc7d1ae00dcb720c270542e3b6b7f190a6c625f7f
475286bd5ffcdac337306c3a1a8b559232ed4757fe23da49c7391a532f84bb5a
```

no-Z3 backend/shim 原始 SHA-256 为：

```text
f00bd54a3939150c16e22bac8634831853a02d0975386c1b11f6ca4d0056818b
8c85ea41281b97f2239d47c2eb21fd7ba4e7417d624520e42395214dd3c58784
```

[独立验证记录](future-region-independent-validation-20261111.md)包含全部命令、源 hash、
表项契约、16 条完整正常 snapshot streams 的独立字节模型复审及具体盲区。没有新增
单指令/IGNIL 或完整 69 输入语料库 campaign；本轮只变更 opt-in LLVM 组件。

完整双端归档位于 VM `~/vmc-runs/` 和 Mac
`/Users/c/Desktop/deob/validation-artifacts/` 下对应目录：

| 归档 | SHA-256 | 文件数 |
|---|---|---|
| `future-region-unit-tests-20261111-artifacts.tar.gz` | `5d064b6019612d59c6270e1b57a153444649fe5d7a52e6fe31a8274d470692b6` | 104 regular / 111 members |
| `future-region-reference-20261111.tar.gz` | `ab660459cd88de7eb45268f5b8b15befe84da964fd78d54bf536e45bd76253f1` | 772 regular / 808 members |

root 又逐项复核 103/771 个数据文件及 native 源快照，旧用例构造保持同一输入；root
全构建/源码绑定/冻结包将在最终交接提交中记录。已归档的 unit/reference、旧
state-context-root 和 loop-carried-root expanded 目录为节省 VM 空间移除；全部文件和
旧运行库仍在经双端核对的完整 tar 中，各目录相邻 `-ARCHIVED.txt` 含恢复命令。
