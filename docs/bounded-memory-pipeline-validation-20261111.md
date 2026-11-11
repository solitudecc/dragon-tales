# 显式三轮 Guest 内存与分发流水线验证

这份记录区分流水线 API 接入、实际状态覆盖收益、独立执行语义和诊断计数。中间版本的严格计数等价门槛失败，不能因 IR 相同或执行一致而改记通过。

## 固定执行方案

新增 `scripts/validation/bounded_memory_pipeline_real.py` 复用原 `guest_memory_checked_edges_real.py` 的反汇编、提升、C/LLVM 执行、完整状态比较与所有故障控制。代理仅检查并记录旧 oracle 的优化请求，替换为明确的七步配方：

```
mem2reg
instcombine
gvn
guest-memory(run_mem2reg=0)
dispatch-specialize(allow_plain_stores=1)
control-flow
dce
```

使用既有 `max_iterations=3,max_pass_runs=32`。每轮加一个只读 checkpoint，记录实际 IR 和真正由目标 switch 使用的 Guest 表 LOAD 索引，因此实际是每轮 8 个 pass、最多 24 次调用。没有 MBA、谓词折叠或 deflatten pass；没有修改默认流水线。旧 B244 以同名 callback 调用公开 `run_guest_memory_ssa`，新库使用内建 `guest-memory`；其余步骤和选项相同。

四个冻结 ELF 为 f-cff/f-combo 的 O0/O2。每个四个边界输入为 `0,1,0x80000000,0xffffffff`；真实 megaInit 和调用路径保留。三轮最终产物执行完整 oracle，逐轮 checkpoint 仅用于精度统计，没有另算成原生执行或新样本。

新覆盖检查自测有 2 个合法报告和 6 个错误顺序/数量/轮次控制；严格比较器自测有 1 个合法报告和 5 个错误计数/状态/字段控制。均拒绝全部指定错误；这些自测不执行目标程序。

## B244 callback 与 9e7 内建接口的中间结果

旧 backend SHA-256：`b2449677c1cfaa247d90286dd4ed8dbefbf944c0f2ee1e30488c3c1eac4e747f`。

中间 backend SHA-256：`9e7c77e085124621152326c100d6715ba3a6b12667f3afe2f2ff610a9bbc92f0`。

共同 shim SHA-256：`e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec`。

两组各自完成 C 16/16、LLVM 16/16 完整 root 对照；每组的 16 个寄存器控制、16 个完整 Guest 内存控制和 112 个 Exit 字段控制全部检出。Graph 未变，运行前后输入与运行库 SHA 固定。没有新增排除、跳过或归一化。四个模块各有 before、after 和三份 round IR，共 20 份 old/new IR **逐字节相同**。

但 `bounded_memory_pipeline_compare.py` 原严格门槛报 `RuntimeError: shared memory counters differ`。原日志保留，比较器未放宽。它要求 15 个共同字段精确相同，并将 `PassResult.changed` 与 counter `changed` 分开。旧 C/Python 接口没有暴露 `leaf_calls_preserved,summary_calls_preserved,summary_calls_rejected`，旧侧记录 unavailable/null，不能填 0；新版另要求 `available=1`。这三个新增字段没有 old/new 对齐证明。

| 模块 | 第 1 轮真实索引 | 第 2 轮增量 | 第 3 轮增量 |
|---|---|---|---|
| f-cff O0 | 70032 | 69760 | 69736 |
| f-cff O2 | 13 个既有索引 | 无 | 无 |
| f-combo O0 | 无 | 无 | 无 |
| f-combo O2 | 73904、73952、74048 | 无 | 无 |

两版本实际索引完全相同。O0 的后两轮收益来自显式重复的既有算法，不能算作内建接口新增证明能力。f-combo O2 在第一轮已有三个索引，也不能说是第三轮增长。

f-cff O2 的三轮 IR 收敛，另三个模块达到配置轮数上限；四个模块都仍出现 Guest-memory incomplete。收敛不等于分析完备。CFF switch 数从 1 分别变为 4/17/1/4，是保留原分发器的上下文复制，不是删除平坦化；原业务 switch 均保持 1→1。表 LOAD、未知路径和默认出口仍然保留。

