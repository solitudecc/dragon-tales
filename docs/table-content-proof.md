# 跳转表内容证明与显式读取契约

2026-11-07，基线 `777712f`。本阶段修复 x86 和 AArch64 把初始 image 表项直接当成
运行时跳转目标的问题。表地址、索引上界和表项内容是三个独立条件；有合法守卫、能读到
文件字节，仍不足以发布完整目标集。

这是静态分析正确性修复。它可能使原先未经证明的表退回未解析，也能恢复实际 Guest
STORE 已完整初始化的表；不等于新增去平坦化、全程序堆初始化分析或通用去混淆能力。
本阶段规定范围内的验证和独立审计已完成；完整语义与兼容性标记仍为
`semantic_complete=false`、`compatibility_complete=false`。尤其默认静态 IR 在下述六个
switch 型 Tigress ELF 上没有完整返回对照，须连同精度代价阅读本报告。

## 反例与默认行为

四个永久机器码夹具在固定的 `0x1000..0x107f` 代码外放置 `0x1080` 表，先用真实
Guest STORE 改写第零项，再经 `index <= 1` 守卫读取表并跳转：

| 夹具 | 初始表内容 | 真正写入 | 初始目标 / 写后目标 |
|---|---|---|---|
| x64 绝对表 | 两个 64 位指针 | 第一指针改为 `0x1060` | `1040/1050` → `1060/1050` |
| x64 相对表 | 相对 `0x1080` 的有符号 32 位偏移 `-64/-48` | 第一偏移改为 `-32` | 同上 |
| A64 绝对表 | 两个 64 位指针 | `STR` 写第一指针为 `0x1060` | 同上 |
| A64 相对表 | 相对 `0x1028`、左移 2 位的有符号 16 位项 `6/10` | `STRH` 写第一项为 `14` | 同上 |

代码没有修改；修改的是外部 Guest 数据。旧代码在两种扫描策略下仍发布初始
`1040/1050`。新默认路径既不信任内嵌 image 字节，也不把普通 `readMemory` 读回的
初始字节当成证明。上述夹具只显式初始化了第一项，第二项没有独立内容事实，因此默认
安全结果是未解析，而不是只发布第一项、回退初始表或把 Unknown 当成零。

`SwitchDetector` 的形状/上界识别以及 `readTableTargets` 的小端、符号扩展和偏移解码
仍可单独使用。`readTableTargets` 只是解析调用者给出的字节，不证明机器 LOAD 会读到它们。

## 在真正 LOAD 之前查询内存

`dragon/analysis/FiniteValues.hpp` 新增 C++ 查询：

```cpp
FiniteMemoryQuery query{blockAddress, loadOpIndex, cellAddress, widthBits};
auto result = dragon::analysis::finiteMemoryValues(
    graph, entries, std::span{&query, 1}, limits);
```

`beforeOpIndex` 必须指向该块中真实的 IGNIL LOAD，其目的位宽必须等于查询位宽。
查询的是这条 LOAD 执行前某个固定 Guest 地址的内容；可以查询同一张表的其他项，
不要求该项恰好是本次运行的动态索引。结果仍是 `Unreachable / Finite / Unknown`，
只保存完整保守集合，不以预算为由截断。

内容来自所有输入路径上的 Guest STORE。部分字节覆盖按字节组合，未初始化字节仍未知；
LOAD 之后的写入不能倒过来改变已捕获的值。独立入口、汇合和循环按原有限分析求不动点，
当前图中已解码的直接 callee 入口始终有独立输入，即便显式 `entries` 没有列出它；不能
借用尾跳路径上的初始化 STORE 约束同一 callee 的任意 CALL 输入。可能别名的写、未证明的 CALL、unlifted 和块内局部控制流
保留屏障。已有可选调用效果证明的范围不因此扩大。

非法位点、非 LOAD、位宽不符等查询返回 Unknown；有效但不可达的查询保持 Unreachable。
预算失败使整批结果不完整并返回 Unknown。分析不读取初始 image 来补齐缺失的字节。

## 调用者可显式断言内容

调用者已有独立证据时，可以设置 `Disassembler::AnalysisHints::readKnownMemory`：

