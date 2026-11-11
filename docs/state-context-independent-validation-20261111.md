# 多级状态上下文：独立验证

基线 `947f9c1`。本轮验证首状态检查 B 的已证明入边延长至共享 bridge U、未知 bounds
检查 J 及实际 target LOAD/switch S。复制哪条安全 continuation 属于覆盖策略；原未知
条件及未选出口仍执行，不能把选择一个副本解释为证明该分支必然成功。

## 独立 native 域

`state_context_native.cpp` 不调用机器 lifter 或单元测试 builders；Python 字节模型不调用
生产证明器。九个模式、1728 个固定输入，before/after × O0/O2 共 6912 次完整观测。
每次比较返回值、调用次数、512 字节寄存器、64 KiB Guest、七个 Exit 字段及 descriptor
的 data/size。寄存器、Guest、descriptor 和 Exit 是不同宿主对象，没有添加 noalias。

B 的 PHI 有已知 50 与三条未知入口。U 保留 size LOAD、外部 entry LOAD 的 PHI、业务
flags；J 再次汇合状态并执行未知 logical size 检查；S 读取实际 Guest table，包含两个
行为不同的合法 payload、回到原 B、回到原 J 与 unmatched default。回 B 采用未知 x，
回 J 采用 state xor 1，避免把重入误接到使用旧状态的副本。输入显式选 0/2 次重入预算，
最多两次回入后仍执行实际 LOAD，再选择 payload；所有原生路径有限且宿主访问有定义。

| 模式 | 新精度门槛 |
| --- | --- |
| basic | copied unknown bounds 及 copied target LOAD，索引可局部常量化为 0x590 |
| two_bridge | 同上，第二 bridge 的额外 LOAD/flags 也保留 |
| outer_guard | 同上，外层拒绝路径先执行 |
| entry_value | 同上，选用另一 entry LOAD 的外部 SSA 值 |
| unknown_call / store / atomic / volatile / fence | 不跨 effect bridge 延长；原有首 B 单块特化仍可保留 |

table 字节只在宿主初始化为 oracle 输入，不作为分析事实。logical size 取已知索引所需
范围前一字节及完整末字节；x 覆盖 0/1/2/49/50/51、0x7f/0x80/0xff、0x80000000、
2^63 和全 1。四种 route 与 0/2 rounds 全组合。复制后的 switch case/default 必须与
原 switch 一致，回 B/J 必须指向原块。

精度只读测量在模块私有副本上使用 LLVM ConstantFoldInstruction，做局部常量传播，
不会对原 native 输入执行额外 CFG 优化，也不复用生产证明器。未知 size 分支仍须有条件，
table LOAD 及 switch 保留。五个故障 IR 分别错误处理 unknown state、删除 flags、
跳过 memory failure、串用初次状态和错接 default。差异集合先由独立 fault 模型给出，
不根据实际 native 输出选取。故障程序也只访问已分配宿主内存。

## 冻结旧库与失败控制

旧 runtime：`~/vmc-runs/state-guard-root-20261111/final-runtime/`，backend
`4b20733fb35f70101f549c7e8ca6aa1249d500a6055d408d519d17c4fd675d03`。
相同源码的 `baseline-native-attempt001/` 为 **6912/6912 完整状态匹配**；
1920 次故障 IR 观测中 **486/486** 个指定差异检出，**19011** 个单字段/长度控制通过。
九模块均只有旧首 B 特化，table LOAD/bounds 各 1，新的上下文索引门槛均未达到。
`baseline-gain-gate-negative/` 对同旧库实际返回 1，normal=0，证明新精度门槛会抓住缺失。

候选 v1 backend `e0483934cbc7e7e8411faaf572ee48116dce11ee16604a7cff82dedd26a60493`
的 `candidate-native-v1/` 未通过精度门槛，尚未执行 native（normal=0）。它复制了 J，
但 J 的两个 pure 出口都安全，旧唯一 continuation 策略停在此处，没有复制 S。原 fixture
和门槛保留，未把 failure 块改成 effectful 来迎合选路。后续生产将安全选择一个可复制的
continuation，仍保留未知条件和另一路径；选择本身不是成功分支证明。

旧 4928 状态守卫矩阵的精度版要求每正例恰增加一块，不能用于新的多块功能。代码及旧
精度门槛不改，本轮以其原有不带 `--baseline`/`--require-gains` 的纯语义模式复验。
候选 v1 在该有限域的 4928 次观测匹配，但其它旧单测发生实际 SIGSEGV；因此不能把它
算为最终候选通过。原生域未触发的崩溃仍由主 agent 修复并补回归。

## 真实四 ELF 验证契约