## 同库不同分配布局的独立诊断

`guest_memory_counter_repeat.cpp` 对完全相同的 f-cff O2 第 1 轮 IR 做 8 次分析。解析前仅分配不同数量的 `sizeof(llvm::BasicBlock)` 内存块，padding 为 `0,1,7,31,127,257,511,1023`；每次使用独立 LLVMContext，不修改输入 IR。

B244 的 loop_barriers 为 `62,37,35,65,37,37,63,65`，join_barriers 为 `0,1,0,0,0,1,0,0`。9e7 的 loop_barriers 为 `62,37,35,35,35,37,35,64`，join_barriers 为 `0,0,1,1,1,1,0,1`。各组所有 before IR 同 SHA、所有 after IR 同 SHA。总共 16 次分析，**0 次目标原生执行**。

这证明旧库本身已有地址布局相关的结果统计变化，不能把它归因于新适配器，也不能据此断言所有语义安全。随后的源码诊断发现两处自然循环收集将全局 loopBlocks 当作单个回边 walk 的 visited，可能在先访问内环后漏掉外环成员，影响 LOAD 延迟处理。这是需要修复的循环保守性问题；本中间实验未证明旧库存在目标程序执行错值。

第一次探针链接同时使用 LLVM shared 与 libdragon，启动报 LLVM option 重复注册，未开始分析；失败二进制/编译日志/启动错误也保留。第二次只链接 libdragon 后正常完成上述 16 次分析，不能将第一次启动失败计为语义失败。

## 中间证据归档

VM：`~/vmc-runs/bounded-memory-intermediate-20261111.tar.gz`。

Mac：`/Users/c/Desktop/deob/validation-artifacts/bounded-memory-intermediate-20261111.tar.gz`。

SHA-256：`d37cdcd1192b30f03c1d29b3fc43d4cc996f15298c84637ac92ecdcfea5cb939`。两端逐一核验全部 331 个文件的集合与内容 SHA 后，VM 才退役 baseline/candidate/counterprobe 展开目录；完整原始输出、错误控制、IR、计数和失败日志全部在 archive。B244 临时运行库不在这个包内，它来自此前完整 store-hub bd0 archive，历史上曾供独立嵌套循环 oracle 使用。所有使用者确认完成后，该临时 backend 也已退役；相邻 ARCHIVED 通知指向完整 bd0 archive。

18 个 oracle/metric 依赖源码与 ABI 头在中间运行后补存快照，Mac 隔离基线与当前 VM 源码 SHA 全相同；最终候选运行前后已重新核验。后续循环修复使用新 namespace，不覆盖本轮 9e7 证据，也不要求已知有缺陷的旧循环计数成为新语义真值。

## 循环成员修复后的最终独立验证

最终 backend SHA-256 为 `2e509ceb03bc5b47d2e95fdacfd579860e520f238ee239746c273b6034ba9bda`，源码循环修复提交 `ee041f9`。shim 保持 E2。在独立 `candidate-builtin3-v2` namespace 使用同一冻结 wrapper 完成真实矩阵，C 16/16、LLVM 16/16，16 个寄存器、16 个 Guest 内存及 112 个 Exit 字段错误控制全部检出，排除/跳过/归一化均为零。18 个依赖源码和 runtime 在执行前后 SHA 一致。

四模块最终各执行 24 个 pass（七个变换加只读 checkpoint，三轮）。f-cff O2 的整体 pipeline `converged=true,budget_exhausted=false`；另三个 `converged=false,budget_exhausted=true`。Guest-memory 的全部 12 次调用均 `status=INCOMPLETE,complete=0,budget_exhausted=1`。dispatch-specialize 有 10 次 incomplete；仅 f-cff O2 的第 2、3 轮成功，仍不能称该模块分析全部完备。每次 Guest-memory 调用都针对当前 LLVM IR 重新构造事实，不把前一轮证书跨 mem2reg/instcombine/GVN 等通用变换复用。