```cpp
struct Disassembler::MemoryReadSite {
  std::uint64_t blockAddress;
  std::size_t opIndex;
  std::uint64_t instructionAddress;
};
// bool(const MemoryReadSite&, uint64_t address, std::span<uint8_t> out)
```

返回 true 是可信断言：在这条精确 IGNIL LOAD 执行之前，对入口契约允许的**所有执行和
所有循环访问**，所请求字节都等于返回值，包括后续 CFG 扩展才发现的路径。它不是一次
动态观察、初始 image 快照、section 只读标志或“初始化函数大概执行过”的替代名称。
错误断言会使基于它的结果失去正确性保证；框架不自动验证调用者的证书。

回调每次请求一个表项，可能重复调用。它可以只证明部分项，但其他项仍须由 Guest STORE
事实证明；任一项不可证明、无效或超过完整集合限制，都不能发布一个删减后的目标集合。
断言只适用于给定读取位点和字节范围，不会初始化其他内存，也不会进入一般堆分析。
位点索引属于当前 IGNIL 图；不能直接把旧图的索引当成新图的内容证据。

原有 `readMemory(address, out)` 仍是普通 image reader。它和读写/执行权限都不隐含
`readKnownMemory`。两种回调分开设置，默认可信回调为空。

## 三种入口与生命周期

| 路由 | 提供契约的方法 | 入口与持有规则 |
|---|---|---|
| 原始代码分析 | `dis.disassemble(bytes, base, graph, hints)` / Python `analyze(..., hints=hints)` | hints 同步快照；Graph 不借用回调 |
| 加载后的 image | `loader::disassembleImage(image, config, hints)` / Python `image.disassemble(..., hints=hints)` | image 元数据入口与调用者入口合并；回调同步使用 |
| 可选有限分发 Pass | `FiniteDispatchPass{code, base, entries, hints}` / Python `register_finite_dispatch(..., hints=hints)` | 合并显式与 hints 入口；注册的 executor 持有快照 |

C 新增 `dragon_analysis_hints_set_known_memory_reader`、
`dragon_loader_disassemble_with_hints` 和
`dragon_pass_pipeline_register_finite_dispatch_with_hints`。可信回调接收
`(block, op_index, instruction_pc, address, out, size, user)`：返回 1 表示已填写精确内容；
0 或其他正数表示 Unknown；负数表示错误，原生分析/Pass 立即停止。回调错误不会伪装成普通
Unknown 后继续下一步骤。C 的函数指针和 userdata 仍由调用者保持有效；Pass 注册后须保持到
executor 被替换或 pipeline 销毁，`clear()` 只清步骤。

Python 的 `AnalysisHints(..., read_known_memory=callback)` 及同名可写属性接收
`(block, op_index, instruction_pc, address, size)`，返回恰好 `size` 字节或 None。
长度不符抛 ValueError，其他回调异常保留原异常对象。同步分析失败时销毁中间 Graph 后重抛；
Pass 失败时抛 `PipelineError`，原异常在 `__cause__` 中，后续步骤不执行，失败目标按既有规则丢弃。

Python 在注册 Pass 时创建独立入口数组、两种回调桥和错误列表。替换或关闭原 hints 不会使
注册的 C 函数指针悬空；成功 override/close 释放失效快照，错误引用在 run 的 finally 清理。
外部仍持有异常/traceback 时，它仍可保留回调对象。快照也不深复制调用者闭包捕获的可变数据。
显式关闭正在分析的 Python loader/disassembler 会抛 RuntimeError，调用结束后恢复可用；
hints 本身仍可以在回调中关闭或替换。C 客户端不得在回调中销毁正在使用的 loader/disassembler。

Python 的 `hints=None` 路径仍调用旧 C 符号；使用新增契约及带 hints 的路由须使用新 shim。

## 推断边、扩展与预算

分析产生的间接边通过 `addInferredBranch` 记录，与 decoder/调用者通过 `addBranch`
提供的显式边分开。重新分析、扩展或改变入口/内容契约时，旧推断边不是新证明。
新增 case、回边和独立入口都必须参与最终复验；它们可能使此前成立的表内容失效。
AArch64 同样不能只保留表中有效的那部分目标，或忽略新 case 中可能别名的写入。

