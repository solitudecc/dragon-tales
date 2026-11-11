# 多入边与旧执行值：独立验证

基线 `01c5abd`。本记录与生产实现及单元测试分开，使用独立构造的 LLVM IR、手写字节模型、
故障 IR 和未改动的完整状态比较器。它不增加表内容不变性、跨函数入口或默认优化假设。

## 先核对旧能力

旧冻结 backend `5739c3be39d330522c1cdb5ed8653cf6a03485defd7b8ca826998b22c9ab6642`
对上一轮实际 f-cff O2 post-GVN `after-context.ll` 再运行八次默认
`runDispatcherSpecialization`，每次都已有一条新增特化，complete、budget=0。
输入本已包含首状态 50 的一个上下文；八次追加调用后，副本数为九。
这些是静态分析调用，不是新 native 或 C/Unicorn 执行。

之后用与 C++ 默认对应的 `instcombine<no-verify-fixpoint>,simplifycfg` 作单独结构清理，
并追踪实际 target switch 所用 LOAD，得到状态集合 `{1,4,5,9,23,32,50}` 和索引
`{69704,69728,69736,69768,69880,69952,70096}`。不能声称旧版完全不能处理这些状态。
第一次首状态特化已通过 SSAUpdater 将其它常量入边的携带值变为 pred 上外部 PHI，
此时诊断中的 direct-old-B definitions 和不可支配 incoming 数量均为零。
本轮真实精度门槛因此比较一次调用内的批处理，而不是把旧重复调用的可解析域当新能力。

旧两条真实流水线的完整原始 before/after/最终 IR 已复制并作只读新门槛复审：均为
一个已知 50 上下文，七个非字面常量 incoming 保留。复审标记 native_executions=0、
analysis_runs=0，不重复计算先前 C16/LLVM16 的正式执行覆盖。

## 原始夹具与明确保留的限制

`loop_carried_context_native.cpp` 与 `loop_carried_context_diff.py` 有九种模式：

- `multi` 有 50、16、23 三个已知入口和一个未知入口，区分一次批处理与原有单入口迭代。
- `phi/load/freeze` 初始 state 来自未知参数，没有可先特化的已知 seed。body 的回边写常量
  state16，同时分别携带直接来自上一次 guard 的 PHI、LOAD 或 freeze(load)。body 在
  region 外真正写 Guest[0x100]，故下一次 LOAD 与旧值不同，错误复用会产生可观察差异。
- `call/store/atomic/volatile/fence` 将效果放在 guard 内，要求继续作为不可复制边界。

所有正常输入均定义；freeze 的操作数是已初始化的内存读取，不为 freeze(undef/poison)
指定人为固定结果。每次回入有明确的 0/2 次预算，包含两个合法 payload、回首块、回后续
join 和未知 default。所有正常访问都在实际分配及约定的逻辑映射内；错误 bounds 控制
仍只读取已分配的宿主缓冲区。

四个 route、两种 size、两种回入预算和十个边界整数共 1440 输入；before/after 各以
O0/O2 编译执行，共 5760 次观察。逐项比较返回值、调用次数、全部 512 字节寄存器、
64 KiB Guest、descriptor data/size 和七个 Exit 字段，没有删减字段、skip 或归一化。

七个错误 IR 控制分别串用新 state16、新写入后 LOAD、新 freeze，以及绕过未知 state、
bounds/default 或改坏 flags。应失败集合由独立故障字节模型事先计算，不从实际结果
反推。比较器还逐字段翻转并检查长度错误。

## 冻结旧库实跑

`baseline-native-attempt001` 为 **5760/5760** 完整状态匹配。2240 个故障观察中的
**604/604** 预声明差异全部检出，**15843** 个单字段/长度控制通过。
`multi` 只特化第一个 50 入口；直接 old-PHI/LOAD/freeze 三模式全无改写。
旧库的 `--require-gains` 精度负控制单独失败，未执行 native，不混入语义比较。

候选实现 `e00d6da` 的冻结 backend 为
`3ad2fc5cec5dd709936788d786b03eeb7c94e9a4ccef92ab2e645949ddc6d9de`，shim 为
`475286bd5ffcdac337306c3a1a8b559232ed4757fe23da49c7391a532f84bb5a`。
第一次对原始夹具使用 `--require-gains` 时，IR 验证通过，但 phi/load/freeze 没有到达
常量索引的 table LOAD，因此精度检查失败，native 执行数为零。该夹具还有一个独立的
future-S 依赖：B.iteration 的 body incoming 直接使用后续 S 中的 add。原有保守契约
会在复制 S 前停止；本轮没有扩大该契约，也没有修改原 producer、model 或精度 gate。
失败的 `candidate-native-v1` 全部文件保留。

