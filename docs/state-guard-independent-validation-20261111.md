# 状态守卫入边特化：独立验证

本记录验证 opt-in `runDispatcherSpecialization` 的入边复制与分支折叠。
它不引入默认优化、表内容不变、ABI 或跨调用状态假设。生产实现和单元测试
由其他 agent 负责；这里的 C++ 输入 IR producer、Python 字节模型和故障 IR
对照不调用生产证明器或单元测试 fixture builder。

## 校验域

`scripts/validation/state_guard_native.cpp` 生成 13 个独立模块。
`state_guard_diff.py` 用同源 IR 的 before/after，分别在宿主 O0/O2 编译并执行。
比较返回值、调用次数、全部 512 字节寄存器、64 KiB Guest 和七个 Exit 字段。
寄存器、Guest、descriptor 与 Exit 使用不同的宿主对象；不添加 noalias。
内存访问 alignment=1，atomic 输入的实际宿主存储满足所需对齐。

每个守卫有一个已知状态 50 和三条未知 incoming。输入覆盖四个 route，
logical size `0x507/0x508`，状态 `0/1/49/50/51/0x7f/0x80/0xff/0x80000000/2^63/2^64-1`。
循环另取 0/2 次回入，回入状态来自未知参数。共 1232 个输入：

| 模式 | 新实现要求 | 同时检查 |
| --- | --- | --- |
| basic | 复制并折叠一个已知入边 | 三未知入边、success/failure 的常量与区域值 PHI |
| size_load | 同上 | 守卫内普通 descriptor size LOAD、失败边、外部 PHI |
| outer_guard | 同上 | 外层路径条件仍先执行，拒绝路径状态完整 |
| loop | 同上 | 初次已知入口与未知回入的 SSA/live-out |
| narrow8 / narrow32 | 同上 | 按 IR 位宽截断状态后的无符号比较 |
| signed64 | 同上 | 符号位与全 1 的有符号比较 |
| ordinary_load | 同上 | 普通 Guest LOAD 随守卫副本原样执行 |
| unknown_call / store / atomic / volatile / fence | 零复制、零改写 | 调用、可写效果和同步边界不被绕过 |

`--require-gains` 检查实际 CFG：已知 predecessor 改指向新块，新块无条件跳到
原来的正确 successor；原守卫仍有条件分支，三条未知入边仍指向它。
精度八项每项恰好增加一个块；五项拒绝项不得改变。只验证 IR 或只报告
`changed=true` 不能通过该门槛。

三个故障 IR 分别把全部未知状态当作已知、把业务/标志 live-out 改为零、跳过
logical memory failure 检查。预先按输入定义应失败集合，不从实际结果反推。
实际分配的 Guest 始终为 64 KiB，因此故障对照不会依赖宿主越界或 UB。
不执行 undef/poison 并给它选定固定值；相关情况由结构测试覆盖。

## 冻结旧库基线

基线 `13f0563`，冻结 backend
`aeb13989cb31557effcde48872f3f6b6c5b0938fc737f11457bb3ba925130937`，
shim `a623a1490d57b9df708194ef9b6a59eec47a5a9c1dc03c72f65e3e98ed4eae6c`。
路径 `~/vmc-runs/guest-memory-address-proofs-20261111/final-runtime/`。

`baseline-native-attempt002` 的 4928 次完整状态比较全部相同；13 模式均零复制，
未知路径保留。528 个故障 IR 观察中，312 个预先指定差异全部检出，其他观察
保持相同；13555 个单字段/长度比较器控制全部检出。没有排除、跳过或归一化。
原生 expected/before/after/control 快照均 gzip 保存。

首次 `baseline-native-attempt001` 在载入生成 `.so` 时缺少 `__atomic_load`，
修正测试链接增加 `-latomic` 后重跑；失败 stdout/stderr 保留，不计作语义比较。
候选 runtime v1（backend `64ae144cced6ade67a4426110db05ec59b1ec8f86b93cf32fcfc1a540afaeaab`）
在 pass 内崩溃，尚未执行 native。独立 fixture 暴露了外部 SSA 值
`entry.size` 经 `MapValue` 后成为空 PHI incoming 的问题；原 IR、gdb 与失败
日志保留在 `candidate-native-attempt001/`。不得把该候选算作成功。

## 真实冻结样本

`guest_memory_checked_edges_real.py` 新增可选测量开关，默认旧用法不变。
测量在 `dispatch-specialize` 前后插入只读快照回调，不修改分析输入或 pass 顺序。
`--baseline-state-guards` 必须零入边复制；`--require-state-guard-gain` 要求
f-cff O2 真正的 `0x100b0` 守卫出现已知 50 入边复制，同时原未知分支、真实 CFF
target switch、程序自身 switch 均保留。`regs+128` RIP router 不计收益。
收益门槛沿实际 `br` 条件找到 `icmp ugt i64 %RCX.*, 50`，再仅从这个状态 PHI
提取该 predecessor 的 incoming 50，并核对副本跳向原 false successor。
其他寄存器 PHI 上恰好出现 50 只作诊断，不能通过门槛。
`state_guard_measure_selftest.py` 的一个正对照和四个指定假收益对照均通过：
无关 PHI 的 50、错误目标、删除原条件、重定向全部未知入边都不能通过。