对已有 Graph 调用一次分析重载时，撤回范围是**整张图的推断边**，重新解码/证明范围却
只有本次提供的 `[base, base + bytes.size())`。范围外的指令和显式边可以保留，但其旧
推断边不会自动恢复；它们保持未解析，直到获得新的相应分析。带 hints 的 loader 与
FiniteDispatch 同样受此规则约束。逐区调用不代表此前各区的证明会累计保留，也没有
自动重建全部旧区域推断的闭环。一次局部重析不是整张 image 的内容/目标完整性证明。

`Graph::merge` 先撤销两图的推断边。碰撞指令只有在架构、指令长度、机器字节和控制流类别
一致时才合并显式 branches/callees 与结构标记；不兼容碰撞保留原来的 first-wins 行为。
两个片段各自的证明不等于合并图的证明，合并后须用新的入口/内容契约重新分析。
兼容合并保留原有出边数量元数据，不用 `branches().size()` 覆盖可能包含 fallthrough
信息的数量；这也不是新增的完整性证书。

FiniteDispatch 的内存查询与普通有限分析共享每轮 `max_operations`；查询/结果缓存也受
`max_facts` 限制，并在每轮 CFG 改变时清空。缓存只在同一不可变分析轮次内复用。
Unknown 或预算耗尽不允许回退到另一套默认大预算分析。计数器包括
`memory_analysis_operations`、`memory_analysis_queries`、`memory_analysis_cache_hits`、
`memory_analysis_cached_facts_peak` 和 `max_round_analysis_operations`；它们是资源统计，
不是去混淆完成率。

x86 新 case 的机器指令解码另有内置 `1 << 20` 上限。若额度耗尽而仍有待处理路径，
整个暂存事务停止；部分 CFG 不能继续参与目标复验并发布推断边，尤其不能漏掉尚未解码的
别名 STORE 或回边。可信 resolver 的 `maxNewInstructions` 只能下调这一上限，不能提高它；
本阶段没有新增 FiniteDispatch 业务选项。低额度失败与同夹具足额正例用于验证此边界，
不等于实跑默认百万指令额度；本轮没有实际触及默认上限的覆盖。
该额度限制纳入扩展的指令数；`disassembleBlock` 仍先生成整个块的向量，因此它不是
逐条解码的硬 CPU 或内存上限，特别大的单块仍有预解码开销。

预算失败或复验失败不会发布新候选；它还可能撤销原 Graph 中旧的推断边，报告
`removed_edges` 和 `changed=true`。因此 Incomplete 不保证 Graph 字节不变，也不表示执行错误。
原先已解码的节点可以保留。显式边不是可撤销的推断证据。

## 兼容范围与验证进度

本阶段改变 `Instruction`、`AnalysisHints`、`FiniteDispatchPass` 等公开 C++ 类型布局。
升级 SDK 时必须使用匹配的头和库，重新编译 C++ 客户端；不能将新头直接配旧 C++ 库。
旧 C opaque 符号及 Guest ABI 保留。默认目标解析更保守，真实程序的 CFG、函数成员和 LLVM
覆盖可能下降，不能把这种下降写成全兼容通过。

已确认的旧实现反例：四个默认用例在冻结 `777712f` 上均可编译，4/4 失败、
8/120 断言失败；形状、`numCases=2`、真实 STORE 和写入宽度前提全部通过。
补强前 v6 主配置 CTest **684/684**、无 Z3 配置 **552/552** 通过；两配置的本阶段组件均为
**44 用例/3201 断言**。C/Python 路由、错误传播及生命周期回归包含在该轮 CTest 中。
随后原生执行发现了下面的函数归属与跨架构缓存问题，因此这些数字保留为补强前覆盖，
不能作为本阶段最终成功证明。补强后 v8 主配置 CTest **690/690**、无 Z3 配置 **558/558**
通过；两配置组件均为 **50 用例/3936 断言**。guard truth 报告 **27 个 CFG、20736 次
Unicorn 执行、0 个错误目标集**。这些是已完成的生产测试，最终原生/语料矩阵另计。