随后只用原工具已有的纯语义模式复跑完整原始矩阵，**5760/5760** 全状态匹配，
2240 个故障观察中的 **604/604** 预声明差异检出，15843 个比较器控制通过。
原 `multi` 一次调用得到三入口，其余三个正例得到 B/U/J 安全前缀，仍不把它们报成
完整 table 特化。五种效果屏障仍为零改写。这是原 shape 的语义兼容证据。

## 单独隔离 old-B 的新增域

新增 `loop_carried_isolated_native.cpp` 直接 include 原 producer，通过重命名 main
保留原文件逐字不变。它只对 phi/load/freeze 三个模式，将 B.iteration 的 body incoming
改为 body 内独立计算的同一个无标志 add；S 的原 add 仍供 S→J 回入使用。没有改变
数据、控制或调用行为。`loop_carried_isolated_diff.py` 直接复用原 byte model、invoke、
helper 和字段比较器，输入为原矩阵中这三个模式的全部 480 输入。

同源变体在旧、新冻结库各运行 before/after、O0/O2，共各 **1920/1920** 全状态匹配。
各有 2240 个故障观察，**604/604** 预声明差异检出，**5283** 比较器控制通过。
旧库三模式均零特化；新库三模式均恰一条 body 回边特化并保留实际 index16 LOAD。
将同一新精度 gate 用在旧库上确实失败，native 执行数为零。
本变体证明的是 direct old-B PHI/LOAD/freeze 的历史值域，不是 future-S 支持；
与原始矩阵重叠的输入不计为新增真实样本覆盖。

旧 corridor producer/model 未修改；新库还完成原 **6912/6912** 语义兼容观察，1920 个
故障观察中的 **486/486** 预声明差异检出，19011 比较器控制通过。原单入口精度 gate
保留；本轮的多入口精度使用新 gate，不改旧 gate 来迎合新副本数。

## 真实四 ELF 的新单次调用

`final-real-original` 和 `final-real-post-attempt002` 两条匹配流水线各完整完成
**C16/LLVM16**，Graph 未变，寄存器、完整 1 MiB Guest 与七个 Exit 字段的故障控制
均检出。每条流水线只用原来的四个输入，不声称增加输入或样本种类。

f-cff O2 两流水线均一次调用得到 **16 个完整上下文、13 个不同状态/索引**，七个
非字面常量 incoming 全部保留。每个上下文仍有未知 bounds、实际 table LOAD、
49 个 cases 和相同 default；原 LOAD/switch 仍在。13 个状态为
`{1,2,4,5,9,11,12,23,32,41,42,48,50}`。16 个副本不能当作 16 个不同状态，
CFF switch **1→17** 是复制，不是去掉 16 个分发器；原程序 switch 均 **1→1**。

此时 f-cff O2 的 conditionsVisited=155、branchesRewritten=16、budgetExhausted=1，
明确 complete=false。其余三模块改写数分别为 0、0、3；两条流水线四模块的 dispatch
都报告 incomplete，GuestSSA incomplete 为 raw2/post4。不把可执行结果一致当分析完备。
旧重复调用已有同域能力，本轮真实收益是有界单次批处理，不声称新真实状态覆盖。

post 的第一次启动误把 runtime 目录作为 `--frozen` ELF 输入，FileNotFoundError 在
执行前终止，零比较；该次目录和 stderr 保留。修正输入路径后的 attempt002 完整通过。
真实测量保留旧单块与旧多块 gate，使用独立的多入口 gate；复制 switch 只计副本。

## 只读总审计与归档

`final-readonly-audit.py` 对十组正式或预期失败记录作零执行、零分析的只读总审计：
核对每个 native artifact SHA、原 producer/model SHA、五组共 20 条 gzip 原生状态流
与独立 expected 的全部字节相同，并核对真实运行库、输入、控制和完备性记录。审计通过。

证据目录：VM `~/vmc-runs/loop-carried-reference-20261111/`。全部 1012 文件已归档：

- VM `~/vmc-runs/loop-carried-reference-20261111.tar.gz`
- Mac `/Users/c/Desktop/deob/validation-artifacts/loop-carried-reference-20261111/loop-carried-reference-20261111.tar.gz`
- 两端 SHA-256 均为 `d59b5293accf773d2bfeb4ddec45a8836e4d75bdace7c08e9c7a6a979ced6024`

归档包含原 future-S 精度失败、旧两种精度 gate 负控制、零执行的错误输入路径日志、
全部正常/故障快照和源文件、旧真实只读复审以及旧八轮重复调用 probe。
可用 `tar -xzf ~/vmc-runs/loop-carried-reference-20261111.tar.gz -C ~/vmc-runs`
恢复原目录。原生完整状态流已经 gzip；没有再复制大型 runtime 库。
