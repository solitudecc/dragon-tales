# Guarded entry view 的独立原生验证

基线 `7de235d`，实现里程碑 `aba656a`。本记录区分 API/语义验证与真实去混淆收益，不能把这些计数互相替代。

## 主测试套件

主构建与无 Z3 构建各通过核心 7 cases / 226 assertions、C 接口 2 cases /
109 assertions，以及 Python 输入/生命周期/流水线/重入检查。完整 CTest 分别为
**1045/1045、910/910**。最终 C++/C 测试对旧头的编译错误均明确指向缺失的新 API；
此前一次 Catch2 include 与一次 Execution.h include 命令错误也分别保存，不能计作旧代码回归失败。
新源码首次构建的 LLVM 枚举名和 C 测试 INFO 宏括号错误已修正；所有失败日志保留。

## 验证域与输入

`guarded_entry_native.cpp` 先用真实 x86-64 lifter 建立 StaticAnalysis + FunctionStart 的原始入口 prologue、PC 检查和 Entry 默认退出，再将成功入边后的 machine body 替换为独立建模的 LLVM CFG。新 API 仍须识别实际初始寄存器 LOAD → alloca → PC switch，不能只信块名。该矩阵验证新的 LLVM 变换，不是新增机器指令 lift 验证。

十种具名夹具为 `plain`、`overwrite`、`partial`、`call`、`loop`、`reg_alias`、`descriptor_alias`、`exit_alias`、`recursive`、`base_reload`。它们覆盖入口匹配/回退、执行后表内容变化、部分覆盖、未知调用、循环再次读取、寄存器/descriptor.size/Exit 与 Guest 重叠，以及普通递归调用和 Guest 基址变化。

冻结输入共 **1120** 个：10 模式 × 4 个实际别名布局 × 7 个输入变体 × 4 个边值。输入变体包括全部匹配、寄存器不匹配、表内容不匹配、size=0、表尾差一字节、恰好覆盖以及错误入口 PC；边值为 0、1、2^63 和 UINT64_MAX。循环上界为入口值低两位，覆盖 0–3 次。事实不匹配是合法输入，必须调用原逻辑，不能列为排除。

各对象放入同一个真实共享 arena，参考模型按顺序执行字节读写并使用该次 arena 的实际 host 地址构造 pointer 字段。每次执行重置同一 arena，完整比较 **139272 字节**：8 字节调用次数加整个 139264 字节 arena。512 字节寄存器、至少 64 KiB Guest、七个 Exit 字段、State/Memory descriptor、基址变换后的额外映射和所有 padding 均在比较范围内。没有指针归一化，也没有只比较返回值。

所有完整帧边比较边写 gzip，内存中不积攒全矩阵，不写未压缩 arena 文件。一条正常/预期流原始约 156 MB，实际压缩约 1.6 MB；旧成功组约 7.4 MiB，新 main 组约 15 MiB。

## 新旧同源结果

| 执行组 | 正常完整状态比较 | 差异 | 故障完整快照 | 指定错误检出 | 保护页正常探针 |
|---|---:|---:|---:|---:|---:|
| 旧 ordinary，O0/O2 | 2240 | 0 | 1120 | 560/560 | 6/6 |
| 新 main：ordinary/raw guarded/optimized guarded，各 O0/O2 | 6720 | 0 | 1120 | 560/560 | 18/18 |
| 新 no-Z3：相同三路，各 O0/O2 | 6720 | 0 | 1120 | 560/560 | 18/18 |

三组是同一冻结输入的不同产物/编译配置，不是新增不同程序覆盖。每组另有 **15683** 个完整快照字段/长度破坏控制；每组故意跨保护页读取均以 SIGSEGV（返回码 -11）被检出，已禁用 core dump。排除、跳过、归一化均为零。

五个原生错误 IR 控制分别把实际表 LOAD 错换为入口常量、删除真实 STORE、删除未知调用、绕过 bounds 和绕过 Entry PC。所有 1120 个故障执行仍完整比较独立故障模型；其中 560 个输入必须与正确模型不同，全部被检测。其余输入未跳过，不能把它们算成应当失败而未发现。

保护页探针独立分子进程执行：实际映射在表地址之前结束，检查正确 PC + 短 size、错误 PC + 短 size，以及空 base + 零 size 的原 IR 已定义路径。每个正常探针保存 880 字节完整观测，包括寄存器、可访问 Guest、Exit 和 descriptor；故意越页控制保留进程状态和日志，不伪造返回快照。

旧库不提供 `createGuardedEntryView` 等新 API，旧头/旧库不能直接编译或调用这些入口。这是新增能力不可用，**不是旧 runtime 的错误语义**。旧 producer 关闭新 API 宏，实际执行同源 ordinary 基线；新 producer 开启宏。main 与 no-Z3 使用完全相同的 generator 二进制，仅切换动态库，no-Z3 的 `ldd` 绑定已保存。

新 main/no-Z3 均核验 10 份 ordinary IR 加 5 份故障 IR 与旧版本逐字相同。新 view 的生成不修改 source IR；生成后实际修改/清空原请求事实并销毁 source lifter，再 verify、优化、导出，验证副本和 manifest 的独立生命周期。

## 精度与结构审计

冻结配方是现有 PassPipeline 的 `mem2reg → instcombine → guest-memory → gvn → dispatch-specialize → control-flow → dce`，最多两轮、64 次 pass；没有额外入口事实重播，也没有为此提高分析预算。

结构审计核对全部 10 份 manifest、独立符号、普通 fallback、guard 只从外部入口到达、循环回原 machine block，以及递归仍调用 ordinary root。新 guarded 定义排在模块首位，其他原函数保留。