解码 cap 新增三个用例，含 i386/x64 各五个边界输入的 **10 次 Unicorn 完整 CPU 返回**，
核对全部 64 KiB 内存与代码不变。匹配 `pre-cap-v5` SDK 的 `daa7…` 旧 backend 对照中，
两个可运行用例的足额正例通过、低额负例失败，合计 **102/134 断言失败**，确实暴露部分
CFG 漏掉尚待解码的写/回边后仍参与证明的问题。这个小额度回归没有实跑默认百万 cap。
原有四表、ARM case 闭环和有限分析复验的旧失败证据分别保存在本阶段，不混成同一个旧库统计。

Graph+CPU oracle 的扩展集在三种内容契约下各执行 60 次，共 **180 次**；旧目标发布负控
另执行 **40 次**，合计 **220 次 CPU 完整返回**。负控精确抓到 **8 次 stale publication**。
这些检查覆盖 Graph 目标/内容时点及独立 Unicorn 执行，**不是 220 次原生 LLVM 完整函数对拍**，
未解析目标仍是显式分析边界。最终 Graph 验证复用同一 core `3159…` 与同一 backend 下
已验证的这 220 次记录，不把 V2–V4 诊断重放累计为新输入。

时序夹具的守卫对 unsigned `index >= 2` 会在 STORE 前返回。oracle V1 曾错误地要求这些
路径也具有写后的表内容；修订版本按真正执行路径选择预期值，完整 64 KiB 比较与用例均保留，
并补充脚本自检。原 V1 失败、修订证据和适配器基础设施尝试保留，不能悄悄归零或计作通过。
原生适配器的初始化补丁冲突尝试也保留，后续 v3 实跑进一步暴露了生产缺口。
使用 v6 生产库的默认契约有 **110 项匹配**，其中 74 次完整根返回、36 次明确边界；
`trusted-load` 的 60 项中只有 **52 项匹配、8 项真实差异**。这 8 项来自 A64 绝对/相对表的
index 0/1：Graph 已发布 `1050/1060`，但 RET 后 padding 启发式把 case 晋升为独立函数，
原函数成员跳过这些 case，LLVM 返回 Unsupported，PC/exit 与 CPU 不同。它们不能改记成
预期边界，也不能因 Graph 目标正确而忽略。原始 v3 失败保存在 `prod-gap-v6`。

ARM 函数归属补强采用 x86 已有的 region 原则，避免把已知函数 region 内的这些 case
仅凭 RET/padding 当成独立函数，同时保留真正显式入口、直接 callee 和完整 prologue 证据。
初版五个回归在旧 v6 上为 **3 用例失败、2 用例通过，15/461 断言失败**；其中两个 ARM32
控制还受到另一项既有跨架构缓存错误影响，不能把全部失败归为 region 改善证据。
v7 组件 **48/49 用例通过**，正常 ARM32 post-RET 控制仍有 2 个断言失败，原始输出保留。

进一步定位到 ARM32/ARM64 分类器同时用首次创建目标的 MC 表初始化静态缓存，混用的
opcode ID 会破坏后续另一架构的函数识别。v8 改为按架构分支分别懒初始化各自 static；
它修复分类器来源，不新增 ARM32 指令语义，也不把 A64 region 规则泛化成新的 ARM32
函数策略。第六个纯混合顺序回归在旧 v6 独立运行时为 **1 用例/282 断言、15 个失败**，
覆盖 A64/A32 双初始化顺序的 post-RET 与 prologue。初版五用例源码 `53df…`、扩展六用例
源码 `fec3…` 以及 v7 失败记录分别保留，不混用为同一组结果。该缓存修复本身不增加 ABI 变化。

分类器还用独立进程复验初始化顺序：旧实现的两个全新进程分别先建 ARM32/先建 A64，
都在第二种架构上出现分类失效；新实现两个全新进程各 3 条记录全部通过。合计保存
**4 个进程/12 条记录**，严格审计实际源/库身份。这是纯 Graph 分类验证，不声称执行了 ARM32 程序。

修后 native v4 的 V2 两策略 **340 次状态比较全部匹配**，此前真实 ARM 8 次差异已消失。
同轮 Static 默认组的 110 项中另有 4 次差异，经定位属于 stage oracle 混用了 V2 合同：
timing 与 path-no-store 夹具的 index 0 在 O0/O2 下跳向 `0x1040`，该地址虽已是 root 内块，
但对应间接 JMP 未发布目标。V2 dispatcher 可以继续已有目标，Static 按既定契约返回
Transfer（实现见 `lib/LLVMLifter.cpp` 的 JMP 降级）；不能要求两个 mode 在相同观察点都返回
RootReturn。此处生产代码未改，也不能挪用该解释消除此前真正的 ARM 函数归属差异。

