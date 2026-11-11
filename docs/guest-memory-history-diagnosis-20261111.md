# 三轮最终 IR 的剩余状态传播阻塞

基线为 `9f9e5f9`，只读输入来自已双端核验的 `bounded-memory-final-20261111.tar.gz`（SHA `b0585e4b8dedbb75934f639fce1e98ec59327b9a931dce684d8970fb2e087dac`）。本次诊断只抽取四份最终 IR 和 summary，没有重跑真实矩阵。以下行号是冻结 IR 文件自身的行号；代码行号对应本次修复前基线。文本阅读能定位拒绝规则，不能替代新修复的执行对照。

## f-combo O0：有常量状态，先被历史执行引用规则拦住

IR `f-combo/O0/s0-product-O0/after-pipeline.ll` SHA `09932ad854dbf0c5d488342e915c0fc33d9eb9cd986385af53ce6697ba18010d`。

- L8825 `memory.valid1203`: `store i64 14, ptr %1071`。
- L8958 `memory.valid1239`: `store i64 34, ptr %1071`。
- L12382 `memory.valid1588`: `store i64 32, ptr %1071`。
- L12974 `memory.valid1712`: `store i64 0, ptr %1071`。
- 上述路径都经 L2694 `bb_89568`，再到 L1201 `bb_68277`，最后到 L4886 `memory.valid131` 的 `%1072 = load i64, ptr %1071`。
- `%1071 = getelementptr i8, ptr %2, i64 %98`；`%98 = add i64 %RBP.13, -168`。

`bb_89568` 有 40 个原入边。取 `memory.valid1203` 入边时，hub `%RBP.25` PHI 选的是 `%RBP.13`，后者定义在待复制路径后面的 `bb_68277`。这个值在原 store 之前已经执行过，属于旧迭代；本次进入新 `bb_68277` 则会重新执行该块。

基线 `GuestMemoryEdges.cpp:181` 的 `selectedPHIsAvailable` 把这样的首块 PHI 历史输入统一拒绝；`:405` 又拒绝 stored-address index `%98` 定义在 corridor 内。单纯解除这些判断会把旧 `%98/%RBP.13` 错映射为 fresh clone 定义，不能这样修。

最小通用修复是为首块选中 PHI 的历史输入创建独立 entry snapshot。要求原定义支配选中的原前驱；证明地址时在越过该首 PHI 入边后切到历史执行视图；fresh LOAD/freeze/add 继续在克隆中执行；所有原未知路径和边界失败出口保留。stored value 若来自 corridor 内仍可保守拒绝，首版无需增加范围。snapshot 的块、PHI、branch 和新增 SSA 修复成本全部纳入既有预算。

后续消费链为：L4931 `memory.valid133` 将 `%1072` 写入另一状态槽并比较 `>50`；L5003 `memory.valid135` 计算 `%1110 = 90120 + (%1072 << 3)`；L5047 `memory.valid137` 执行实际表 LOAD `%1118` 并进入 CFF switch（默认 Exit source PC `0x10adc`）。因此可验证精度门槛是至少一个被实际 switch 使用的常量索引，优先候选 14→90232、34→90392、32→90376、0→90120。必须由真实 IR 检查器证明，不凭 state STORE 或复制数宣称成功。原预算若先拒绝 SSA 修复，需如实报告，不能抬高预算或假定表不可变。

## f-cff O0：每轮单边策略和指针 PHI 识别缺口

IR SHA `bee28f4b161bdd6bbdebfbd1210f0f182e93485aa05d83968b929259ef7ff7b6`。

三轮已经获得真实索引 70032/69760/69736，仍保留 `%417 = load i64, ptr %416`（L2626 `memory.valid7`）及 `%417>50` 分支（L2678 `memory.valid9`）。L2422 `bb_67382` 有 41 入边，包含很多已知状态写入；例如 L3569 `memory.valid21` 写 1，L3931 `memory.valid37` 写 48，L4095 `memory.valid47` 写 32。

两个限制需分开：

1. `GuestMemoryEdges.cpp:601` 首次成功即返回，`:646` 注释/实现限定每个原 LOAD 每次最多特化一个入边；因此更多轮有收益并不代表单轮算法已穷尽所有原入边。后续若做批处理，应 snapshot 原入边，逐边重新证明，保留 direct-before-hub 顺序并继续使用原预算，不能遍历新复制边形成无限展开。
2. 前几轮 SSA 修复已把不少 STORE 指针变为 `%state.edge.live524` 等 `phi ptr`。例如 memory.valid21 指针 PHI 来自 `%state.edge.live470/%state.edge.live468/%2425/%2440/%2459`，其中包含循环自引用；当前 `byteAddress` (`GuestMemoryEdges.cpp:63`) 只接受直接 GEP，因此明确不识别这些指针。不能剥掉 PHI 或全局假定 RBP 不变；需后续独立设计逐边对应的 pointer/index PHI 关系证明，未知输入仍退出。

本轮历史 entry snapshot 修复不顺带实现这个 pointer-PHI 扩展。普通 CFF 原有三轮真实索引门槛、原表 LOAD/default 和所有未知分支必须保持。

## 验证约束

新历史能力应覆盖一/多个有未知入边的 hub、Add/LOAD/freeze 历史输入和 fresh 值同时输出、外部 live-out、回入原块、地址不匹配、普通非 PHI 跨历史引用、poison flags、fresh LOAD/freeze 重新决定地址的拒绝，以及含 snapshot 的 6-block/256-instruction/累计 repair 边界。独立 C/完整 Guest 原生 oracle 判定语义；旧库不必出错，它应在新精度门槛上失败。

现有 `TestLLVMGuestMemoryHubs.cpp` 的 `oldHistory` 负例仍应保留：它的 STORE 使用 entry knownFrame，循环历史 frame 却来自独立 unknown 输入，两者没有地址相等证明，不因新增 snapshot 就升级为正例。
