# Guarded entry：真实样本验证契约

本阶段从 `7de235d` 开始。执行前固定下述范围，随后两组真实矩阵和一组独立最终 IR 观察完成。保存输入检查仍只算输入域审计，不计新 native 验证。

## 输入事实与来源

保留原 f-cff / f-combo、O0 / O2 四个 ELF，每个输入为 `0,1,0x80000000,0xffffffff`；LLVM 根仍从 wrapper `0x1000` 进入。Guest mapping 为 1 MiB，寄存器快照 512 字节。原 oracle 在 wrapper 入口设置 RSP=`0xc808`、RSI=`0x8000`、RDI=测试输入、DF=0；其余随机初值不作为前提。

ELF wrapper 保存 RSI/RDI，真实调用 megaInit，再恢复两参数并调用 kernel。逐字节核对 wrapper 后，首次 kernel 入口 RSP 候选是 `0xc7f8`，返回槽为 `Guest[0xc7f8:0xc800]=LE64(0x101a)`。这是待运行时检查的首调用值，不是递归或循环不变量；未新采集 kernel 现场快照。API 中 register width 单位为 bit：RSP `(32,64,51192)`，RSI `(48,64,32768)`。

| 样本 | Kernel PC | CFF 间接跳转 PC | CFF 表范围（半开） |
|---|---:|---:|---:|
| f-cff O0 | `0x102a0` | `0x102d8` | `[69632,70040)` |
| f-cff O2 | `0x10080` | `0x100b6` | `[69696,70104)` |
| f-combo O0 | `0x10770` | `0x10adc` | `[90120,90528)` |
| f-combo O2 | `0x10090` | `0x101f6` | `[73792,74200)` |

每表 51 个 8 字节目标，共 408 字节，全部从冻结原 ELF 的文件区读取。`.rodata` 权限不被解释为 caller-owned Guest 不可变。16 份旧输入保存流已逐份核对初始字段和表字节；保存流只是域检查，表事实来源始终是 ELF 文件。自然业务 switch 的相邻表不加入该契约。

未知 call 或潜在别名写之后，表内容事实可能失效。SP 检查只能约束由它推导的具体栈地址，RSI 检查只能约束具体 output 地址；两者都不是通用 noalias 断言。特别是 combo 在首分发前包含分配器调用，不能预先保证完整分发器被消除。递归 kernel 调用保持普通符号，不重复注入首次入口上下文。

## 两个预冻结配方

`sp` 检查 RSP 与整张 CFF 表；`sp-output` 另检查 RSI。二者都调用公开 `guarded_entry_view`，保留原 root 并产生独立 module。view 的新入口先检查 PC、边界/非空与真实字节相等，失败回原 root。默认 source 的 IR 在创建和优化 view 后必须逐字节不变。

测试 adapter 位于 `scripts/validation/guarded_entry_real.py`。原 oracle 内 wrapper 对 kernel 是 LLVM 直接调用，所以 adapter 在**导出的测试 IR** 中仅替换 wrapper 内该一条 call 的 callee token 为 manifest 的 `view_symbol`，检查四指针 void ABI 和 `dragon.guest.call.target` metadata。原 root、fallback 和递归调用全部保留。导出 raw/bound 的 before、每轮、after 分开保存并记录 SHA；该绑定不修改生产 module。

adapter 自测已覆盖成功绑定、递归/fallback 不变及六个拒绝控制：错误 PC metadata、缺少 guarded 定义、重复 call、缺少可信 metadata、错误 wrapper、重复绑定。自测不计 native 执行。

原比较器、16 输入、C reference 和 LLVM 完整状态核查不变。每配方要求 C16/LLVM16、16 个寄存器控制、16 个 Guest 控制及 112 个 Exit 字段控制，不能新增排除或放宽比较。原三轮、最多32个 pass run不变，顺序为 mem2reg、instcombine、gvn、guest-memory(`run_mem2reg=0,edge_worklist=1`)、dispatch-specialize(`allow_plain_stores=1`)、control-flow、dce、只读 checkpoint。此 targeted recipe 不包含 MBA/predicates/deflatten。