修订的 native oracle `b774…` 按 mode/index 选择公开观察合同，并通过 **27 项纯合同自检**；
退出七字段、PC、SP 与完整内存仍逐项比较，不归一化 Guest 状态、不删除用例。
旧 `909…`/v4 Static 失败保留在 `oracle-v4/static-contract`，与 `prod-gap-v6` 的真实生产失败
分别归档。观察合同修订后两配置各重跑对应的 2 项纯 harness CTest，通过；生产字节没有变化，
没有把主 690/无 Z3 558 全套再跑一遍或重复累计。

v5 `native-final-audit` 已闭环 **1420 次正常状态匹配**，其中 **1104 次完整根返回、
316 次精确声明边界**；另有 **340 次负控**精确抓到指定的单个结果字段或单字节内存差异。
主配置 13 个正常任务共 1080 次，无 Z3 的 4 个正常任务共 340 次；负控另为 4 个任务/340 次。
正常比较累计 **116760 个建模字段、93061120 字节内存**，每次比较全部七个退出字段、PC、SP
及完整 64 KiB Guest 内存，Graph 报告的 unlifted 均为空。排除、跳过、Guest 状态归一化均为 0。

各 mode 按各自公开观察合同核验；单组默认配置的 V2 为 **74 根返回/36 边界**，
Static/product 各为 **70/40**。部分可信 `trusted-entry1` 的 product 组为 **56/4**。
`trusted-load` 组保持 **60/0**，包括已发布 A64 目标的完整返回，没有用 Static 观察规则
放宽此前真正的 ARM 差异。核心脚本 `3159…`、原生脚本 `b774…` 与最终原始记录分别绑定。

69 项语料的原 worker 已完成前 60 项；第 61 项 `sys_python3` 的新版 worker 被 OOM
终止并返回 -9，kernel 证据和原失败 `001` 全部保留，失败尝试没有计入通过数。
生产 v8 保持冻结，原 plan `02b149…`、原 worker/driver 与 Tigress 计划 `47c7…` 没有回写。

历史流式恢复版本先复审此前 60 份完成记录，用于剩余 9 项的初版仅将外层定义观察器的
`ir_path.read_text()` 改为逐行读取，随后仍出现下面的 `002/003` 失败。同一锚定正则、完整定义集合/唯一性、chunk 128、
unchanged `check_one.py`、verify 与库/输入/源码核对均保留；没有额外截断函数/模块或删除检查。
补充 descriptor 的 SHA 为
`ac5a050d9cdbbe84c1f7ad0dfb9805ee8ba5928669eb6654d990dfc6a780b3b9`，
锁定低内存 worker `3c066936…` 和恢复 fork `956be110…`，与原计划分开归档。
纯恢复框架自检为 35 个拒绝负控、6 个接受正控，未执行 Graph/LLVM，不能算语料新覆盖。

低内存重试 `002` 仍被 OOM 终止并返回 -9。`001/002` 都已完成 linear Graph 和
`0000..0027` 的 28 份 verify receipt，这 28 份 IR hash 逐项相同，不能重复算成 56 个新模块。
两次另有相同的 125752481 字节 orphan IR，SHA 均为 `402bc7b…abba`；这些部分证据没有使
第 61 项成为完整成功。第二次 kernel 记录由 opt PID 958851 触发 OOM、杀死父 worker
958547，当时 swap 只剩 120 KiB。逐行 observer 减少了额外 IR 文本副本，但没有解决总体峰值；
这两次是资源失败，不是观察到的生产语义差异，原记录均保留。
三个失败 attempt 的原件分别位于 `corpus-default/attempts/corpus2-sys_python3/001`、
`002`、`003`；完整归档为 `raw-resource/resource-oom-python61-{001,002,003}.tar.gz`，
对应 `resource-oom-61-evidence-{001,002,003}-files.json` 与 `-Mac-verification.json`。

