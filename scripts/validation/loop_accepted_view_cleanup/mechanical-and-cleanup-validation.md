# 选定视图清理验证（2026-11-15）

本次只复用原六步 LLVM 优化，作用于已经闭合的独立 GuardedEntry B 视图。原普通 root/caller、其它函数、全局和 metadata 的严格比较全部通过。清理程序没有执行 Guest；清理后原生语义校验由独立 native 记录负责，本报告不重计核心此前的64次名义执行。

源码 `loop_accepted_view_cleanup.cpp` 始终为 23725B / SHA256 `0ffa1459a03ad2526b02dd1538326a6f7974895dde469d8c47cb9b061bc947b2`。没有改变产品核心、默认 pipeline、solver/proof/DFA 预算或为实际输入放宽 intrinsic 门。

## 机械自测及独立构建

人工 toy 不读取生产 B，也不伪造 positive closed REPORT。它在一个函数内恰执行一次 SCCP、DFAJumpThreading、JumpThreading、SimplifyCFG、InstCombine、DCE；每步 whole-Module verify、Outside 全文与对象身份比较均通过。另一个故意未优化的 ordinary 函数保持原样，view 从3块6指令简化，原 dead LOAD 的 WeakVH 安全报告删除。

五个独立错误控制都由指定检查器检出：ordinary body、global initializer、named metadata、非法 return type、not-ready plan。错误 IR 不执行，not-ready 的 passes_attempted=0。自测 TU `loop_accepted_view_cleanup_toy.cpp` 为7592B / SHA256 `da3ac9ad7b5cf37f8790ebeff1aa3703324a06d7279d8c0fd000ee48d89f32b7`，include 原样0ffa；使用相同六步 selected-FPM 机制及原 Outside/verify/结构 helper，生产 main 的真实闭合门留待真实输入。编译50.979秒、链接2.411秒、自测9.832秒，均退出0。

原样0ffa随后单独构建为实际 emitter：编译49.770秒、链接2.411秒；独立 main 的 not-ready smoke 预期退出1、0pass。父 controller 实际采到该进程 `/proc/PID/maps`，确认固定 SDK和solver，无共享 LLVM。未重复 toy 的六步。

- emitter：`/dev/shm/accepted-view-cleanup-emitter-build-20261115-v1/emitter`，8722608B / `49b92cbadd07ca5563bafbfebb894c17a3dfecf800de9a7f39e25b912b4e8ae2`。
- object：13004128B / `3c247e5848cfc3a37337394c13a53f0e0bbd52bdd151f03823b3c57473236ba3`。
- 实际 SDK：225080032B / `d446226f9ff402bf1e096e197993aa8f2301d2ab81af3c573b50ae13ed2e0ed4`。
- 相同 Z3.so.4.13：`4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`。

CPU0–3，GCC14/LLVM21.1.8，固定每命令180秒、启动512MiB/运行64MiB空间守门；只链接固定 SDK，不叠共享 LLVM/private implementation object。

## 实际 B：恰一次固定六步

实际计划 SHA `c0fff647988c3613d5a918a5f497128c5cf8439e1ba9f83012785129193080c1`，在 source/actual closed REPORT/FILESET/STATUS 和419条库存 pin 核对后运行。输入 source SHA `714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`，实际 producer REPORT `aebcd589202cced2d99411fc6524648ca7bceef10d4e9a0bb83ba5e718b0b1c0`，ledger `01d0af4a6fb4874a2c9174935c3f324ad4547df876016ca632a1a38bb4d4f12b`，STATUS `9be49f5c3375fb37ec3d2fdc9e20a06ab43334c213249ea450ec42b436dd9992`。input seal与库存准备及其历史失败由同目录 inventory 记录负责。

实际运行40.154秒、退出0，六阶段全部 wholeverify/Outside 严格检查通过，source及所有 pin 前后不变，实际 SDK/solver maps 前后匹配。没有试其它 recipe 或重跑六步。

| 阶段 | view块 | view指令 | switch | reachable cyclic switch | 原库存read存活 |
|---|---:|---:|---:|---:|---:|
| source |733|16814|1|0|419|
| SCCP |733|14500|1|0|419|
| DFAJumpThreading |733|14500|1|0|419|
| JumpThreading |450|11877|1|0|419|
| SimplifyCFG |448|11868|0|0|419|
| InstCombine |448|10900|0|0|345|
| DCE |448|10900|0|0|345|

419是全部原 simple i64 LOAD 的完整超集，不是419个T/表目标读取；419→345只表示这些原对象删除74个，也不等于完整最终LOAD数量（优化可能克隆其它读取），更不能说删除74个T。清单没有按地址/名字挑成功项，512上限保持不变。source本来 cyclicSwitch0，因此本次不是以“计数变0”冒充首次完整去平坦化。

raw `00-source.ll` 原样保留；fresh AsmParser 真实 ModuleID 是 `<string>`，没有set/strip补偿差异，`00-parsed.ll`和其hash单独记录。其它函数/全局比较以该真实 parsed Module 为基线。report固定 semantic_validation=false/deobfuscation_complete=false，不把结构优化替代执行对照。

## 输出与证据

- ROCK结果根：`/home/ubuntu/vmc-runs/accepted-view-cleanup-real-B-20261115-v1`。
- Mac完整镜像：`/Users/c/Desktop/deob/validation-artifacts/accepted-view-cleanup-real-B-20261115-v1`。
- `after.ll`：1280465B / SHA256 `8418fd314005fa8b771473ea56e907a352416b459246e4f0e39d796c55ee1d2e`。
- `REPORT.json`：SHA256 `6ed3489244c19e6c28b039b455810e06e19beda59aaf33dfbf31d3f2321c6c31`。
- 外层资源/argv/pin receipt：同名 `...-control/RECEIPT.json`；实际三条 controller 在本目录原样保留。
- toy与独立构建：各自 `accepted-view-cleanup-toy-20261115-v1`、`accepted-view-cleanup-emitter-build-20261115-v1`，其RAM原件和日志/源一起进入本轮小包；SDK大文件只引用已封大包，不重复。

本阶段 Guest/native/Unicorn0；不是新的C/UC参考校准，不重计core producer或旧原生帧。实际清理后的32全状态/observer对照及相应控制结果必须看后续独立记录。所有真实IR逐stage、stdout/stderr、预期拒绝、toy的故意坏IR、source/argv及对象/可执行文件均保留。没有新增排除或归一化。