精度门槛是至少一处 guarded 成功 body 中的真实 CFF 目标 selector LOAD 被证明为常量，或对应 CFF switch 变成直连。入口检查表字节的 LOAD、地址变成常量、复制 switch、自然业务 switch 的变化都不算。完整状态通过本身不等于精度门槛通过。

错误表字节/SP/size/PC 与 fallback 行为由独立 native 组覆盖，不能把破坏后的 Guest 强套原 C 程序的期望值。任何成功路径 observer 均为单独统计的最终 IR 观察构建，不允许在优化输入插入观测 call；若成功边无法唯一识别，报告缺少实际命中证据，不能猜标签。

## 冻结记录

执行前目录 `/tmp/dragon-guarded-entry-real-freeze` 含 `PREDECLARED.json`、`CONTEXT-FACTS.json`、`BASELINE-PREDECLARED.json`、`SAVED-INPUT-DOMAIN-CHECK.json`。

- PREDECLARED SHA256：`d6c4d0fb5cb6c5fc4d1ea6086e0dd2a6aba0110cf9cf91d4f89e074439e36055`
- 最终执行前 adapter SHA256：`812c7862616804db32a40ace8984da38634362017a64f172180d752443239b0e`
- CONTEXT-FACTS SHA256：`d3aa9243472b136d2caac5cb7dcbaeab133fe4b647d10967e12446464bb55669`
- 原完整 evidence archive：`/Users/c/Desktop/deob/validation-artifacts/guest-edge-worklist-real-20261111.tar.gz`，SHA256 `8612e8ece6af95c519912beb84f45e1d9f5938773788fa7098f2eb9dfc83fef5`。

旧 raw 目录已按完整归档核验后退役，不将归档内部路径当现存库使用。


## 执行结果：语义通过，真实精度门槛未通过

固定 main runtime 位于 `~/vmc-runs/guarded-entry-20261111/runtime-main/`，backend SHA256 为 `32f2bd3dd929b4e678c6cbb61c3a63023623687371c2fa9f60af387cc0fc325c`，C shim 为 `be0f17a386770ac94a0720b3c4465aa1cca5de66b5f9cb66c29a687fe59ed975`。所有库、六个样本文件、冻结输入、driver 与导入源码在执行前后哈希一致。没有增加预算、外层轮数、排除、跳过或语义归一化。

| 主矩阵 | C reference | LLVM 完整状态 | 寄存器/Guest/Exit 故障控制 | source IR 不变 / Graph 不变 | 耗时 |
|---|---:|---:|---:|---:|---:|
| SP-only | 16/16 | 16/16 | 16/16、16/16、112/112 | 4/4、4/4 | 95.16 s |
| SP+output | 16/16 | 16/16 | 16/16、16/16、112/112 | 4/4、4/4 | 87.29 s |

这里的时间只是两次实际运行记录，不是性能对比结论。每组 GuestMemorySSA 12/12 次报告 incomplete（budget counter 合计12），dispatch-specialize 12/12 次报告 incomplete（budget counter 合计15），其 switches_rewritten 合计均为0。旧 summary 的 GuestSSA-prefix 字段不统计 builtin guest-memory，必须读取 bounded-memory report 的实际 pass records。

两个配方的 guarded 成功 body 在最终 IR 中均保留以下结果；自然业务 switch 和 guard 的表校验 LOAD 已排除：

| 样本 | 真实 CFF switches | 仍依赖真实 LOAD 的 selector | 已知实际 LOAD 地址数（逐轮） |
|---|---:|---:|---|
| f-cff O0 | 18 | 18 | 7 → 14 → 14 |
| f-cff O2 | 17 | 17 | 13 → 13 → 13 |
| f-combo O0 | 3 | 3 | 0 → 2 → 2 |
| f-combo O2 | 4 | 4 | 3 → 3 → 3 |

未取得预先要求的任何真实 CFF selector 内容常量化或直连收益，精度 gate 保持失败。不能把表入口 guard、这些地址证明或 switch 副本数量称为已去平坦化。相较上一阶段单模块结果，guarded view 因额外克隆、guard 和共享预算占用，f-cff O0 地址覆盖为14而非16，combo O0为2而非5；默认 source 本身完全未被改写。