重试前对 122 份 inactive `/tmp` 历史文件先作 VM gzip-1 与 Mac 双副本逐 SHA 归档，再
移除原文件：pre-cap/prearm SDK 72 份/343542937 字节，legacy/addressing/旧 tests 50 份/
552814008 字节，合计 896356945 字节。当前 `777712f` 基线与冻结生产未动。此举主要回收
tmpfs/swap 容量，不能把归档字节数称作释放了同量物理 RAM。恢复收据为
`inactive-tmp-SDK-storage-migration-receipt.json`（`f6d1a3…d80fb`）及
`inactive-tmp-older-roots-storage-migration-receipt.json`（`20be66…aef5`）。

重试 `003` 使用同一低内存 worker/fork、库 pin、chunk 128 和命令，未调整环境或系统配置。
`resource-retry003-preflight.json` 双端 SHA 为
`273675b7e93e5ddf30e33c2a7801f54c3a284aec8b43770fcd7cd29308ebcd82`；
它是资源与来源预检，不是完成记录。该次重试完成 linear 的 `0000..0076` 共 **77 个模块**，
verify 全部 rc0，原先相同 hash 的 125752481 字节大模块也已通过；开始 recursive、尚未返回
新 Graph 时再次 OOM/-9。这仍不是第 61 项完整成功，不能把 77 个 linear 模块当成双策略验收。

第三次 kernel 在 17:15:15 记录 Python PID 961092 自己触发 OOM，当时没有 opt 并存；
virtual memory 为 5978396 kB、anon RSS 为 2831224 kB、SwapFree 为 0。这个峰值不同于
前两次 verify 阶段：`check_one.py` 第二策略的 `g = img.disassemble(...)` 先计算 RHS，
旧 `g`、observer 的 `active_graph` 与最后一个 `bb` view 仍保留前一 Graph，导致两图共存。
失败 `003` 的原件保留，归档收据为 `39b026…e2225`、成员 manifest 为 `a38979cd…ca64`。

独立生命周期 worker
`e14d0af20f408705171e1257ca89d970e1fe06929f5e66c294c59cab9d651941` 已完成源级审阅：
只有前一策略全部 chunk、定义、accepted 与 verify 检查完成，且所有 LLVM lifter 的 native
指针均为 None，才在第二策略开始前显式关闭旧 Graph。失效 view 仍受 ReferenceError 契约保护。
原 `check_one.py`、生产库、逐行 worker `3c066936…` 与 descriptor `ac5a050d…` 均保持原样；
新恢复路径单独固化，不回写旧证据。

小 cjson 真实 A/B 已通过：原逐行 worker 与生命周期 worker 使用同一新版库，两策略共
**4 个 Graph/4 个 raw 模块**，每策略 111 个有效函数；解压后的 canonical/delta 逐字节、
IR SHA、definition、accepted、verify 和 warnings 全部一致。报告 SHA 为
`150f3a8d7254da69f3fb6b2d4982fb0230d2d71ec236dea446ef2afc658235da`。
另有 21 项生命周期负控、6 项 A/B 负控及 driver schema 自检。这些证明恢复框架的观测
一致性与失败检查，不计入 69 项语料结果，也不是 CPU 新覆盖。

新恢复 driver `f6184f6f…`、生命周期 worker `e14d0af…` 由独立 descriptor
`65ab5f55009e7245d7d57109c84b3fa7f44f623563e3f0b632db2448d18f31b5` 固定，
freeze receipt 为 `677573d…dfb4c`。`004` 的 PID 963294 单次 live observation
`retry004-lifecycle-live-process-observation.json`（SHA
`b68c9f1c6007d984a4562fe0686cc165651b726e05a055bddf34a57cfc635a9d`）中
21 项实际身份核验通过；该观察本身不是完成记录或额外 Graph/LLVM 执行。
随后 `004` 实际完成 linear 77 个模块和 recursive 77 个模块，全部 verify，第 61 项成为完整
双策略输入。此前三次 partial 与失败记录继续保留，不额外加进成功模块数。