`state_context_real.py` 复用未改动的四 ELF 完整状态 replay，只在 dispatch-specialize
前后注册 read-only IR 捕获，并添加独立 context 门槛。旧单块的
`--require-state-guard-gain` 及其 switch 数量门槛没有放宽。

新门槛针对已冻结 f-cff O2 的真实 RCX PHI incoming=50，检查 B/U/J/S 路径、未知 bounds
实际依赖复制路径上的 LOAD、所有原 unknown incoming、原 table LOAD、完整 switch cases
和 default，以及标准 pipeline 后实际 Guest LOAD 的常量字节索引 70096（0x111d0）。
raw copiedS 的 switch selector 必须来自其实际 LOAD；cleanup 后常量 LOAD 也必须作为
target switch 的输入（允许 PHI/freeze identity），不将无关的常量地址 LOAD 计作收益。
复制的 target switch 只统计为副本，不称为 CFF 消除。表内容、只读节和一次执行不构成
不变性证明。测量器的 **1 个正例、10 个指定假收益对照** 已通过，最后一项专门拒绝
“真实 table LOAD 存在但 switch 使用另一个值”的假收益。

旧原顺序、post-GVN 两种 baseline 各重新执行为 **C16/LLVM16 完整根匹配**，
Graph/input/library 不变，新 context=false，70096 LOAD 前后均 0。原有三个 dispatch
budget incomplete，以及原顺序两个/post-GVN 四个 GuestSSA incomplete 模块继续报告。
正常比较没有排除、skip、归一化或删减字段。

关联 actual target LOAD 的测量增强发生在旧基线已执行之后。对旧两份完整保存的
before/after-context IR 再做严格只读复审，仍为 context=false；
`baseline-real-*-strict-review.json` 标记 **executions=0**，保存原执行 summary 和 IR hash，
没有覆盖旧原始 summary，也没有把复审增加为新的执行。测量源码未修改 native 输入、
预期模型、完整状态比较器或 pass 顺序。

## 修复候选 v2 最终独立结果

冻结 runtime `~/vmc-runs/state-context-root-20261111/candidate-runtime-v2/`：

- backend：`5739c3be39d330522c1cdb5ed8653cf6a03485defd7b8ca826998b22c9ab6642`。
- shim：`475286bd5ffcdac337306c3a1a8b559232ed4757fe23da49c7391a532f84bb5a`。

`candidate-native-v2/` 使用与 947 baseline 逐字相同的三份 producer/model/比较器源码：
**6912/6912** 完整状态匹配，**486/486** 个指定故障和 **19011** 个比较器控制检出。
四个正模式 bounds 与 table LOAD 各从 1→2，bounds 保持未知条件，known50 索引实际可
常量化；switch 的两种 payload、default、回原 B/J 均保留。五个 effect bridge 模式
只保留旧首 B 单块特化，不延长，全部九模块 complete、unknown incoming 保留。
没有因 v1 精度失败而更改 fixture 或放宽门槛。

旧 4928 原生矩阵在 `final-state-compat/` 以原纯语义模式重新运行：**4928/4928** 完整
状态匹配，528 个故障观察中 **312/312** 个指定差异与 **13555** 个比较器控制检出。
这是已有输入的兼容复验，不能算作新的样本覆盖。

新原顺序与 post-GVN 两条真实流水线各为 **C16/LLVM16 完整根匹配**，输入、Graph 和
mapped library hash 全部固定。两者都仅 f-cff O2 出现 B/U/J/S 已证明初始状态上下文，
未知 bounds 依赖复制路径中的 size LOAD；copiedS 的实际 LOAD 提供 switch selector。
标准 pipeline 后，目标 switch 所用的 Guest LOAD 字节索引 **70096 从 0→1**。49 个
原覆盖 case、default、全部原 incoming 与回原 B/J 继续保留。每条流水线的寄存器、内存
故障控制各 16 个，七 Exit 字段故障控制各 112 个仍全部检出。

f-cff O2 的真实 CFF switch 数量为 **1→2**，表示一个受检查上下文副本，**不是消除**。
其余三个样本的 CFF 各 1→1，原程序 switch 四样本均 1→1；RIP router 清理不计收益。
三个整模块的 `dispatch-specialize` 仍各在 conditions_visited=257 触及预算并报告
INCOMPLETE；f-cff O2 无预算耗尽。GuestSSA 原顺序两个、post-GVN 四个 incomplete
模块保持显式标识。完整根返回不代表全部模块分析完备。

本轮没有改地址/别名分析、机器 lifter 或默认优化顺序；没有新增单指令/IGNIL campaign
或 69 输入全语料运行。有限定义输入、全部字段和故障控制不构成任意输入/整个优化器的证明。

证据根：VM `~/vmc-runs/state-context-reference-20261111/`。