最初的精度报告还记录了 `!oracle.target` 标记数量，但**标记消失本身不足以证明 LOAD 被正确化简**。原报告保留，随后追加了不依赖 metadata 的 SSA 值审计：plain 的实际 Guest+256 LOAD 与 14 比较，真边唯一进入成功块；成功块写入 RAX 的表达式明确为 `入口 RCX XOR 14`。PC、RBX=256、寄存器/Guest 非空及 size 检查支配这条路径；false 边仍调用 ordinary root，入口事实读取和检查保留。main 与 no-Z3 均通过。

SSA 审计另有五个错误控制：只删除 metadata、把结果改为 15、把被检查值改为 15、丢弃 fallback、改坏 bounds，全部被拒绝。这些是静态审计控制，不计作新增原生执行。

因此，本矩阵证明一个经过真实入口检查的表读取可以在成功路径化简，同时其他输入和执行中写入保持完整行为。它不证明文件表永远不可变，也不等于消除了所有真实 dispatcher。其余模式的 metadata 数量只作观察，不能代替逐值收益证明。

## 冻结来源与证据路径

- Producer v2：`fa1bbc7c3574ced7f752c5bea8d38cfdfb0a2827902cde0cd83475395aaaeebe`。
- Runner/model v2：`0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c`。
- Main backend：`32f2bd3dd929b4e678c6cbb61c3a63023623687371c2fa9f60af387cc0fc325c`。
- No-Z3 backend：`4a12438bb60d1f00c289c4332bd71c14b6ec1b3d482a03f6c13ec83f28f375eb`。

产物位于 `~/vmc-runs/guarded-entry-20261111/`：`oracle-old`、`oracle-old-results-v2`、`oracle-new-v1-ir`、`oracle-new-v1-results`、`oracle-noz3-ir`、`oracle-noz3-results`，以及邻接的独立审计 JSON。源和 runtime SHA 在各实际运行前后核对一致。

完整 metadata 包 `metadata-final-v1.tar.gz` 已在 VM 与 Mac 逐 payload 核对文件集合、
长度及 SHA：596 个 payload、49,503,719 字节，压缩包 12,890,048 字节，SHA256
`90c5cc092a91407b25cab0c51e379397bd5bed095046f776cbea2b3f09681842`。
其中保存实现 `aba656a` 的 18 个文件、补丁、真实构建参数、编译源码匹配记录和全部
native 成功/失败/审计材料。四份运行库另以 `.so.gz` 无损归档；压缩及解压后摘要均
在双端核对，见本机 `runtime/manifest.json`。Mac 根目录为
`/Users/c/Desktop/deob/validation-artifacts/guarded-entry-20261111/`。

真实四 ELF 的两组完整对照、三份证据包和**未通过的精度门槛**见
[真实验证报告](guarded-entry-real-validation-20261111.md)。两组各 C16/LLVM16 和
144 个指定错误控制通过，但实际 CFF selector 内容常量化为零。SP-only 独立观察
16 次确认 guard 成功命中，仍不构成去平坦化收益。后续入口路径专门化属于另一个
实验，不改写本阶段结论或原始配方。

空间整理只退役了更早 `llvm-control-flow-20261109/{baseline,final}/{main,noz3}`
的四份展开 backend。原字节分别保存为邻接的 `libdragon.so.retired-20261111.gz`
及本机 `retired-runtime/`；四个原始/压缩 SHA、逐文件双端核验和恢复位置均在 receipt
中，不删除旧失败记录。当前阶段 runtime 与这四份历史库不是同一批证据。

仓库可复查工具：

- `scripts/validation/guarded_entry_native.cpp`：冻结普通/guarded/故障 IR producer。
- `scripts/validation/guarded_entry_diff.py`：完整 arena 模型、原生编译/执行、保护页及错误控制。
- `scripts/validation/guarded_entry_saved_audit.py`：重读全部完整流、记录顺序、模型、哈希、文件集合及保护页结果。
- `scripts/validation/guarded_entry_precision_audit.py`：旧新源 IR、manifest、入口/回退/循环/递归结构。
- `scripts/validation/guarded_entry_value_audit.py`：plain 结果 STORE 的实际 SSA 关系及五种审计错误控制。

保存流审计已分别复读 old 的 12 条执行流 + expected，以及 main/no-Z3 各 16 条执行流 + expected，连同完整文件集合与 artifact SHA 全部通过。审计重读不增加原生执行数。

## 失败历史与覆盖边界

1. Producer v1 的两个 `at(b,regs,0)` 存在 C++ 重载歧义，首次编译失败。v1 源和 `oracle-old/compile-v1.log` 保留；v2 仅将这两个零改为 `std::uint64_t{0}`，没有改变夹具、输入或预期。
2. Runner v1 误将普通入口的保护页子进程也用于只导出 `control_*` 的故障库，因缺少普通符号而失败。原 `oracle-old-results` 保留：1120 次正常 O0 比较零差异，但故障执行为 0，整组不完整，不能列为完成。v2 仅移除故障库中错误的普通符号调用，保留所有正常保护页探针及最终故意 bad-read；完整结果使用独立新目录重跑。

本次 native 只覆盖 **x86-64 入口形状/寄存器布局的 LLVM IR，在 AArch64 host 上编译执行**。没有执行 x86-32、AArch64 guest 的新原生矩阵；多架构结构/unit 结果应由 root 另行列明。未新增单指令、Unicorn、IGNIL、完整 69 输入 corpus 或二进制输出验证。纯优化统计、IR verify 和 source 归档不能替代本节完整语义对拍，也不能把真实部分尚未恢复的控制流写成已去除。