基线 69 项仍使用原 worker，范围是 Graph inventory，没有旧版 LLVM 重跑。新版最终来源
由 `65ab…` 固定为同阶段前 60 项原 worker、后 9 项生命周期 worker `e14d…`，不能归入
失败尝试的 `3c066936…`，也不能写成单一 worker 跑完 69 项或旧阶段 IR 缓存复用。
69/69 主 driver 已完成，产生 9 份生命周期清理 receipt；独立 forward 与 precision 全流审计
均已通过。forward 源 `76dd…` 的最终报告 SHA 为
`9e02d79f6f7ad76df9c094a97d41992d39d293c62b3bdd8c3c101bb211d5cec8`，
`precision-final.json` SHA 为
`317e329d699ed4773e950c0cab6abbcb28c8926627c83654ec9a173708be6f61`。
它们审计记录、身份和保留的完整流，不另执行 Graph/LLVM/CPU，也不证明全程序语义等价。

| 项目 | 结果与范围 |
|---|---|
| 输入与策略 | 69 输入，各版本 138 条策略记录；旧、新 Graph 均为本阶段 fresh |
| 旧 `777712f` 基线 | 205406 个有效函数候选 checkpoint；只做 Graph inventory，没有旧 LLVM 重跑 |
| 新 v8 | 205018 个有效函数候选 checkpoint，所有这些候选的 fresh V2 raw 验证覆盖完整 |
| 新模块 | 1671 个 raw 模块；未跑全量 optimized campaign，旧阶段 raw 复用为 0 |
| 候选变化 | 有效候选记录减少 388、增加 0，不能直接称为丢失了 388 个真实函数 |
| 结构变化 | canonical 109 条、hardware 108 条、unlifted 109 条策略记录变化；warnings 变化为 0 |
| unlifted 累计记录 | 旧 233134、新 231798；下降不代表支持了更多指令 |

checkpoint 按两种策略累计，并非去重的真实源码函数数。检查所有新 Graph 的有效候选，
也不等于证明其已覆盖所有真实函数。summary 还记录 fresh 旧版与 prior 基线 **138/138 条
策略记录完全相同、0 条变化**；不能套用更早 `206649/1686` 阶段的数量，也不能把该一致性
推广为新旧全兼容。独立精度审计给出的代价如下；除专门折叠 strategy 的行外，数量均按
`(input, PC, strategy)` 及其目标成员累计，编译变体和两种策略会重复，**不是全局唯一地址、
独立函数或独立程序数量**：

| 观测 | 数量与含义 |
|---|---|
| 保留的间接跳转变为 unresolved | 2957 个 site/strategy 观测；折叠 strategy 后为 1524 个 `(input,PC)`，涉及 54 个输入 |
| 上述保留间接指令的目标成员撤回 | 42063，按策略累计 |
| 不再递归可达而消失的 decoded-address 观测 | 568329；不是删除输入机器字节 |
| 随源指令消失的 direct branch / callee 目标 | 96535 / 31972；须与仍解码源处的直接边损失分开 |
| 保留地址的机器字节变化 | 0 |
| 保留地址的 direct branch / callee 目标损失 | 0 / 0 |

其余块、IGNIL 和函数成员分类保存在未归一化的 canonical/delta 与 `precision-final.json`。
目标及覆盖下降是本阶段真实精度代价，unlifted 从 233134 减为 231798 不能解释为新增支持。
审计通过说明记录完整、变化分类和身份核对成立，**不表示新旧 Graph 兼容通过**。

另有 `003` 与 `004` 的只读辅助对照：77 个 linear 模块的 IR hash、定义、accepted、verify
一致，完整 254961064 字节 canonical 流精确相同，报告 SHA 为
`9b358a831be116b972e5720bba928ab502c5d8d0435b4cecad17f89667ca7750`。
它没有增加成功输入、模块或 CPU 覆盖，失败 `003` 仍不完整。

### 存储迁移与恢复

为归档保留空间，owned `build/build-noz3` 的 402 份生成 ET_REL `.o` 缓存分六包双端逐
member 核验后移除原缓存：105 份 unit、214 份 Catch2、83 份 lib/shim 对象，888643904 字节
压缩为 209049453 字节，净回收 679594451 字节（648.1 MiB）。`.so/.a`、test executable、
source/header、CMake/Ninja 均未动；16 份受保护产物的 size/SHA/mtime_ns 和 118 份生产
source hash 全同，原 dirty checkout 未接触。总收据是
`build-object-cache/storage-migration-final-receipt.json`（`f12662…f476`）。
按每批 `restore_command` 恢复原路径、mode 和 mtime；**仅在原源码与工具链 pin 一致时
复用对象，否则让 Ninja 重编**，避免旧对象与新头混用。