独立源码/IR审查进一步定位 f-cff O0 首次分发：最终真实 selector `%2649` 从 `%2582` LOAD，同最后 guard 的 Guest[70032] 指针一致；然而从 guard 到目标需经过九个机器块，沿途有五次动态栈 STORE，**没有 call**。machine-root 的 RSP PHI 有20条入边，仅 guarded 入边是51192；其余合法回入值不能当首次SP。现有无路径限定的内存证明因此不能排除覆盖表槽。这里不能误归因为首次路径未知 call，也不能用全局替换 RSP PHI 消除真实循环路径。进一步说明见独立 `guarded-entry-loop-diagnosis-20261111.md`。本阶段未据此扩预算或另开优化配方。

## 独立 guard 命中观察

只对 SP-only 的**最终已优化 IR**做额外观察。脚本先核对51个表值相等检查组成单链、失败均进入原 root fallback、SP检查和唯一成功边，再拆分最后成功边，精确修复目的块PHI incoming labels，插入向 stderr 写 marker 的 host call。该 IR 只经 verify 与 llc/gcc 编译，没有再送进任何优化 pass；主矩阵 IR 完全不变。

四个 ELF 共16次观察：每份1,049,176字节完整输出与主矩阵保存流逐字节相同，16/16输入各恰好命中一次 guard成功marker；16个输出翻转控制与48个marker缺失/重复/错误控制全部检出。该组不是额外 C reference 或主矩阵比较，不能与前述 C16/LLVM16相加。它证明 SP-only 实际进入成功路径，但没有改变0真实CFF收益结论。SP+output 未另跑 observer，不宣称其新增独立marker覆盖；坏表/SP/size/PC的运行时边界由另一个独立native验证组报告。

## 完整证据与空间处理

Mac目录为 `/Users/c/Desktop/deob/validation-artifacts/guarded-entry-20261111/`。每个压缩包均在VM和Mac核对归档SHA、精确文件集合、每个payload的长度和SHA后，才重新核对并退役对应VM展开组；原始失败和日志保留。

| 包 | SHA256 | 压缩字节 | payload 文件 / 字节 |
|---|---|---:|---:|
| `real-sp.tar.gz` | `d8dde4e58470050b42ba0ce734db3cee651a4f93955ff89dde97324bd70015f5` | 57,156,596 | 309 / 230,760,905 |
| `real-sp-output.tar.gz` | `6c3560c87de4ec6e887d023dafd6662656fd886c053070ba12959cd33c165508` | 57,541,933 | 432 / 232,262,483 |
| `real-final.tar.gz` | `525992efb770b3a12ee26d67057b5a9280a32551bfc90b258b129349747ffd3b` | 34,947,743 | 382 / 48,615,068 |

对应VM包为 `~/vmc-runs/guarded-entry-real-20261111-sp.tar.gz` 和 `~/vmc-runs/guarded-entry-real-20261111-sp-output.tar.gz`。首组展开目录 `candidate-v1-sp` 180文件/229,265,393字节已退役；第二组真实目录为 `candidate-v1-sp-output-output`，180文件/229,280,031字节已退役。第二个重复output来自runner生成时目录字符串替换，launch中recipe始终正确为sp-output；第一次按短路径复制失败属于文件操作错误，未改变任何执行、输入或比较，也未重跑。两份ARCHIVED记录给出精确恢复命令。本机保留必要IR供独立诊断。

`real-final.tar.gz` 包含 observer、外层审计、冻结记录和归档前报告，其VM路径为 `~/vmc-runs/guarded-entry-real-20261111-final.tar.gz`。三个包共同构成完整本阶段真实验证证据，不复制runtime库。observer的113文件/45,356,831字节在双端逐payload核验及重新核对展开文件后已退役，`sp-final-observer-ARCHIVED.json` 留有恢复命令；三个ARCHIVED记录也复制到Mac归档目录。当前文档补充了最终包自身的定位。