四个原 ELF 与 transformed.c 来自
`~/vmc-runs/crosscall-cff-combo-latest-20261111/`，固定种子 4242。
真实 Guest `megaInit` 执行，不注入堆快照，不把只读节或表字节当作运行时常量。
每次重放仍使用原有限输入 `0/1/0x80000000/0xffffffff`，比较 76 个模型寄存器
字段、全部 1 MiB Guest、调用记录和七个 Exit 字段。Graph、输入和实际 mapped
库的 hash 在运行前后固定；全部寄存器/内存/Exit 字段故障控制继续启用。

旧库原 pipeline 和额外 post-GVN GuestSSA pipeline 各为 C16/LLVM16 完整根匹配，
所有 Graph 不变，四个真守卫均零入边复制。原 pipeline 2 个模块、post-GVN
4 个模块的 GuestSSA 仍明确报告分析 incomplete；执行完整不代表分析完备。
这也不表示真实 CFF 或 table LOAD/default 已被去除。

## 最终结果

修复后 v2 冻结路径 `~/vmc-runs/state-guard-root-20261111/candidate-runtime-v2/`：

- backend `4b20733fb35f70101f549c7e8ca6aa1249d500a6055d408d519d17c4fd675d03`
- shim `81e02399abf9bdbb5dee66da782ad940cbadb1568557dac3159a18e399226753`

`final-native/` 使用与旧 `baseline-native-attempt002/` 相同的 producer/model
字节与 source hash。4928/4928 完整状态匹配；八个正模式各恰好复制一块并折叠
已知分支，五个屏障模式均零改写，13 模式都保留三条未知入边并 complete。
故障 IR 仍为 528 观察、312/312 指定差异检出。旧库使用相同 `--require-gains`
门槛明确失败，保留在 `baseline-gain-gate-negative/`；该次没有执行原生比较，
不与旧语义基线混算。

直接受映射修补影响的旧 checked-edge 矩阵在 `final-checked-compat/` 重跑：
2176/2176 完整状态匹配，192 个故障 IR 观察中的 88/88 指定差异检出，5987
比较器控制通过。这里重跑的是已有输入，不能算作新增样本覆盖。

另加 `checked_edge_outside_phi_native.cpp` 与 `checked_edge_outside_phi_diff.py`
小子集，复用原 checked-edge producer 和未改动的字节模型/比较器。success
出口 PHI 取 region 外的 Argument 或 entry LOAD，并实际异或进返回值、寄存器
和 Exit next_pc。每模式 16 个已知/未知与成功/失败边界输入，O0/O2 × before/after
共 128/128 完整状态匹配；64 个故障观察中，32/32 预定错误外部值全部检出，
其余失败路径保持相同。全部快照 gzip。旧库在第一个 Argument 模式实际
`verifyModule` 报 `Operand is null`，生成器 rc3，原始 stderr 和 IR 在
`baseline-outside-phi-failure/`；未把这次失败算作 native 比较。

新库原 pipeline 和额外 post-GVN pipeline 各重新执行 C16/LLVM16 完整根匹配。
两者均只有 f-cff O2 的 `%memory.valid13` 已知 50 入边改向
`bb_65712.state.edge`，再无条件进入原 `bb_65712.bb_65718_crit_edge`。
其余三个冻结守卫零入边复制，四样本的实际 CFF switch 和原程序 switch 各自
仍为 1→1；table LOAD/default 保留，不宣称完整去平坦化。
所有输入、transformed.c、Graph 与实际 mapped 库 hash 检查继续通过。
每次真实重放的寄存器/内存控制各 16 个、七 Exit 控制 112 个均检出。

真实分析的预算状态必须与根执行成功分开。旧/新、原/post-GVN 四组均相同：
`dispatch-specialize` 在 f-cff O0、f-combo O0、f-combo O2 三个整模块
`status=INCOMPLETE`，各 `budget_exhausted=1`、`conditions_visited=257`；
f-cff O2 模块 SUCCESS，budget=0，conditions=155，branches_rewritten 从旧 0
变成新 1。原 GuestSSA 2 个模块、post-GVN GuestSSA 4 个模块 incomplete
亦保持显式记录。因此这里只证明该已知入边特化正确，不代表其他入边或整模块分析完备。
这里的 old 0→new 1 是完整真实 pipeline 中 `dispatch-specialize` 所处阶段的
整模块计数；实现侧单独冻结 IR 探针的 old 2→new 3 属于不同阶段与预算配置，
不能混成同一组数字。手动高阈值 LLVM jump-threading 探针也不计作框架收益。

旧真实记录最初用宽松的“所有 PHI 常量”诊断，后来 gate 收紧为真实条件所引用
的 RCX PHI。`state_guard_review_saved_real.py` 用新门槛对已保存 before/after IR
只读复审，旧原/post 两份结果仍零复制；新原/post 两份记录也通过。
`*-budget-review.json` 保存严格条件、全部 pass counter、IR hash 与原执行 summary
hash，明确 `executions=0`，不把这些复审算作再执行。原始 summary 未被覆盖。

本轮没有新增地址/别名语义，既有 1760 address/alias 独立矩阵没有重复；相关单元、
UBSan 与完整 CMake/CTest 由主 agent 集中运行并另报。
本轮不新增单指令/IGNIL campaign 或 69 输入语料库完整语义证明。

VM 证据目录：`~/vmc-runs/state-guard-reference-20261111/`。