统一 `storage-migration-artifact-index.json`（`3dcdca03…7424`）还链接历史 19 份 backend、
8076 份 IR、122 份旧 tmp 和六批对象的恢复链。输入 78 份 payload 的双端归档及 corpus
四批 69 输入/276 流也已保存；这些存储操作增加的语义测试计数为 0，旧 manifest 保持不变。

### Tigress 默认回放结果与能力缺口

六组测量及独立 aggregate 审计均已完成：**C 288 次匹配，LLVM 0 complete / 288 incomplete**。
**默认静态 IR 对这六个 switch 型 Tigress ELF 当前没有完整返回对照。**
这不仅是 goto/indirect 难档未完成；本轮没有注入可信表、初始化堆快照或新内存前提。
证明内容不足时保守退出，不能把缺少完整返回包装成去混淆成功。

| 组 | C 匹配 | LLVM complete / incomplete | rc | executed-undecoded 观测 |
|---|---:|---:|---:|---:|
| mba-s0 | 48 | 0 / 48 | 1 | 0 |
| mba-s1 | 48 | 0 / 48 | 1 | 207 |
| opaque-s0 | 48 | 0 / 48 | 1 | 0 |
| opaque-s1 | 48 | 0 / 48 | 1 | 135 |
| combo-s0 | 48 | 0 / 48 | 1 | 0 |
| combo-s1 | 48 | 0 / 48 | 1 | 6515 |

六组原 rc1、CFG wrong 0 和 unlifted 0 均保留。executed-undecoded 合计 **6857 个 analysis
地址观测**，按 group 累计，输入或策略不同可以重复，不是全局唯一地址或语义错误次数。
mba-s0 停在未发布的间接边，例如 O0 `0x10167`、O2 `0x1011b`；即使实际 next PC 已知，
Static FunctionStart 仍按既定合同退出 Transfer（reason 2）/Branch（kind 1）。
`complete=0` 意味着没有完整 LLVM 返回对照，“LLVM 差异为 0”不能包装成语义成功或去混淆成功。
历史 `223b…` 在相同冻结输入上测得的 288 complete 只作历史测量比较，本轮没有 fresh 旧版
LLVM 重跑。覆盖下降是真实能力代价，不能用 CFG wrong 0、没有完整对照时的 LLVM 差异 0，
或生成 IR 变小来宣称语义成功或去混淆增益。

独立 aggregate 源 `4c452…` 已实际执行并通过，最终报告
`tigress-default/final-matrix-audit-v1.json` SHA 为
`fa7e65efa13fcd1b556cac49f065d8fe7c29b4ce62a796aded0a74cfd60f20cb`。
六份 tar 逐 member、原始记录/退出、实际 VM 来源与双端归档均已核验；这是 metadata
审计完成，不改变 `semantic_complete=false`、`compatibility_complete=false`，不把 rc1
或 incomplete 改写成语义成功。

下一能力缺口是**可证明的初始化/只读表内存模型**：需要内容来源、入口初始化前提及
写入/别名保证，不能直接把 section 只读标志改名为证明。本阶段未实现通用 heap/init 分析或去平坦化。

回归分开验证纯表字节解析、索引边界证明、内容证据与真正 CFG 目标发布。原 guard 测试
显式提供独立的可信表前提，默认无契约另设负例，避免全部 Unknown 使边界测试失去敏感性。
真实 CPU 仍须独立核对 STORE 后的表、代码未修改、间接边和完整返回；IR verify 不能替代它。

一般堆/全局初始化、通用跨调用指针别名建模、内存相关谓词消除、去平坦化和二进制改写仍未实现。
此前 TLS 相关目标撤回不是本阶段自动解决的能力声明。历史报告和失败证据保持冻结；
本阶段产物目录为 `~/vmc-runs/table-content-20261107/`，Mac 对应
`validation-artifacts/table-content-20261107/`。