最终真实索引仍是前表中的集合和增量。Guest loads_forwarded 按轮分别为 `[1,1,1] / [0,0,0] / [4,4,0] / [0,0,0]`；dispatch branches_rewritten 分别为 `[17,1,1] / [19,0,0] / [91,2,0] / [18,1,0]`，这些是整个模块的计数，不是还原 CFF 状态数。CFF 1→4/17/1/4、原业务 switch 1→1 均保持；没有声称去掉真实平坦化。

从完整中间 archive 读取旧 B244 的 20 份原始 IR，与最终修复的 20 份 IR **逐字节相等**。`fixed-final-review.json` 逐 pass 记录实际计数差异和不可比较的新字段，原 strict-counter gate 继续标记 false。最终语义以独立 C/完整 Guest oracle 判定，不要求修复后计数复制有缺陷的旧循环集合。

同一个已冻结 C++ helper 对同一 IR 再跑 8 个 padding，最终 `loop_barriers` 全为 35，`partial_overlaps` 全为 28；`join_barriers` 仍为 `0,1,0,0,0,1,1,0`。before/after 各自只有一个 SHA。此组是 8 次分析、0 次目标原生执行：循环计数的布局波动在该输入消失，但 join 计数仍有布局相关性，不能声称所有统计具有确定性。这不改变 15 字段严格对齐失败的历史结论。

另外增加 `bounded_memory_pipeline_audit.py`，对保存的真实 IR 做独立后置审计。冻结 wrapper 本身只记录度量，不冒称其返回码原已包含该门槛。三组现在都通过同一审计：每轮实际目标 LOAD 索引与保存记录一致，O0 必须达到 `{70032}`→`{70032,69760}`→`{70032,69760,69736}`，四模块共八个原目标 switch 的 case 地址集合、默认 Exit 来源 PC 和 selector 的 LOAD 依赖保留。

这一 whole-pipeline 检查允许 LLVM control-flow 拆 critical-edge、重命名块，因此不要求目的块的文本标签相同；没有更改 case 地址值或默认来源的比较。自测的 2 个正例包括合法块重命名，4 类 case/default-source/LOAD/移除 switch 错误全部拒绝，并继承度量器 7 个假增益及 2 个 switch 损坏控制。所有这类审计仅阅读保存 IR，不新增目标执行。最初尝试复用单独 dispatch 阶段的同标签检查不能用于整个优化流水线；该探索未作为通过证据。

## 最终证据包与恢复

最终 VM 包 `~/vmc-runs/bounded-memory-final-20261111.tar.gz`，Mac 包 `/Users/c/Desktop/deob/validation-artifacts/bounded-memory-final-20261111.tar.gz`，SHA-256 为 `b0585e4b8dedbb75934f639fce1e98ec59327b9a931dce684d8970fb2e087dac`。VM/Mac 独立逐项核验全部 215 个文件集合及内容 SHA 后，才退役 VM 的 `candidate-builtin3-v2` 与 `counter-probe-fixed` 展开目录，摘要、pre/post pins 和恢复说明仍保留。Mac 也有完整展开副本。

最终包保存完整最终 native 原始流、故障控制、每轮 IR、固定 IR padding、三个运行器以及依赖快照和中间包绑定。中间完整 331 文件在前述 d37 包中；两个包一起构成本次历史证据，不能将最终包误称为已包含全部中间原始流。最终库本体由 root 的完整运行库档案保管，本独立包固定其 SHA，不复制已另存的运行库。恢复最终组：

```sh
tar -xzf ~/vmc-runs/bounded-memory-final-20261111.tar.gz -C ~/vmc-runs
```

`FINAL-ARCHIVED.txt` 和 `INTERMEDIATE-ARCHIVED.txt` 分别提供恢复位置。原 strict 计数失败和残余 join 布局波动均保留，归档不会将失败重新标记为通过。

## 覆盖边界

这是四个既有真实 ELF 的显式编排与完整 root 执行验证，不是 69 样本语料重跑，也不是新指令/IGNIL 语义验证。它不证明所有循环不动点、表内容不变性或全 CFG 可达性。嵌套循环的独立 C/Guest 原生 oracle 由单独验证组件负责，最终结果应另列，不能以本记录的 IR 相同替代。
