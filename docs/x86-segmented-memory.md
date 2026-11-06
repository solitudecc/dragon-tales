# x86 段内存访问的保守提升

2026-11-06/07，基线 `b95c84b`。本阶段修复通用 x86 提升器把无法建模的段内存访问当成
普通平坦地址的问题。处理方式是诚实地回退为 UNDEF 并登记 `unlifted`，没有增加
FS_BASE/GS_BASE、段描述符表或 Guest ABI，也不提供真实段内存访问的可执行语义。
已测的首版生产提交为 `fdf138c`，最终收敛的生产、fence 测试与夹具提交为 `359df9a`，
对应 hardened 快照的主库 `7223b80f…` / 无 Z3 `aa127d9f…`，所列最终证据已验证。
Git 最终状态由外部 receipt 记录，不与执行验证记录混记。

**最终范围：诚实的段访问回退，并恢复原本应为 NOOP 的 PAUSE/内存 fences。**
明确标为 fdf 的条目是已冻结的 pre-hardening 历史，与 hardened 最终结果分别列示。
LLVM 的保守 mayLoad/mayStore 元数据曾把 MFENCE/PAUSE 误当成需要停止的未知内存操作，
该回归已修正，并以 hardened 库重新执行本文列出的最终验证。

TLS marker 也使现有 `memoryState` 整块拒绝，产生 14 条 Lua switch 的
`(输入,PC,策略)` 目标撤回观测：4 个 ELF × 两策略共 8 图，按 `(ELF,PC)` 去重为 7 个
输入内位点（3 个 O2 变体各 2 个、O3 变体 1 个），不是 14 个独立程序或源级 switch。
14 项函数成员集合变化也是 `(输入,函数入口,策略)` 记录，去掉策略为 7 项输入内入口，
不能称为 14 个独立函数或 7 个独立源码函数。
30 个独立 guard CPU 探针支持这些样本的旧边界。当前保留其未解析精度损失，不能称兼容通过。
通用 post-opaque memory snapshot 原型已撤回，不合并、不注册其测试为已实现能力：
其恢复的未知读取关系，可能扩大下游尚无内容证书的 image 表目标发布。必须先建立表内容
来源/alias/不可变证明，再考虑恢复通用关联；不能为了恢复 switch 而忽略 marker。

## 原问题与修复范围

上一阶段的[有界调用效果证明](stack-call-effects.md)发现：
`55 64 48 8b 2c 24 48 83 c4 08 c3` 在 push RBP 后读取 `FS:[RSP]`，
当 `FS_BASE=0x1000`、普通保存槽为 `0x1040`、对应 FS 槽为 `0x1060` 时，实际返回
RBP 为 `0x1060`。旧通用 `emitAddress` 丢掉段基址，且没有标记未提升，会产生看似确定的
平坦 LOAD，导致分析错误地保留原 RBP。上一阶段只给调用证明增加了拒绝门禁；本阶段处理
普通根函数、切片及 LLVM 提升共同使用的通用指令 IR 边界。

本阶段在原始 LLVM MC 内存操作数仍然存在、尚未改写成 scratch 寄存器之前检查段：

- x86-64 实际内存访问使用 FS/GS 时，不能在现有状态模型中表示其基址，进入 unsupported 回退。
- x86-32 显式 CS/DS/ES/SS/FS/GS 内存覆盖均进入同样的保守范围，不假定段描述符为平坦映射。
- 普通内存五元组和 moffs 的操作数布局分别识别。立即数、位移或操作码中出现 `0x64/0x65`
  不会被当成段覆盖。
- LOAD、STORE、读改写、条件移动、SSE 及内存间接 CALL/JMP 均不能保留错误的平坦内存地址。
  不支持指令定义的显式/隐式建模寄存器和算术标志按既有规则变为 UNDEF；XMM/YMM/ZMM
  定义使两个建模 XMM 半部都失效，读取 XMM 的 STORE 不伪造寄存器定义。
- 内存间接 CALL/JMP 额外保留 UNDEF 目标与对应 CALL/JMP 操作，维护控制流骨架；未知目标
  不变成直接目标或正常顺序执行。
- MASKMOVDQU 的内存写通过隐式 (E)DI 发生，LLVM MC 只有两个 XMM 操作数。通用 unsupported
  判定同时检查 `mayLoad()/mayStore()`，不能因为没有显式内存五元组或寄存器定义就漏掉 marker。

## 保留的合法行为与专用路径

LEA 只计算有效偏移，不访问段内存，段覆盖应被忽略；其正确提升继续保留。边界回归还暴露
`67 48 8d 43 65` 在 `RBX=0x12345678fffffff0` 时，旧 IR 得到 `0x100000055`，
Unicorn 得到 `0x55`。LEA 现按 MC 基址/索引的实际地址宽度计算，再写入结果寄存器，保留
地址回绕和结果扩展规则；该窄形式已由新组件和原生正例复验。普通内存和无基址/索引
形式没有随之扩大修复范围。

x86-64 的 CS/DS/ES/SS 覆盖不改变普通内存访问的基址，继续按平坦地址提升。
内存形式 NOP、prefetch 等提示不产生架构可见的数据结果，继续保留已有行为。隐式 SS 栈访问，
例如 PUSH 寄存器、POP 寄存器、RET 和直接 CALL，不因 FS/GS 原始前缀字节被错误拒绝。
PUSH/POP 的显式内存操作数仍受段检查。

字符串及 x87 的已有专用路径优先保留。MOVS/LODS/CMPS 的未知 source 段可使用 UNDEF
地址并登记 unlifted，同时保留安全的索引更新、计数和 REP 零次保护；x86-32 显式普通
DS 等 source 覆盖也纳入未知范围。STOS/SCAS 的目的固定使用 ES，FS/GS 覆盖不重定向
该目的。x87 继续使用已有未知地址和建模状态失效规则，不用通用回退覆盖其专用处理。

## `unlifted` 是必须检查的边界

纯 STORE 的 unsupported 回退允许没有任何 IGNIL 内存操作。未知内存效果由 `unlifted`
标记暴露；它不等于“这条指令没有写内存”。旧切片器的 `storedValue`/`memoryState` 和
有限值集分析均须在该边界停止相应内存推断，不能继续转发先前 STORE 的常量。

StatefulV2 和 StaticAnalysis Guest 执行在未提升指令处返回 Unsupported。Legacy 的单块
trace 接口拒绝带 marker 的输入，但 Legacy 整函数路径目前不会因此统一停止，不能据此
保证不支持的段内存语义。低层消费者若只取 IGNIL 操作并忽略 `Block::unlifted()`，也可能
继续使用旧内存；这样的部分 IR 不能作为完整可执行语义。本阶段没有增加全内存 UNDEF
操作，也没有改变 Guest 或 C/Python ABI。

## 验证记录

### 最终验证

生产已收回通用 memory snapshot 原型，相对 fdf 仅恢复 PAUSE/MFENCE/LFENCE/SFENCE 的
NOOP 集合。新主 CTest 632/632、无 Z3 507/507 通过；组件 13 用例 / 1142 断言，
无 Z3 13 / 1114 全部通过。两构建 42 / 39 个对象逐 hash 核对，各仅 X86IGNILLifter.cpp.o
一项变化，LLVM lowering 及其余生产对象完全相同，before/after/changes receipts 保留。
原型没有构建或合并。上述 14 条观测的保守未解析
代价保持，不把通过单测或 IR verify 称为兼容通过。

最终语料已执行并经独立 forward audit：69 个输入 / 138 策略全部 fresh Graph。
其中 raw 新生成覆盖 3 输入 / 6 策略 / 10467 有效函数检查点 / 86 模块；已验证缓存覆盖
66 输入 / 132 策略 / 194939 检查点 / 1589 模块。合计 205406 检查点、1675 raw 模块，
lift/verify/coverage 错误均为零。缓存以新 Graph、相同 lowering 对象及逐项 provenance
核对为依据，不能说成全部 69 输入的 raw 都重新生成。

最终 unlifted 总数 233134，较 fdf 的 233150 精确减少 16 个 fence/PAUSE；较 fdf 有
5 个 canonical / 0 个 hardware 指纹变化，较 b95 有 64 / 8。14 条 site/strategy withdrawals
原样保留为精度代价，`semantic_compatibility_claim=false`。三库警告同为 missing IGNIL 6、
undecoded 边 26、invalid 候选 3、空 symbol 函数 0、objdump missing 146。
独立审计核对全部 138 条 delta 合成、116 份实际 Git 源及 42/39 完整对象集；
`metadata/corpus-hardened-final/forward-audit.json` SHA 为
`f5f45b974a88dba6b7521f42b9e5a5879cc7e58bdc994c3b7304df98e6593c85`。
这是身份、来源与 raw 合法性/覆盖审计，不是全语料语义证明；全量 optimized 仍未重跑。
额外 delta 审计逐项确认 fdf→最终仅移除 16 条 hint marker（10 MFENCE、6 PAUSE，
涉及 5 图），这些记录的 IGNIL 文本完全一致；marker 元数据仍会改变 V2 的 Unsupported
边界，不能据此把相关 raw LLVM 缓存当作未变输入。该审计初稿把 5 图误作 5 IR 记录，
计数单位断言失败的原源码/说明仍保留，修正单位后没有放宽语义检查。

完整完成态 metadata/source/JSON/records/complete/deltas 包为
`raw-hardened/final-incremental-metadata.tar.gz`，SHA-256
`77472a9c5ab59f04bacc283735fda219a769ce55286af9a2c7013d95c9b20503`。
599 个 payload 文件逐成员 SHA 核对，另有 1 个与 sidecar 完全一致的自描述 manifest；
219 目录记录在 manifest 中（tar 无目录 headers）。Mac 独立复核 receipt 已回传 VM，
冻结 DSO、原始输入及 fdf 缓存仍为单独 pin 的依赖，不把此 metadata 包说成独立完整运行环境。

新库和公共接口保存在开发机 `hardened-final/`、`hardened-final-noz3/`，78 文件逐 VM
SHA-256/大小一致，零差异；`hardened-snapshot-manifest.json` 与 VM 同份，SHA 为
`f72b4dce21faf80ed20d6eebf46e15ef64a3ae9380379069a806b5161f371309`。
此快照用于最终原生 pin，旧 `final/`、`final-noz3/` 和旧 manifest 原样保留为 fdf
pre-hardening，未被覆盖。

硬化后的原生独立终审为 `hardened-native-final-audit.json`：17 个 fresh 组、2632 次比较。
新库有 896 次完整 root 匹配、1568 次明确的未提升指令前边界匹配。core20 夹具主库六组
为 576 完整 / 1344 边界，无 Z3 为 96 / 224；fence 两夹具主库六组 192 完整、无 Z3
32 完整，共 224 次正常完成，不再被错误停在 hint 上。

结果/内存负控各 80 次精确单字段/字节差异，内存均 `count=1`。fdf hint 失败控制的 8 行
均在声明的 hint PC 返回 Unsupported，而 CPU 应正常完成；它是旧版错误拒绝的独立控制，
不计新库成功。392 个 fresh 可执行文件/模块报告及实际两库 DSO 哈希逐组核对，78 个
快照文件全 hash 一致；错误、排除、跳过、归一化均为零，比较全部建模字段和 64 KiB 内存。

旧 `b95c84b` 六组 1920 行明确复用并重核原 JSON 成员 SHA 及 240 份 ldd：480 次普通
完整匹配、96 次 LEA 仅 RAX 语义差异、1344 次 Unsupported 契约差异。没有把任何 fdf
成功记录充作硬化库执行。`raw-hardened/native.tar.gz` 的 7312 个常规成员逐 SHA 核验通过，
75 份小记录在 `metadata/hardened-native/final/`；原生和上述 corpus 各自完成独立组件终审。

函数级标准 Tigress 的 MBA/opaque/combo × 两策略六组已全部使用最终新库 fresh 完成：
C 288、LLVM 1728 次完整匹配，CFG wrong、unlifted、executed-undecoded 均为零。
每组两 ELF × 默认六 LLVM plans × 24 输入；72 份可执行文件 ldd 按 V2 动态和 static
独立核对，实际 DSO pin 与 hardened 新库一致。六份逐成员 SHA 归档及语义审核全部完成。
这里扩展到六计划，不能把本轮 1728 与历史仅两 product 计划的 576 相减，算成恢复收益。
普通 goto/indirect 另四组也已使用新库 fresh 完成：C 256、LLVM 576 complete / 960
incomplete，GCC 每策略各 192 / 192，Clang 每策略各 96 / 288。wrong、unlifted、
已解析目标遗漏均为零，保留原始 rc1 与 `semantic_complete=false`；
incomplete 不是完整函数成功。矩阵入口为双端 `hardened-tigress-matrix.json`。
十组共 C 544、LLVM 2304 complete / 960 incomplete，均为最终新库 fresh 执行。
普通组另保留 492 条未解析 transfer 观测、2538 条参考 hook PC 未解码观测；按 group/case
去重为 82 条 transfer 和 423 个 PC。跨六 plans 会重复，既不能算独立程序数，也不能
把这些未解码观测写成零。Pass 报告保留 36 SUCCESS / 60 INCOMPLETE。

正式聚合 `hardened-tigress-final-audit.json` SHA-256 为
`a251c81ab764615a6f17dd993d856c7005ca73a0a0afb6bf10596befedd2fd74`，
`validation_complete=true`、`semantic_complete=false`。168 份 exe ldd 与十个归档的
9578 个常规成员已核验；原始 4324853358 bytes、压缩 3782666624 bytes。十个拥有的
VM raw 子目录均在核验、复查源码及归档后清理，小型证据双端保留。

普通组初版审核器错误要求所有 Pass 状态 SUCCESS，曾拒绝原本就包含 Incomplete 的冻结
结果；原 source、suite、gate 和失败 stdout 全部保留。新审核器通过独立 extension 按每个
key 对比冻结期望状态，status2 仍为 Incomplete，未归一化为成功；0↔2、缺失/重复 report、
ERROR/未知状态等 10 个故意破坏控制通过。此修正针对错误的审核门禁，没有改变程序结果
或隐藏普通分发的 partial 状态。

### 旧代码回归已确认失败

`tests/disassemblers/TestX86Segments.cpp` 的 11 个用例仅应用于旧 `b95c84b` 源码后，
实际构建并运行：8 个用例失败、3 个通过；1300 条断言中 346 条失败。
这包含普通段内存、moffs、SSE 双半部、间接控制流、隐式 MASKMOVDQU、32 位字符串 source
以及禁止错误内存常量转发的回归。3 组通过的正例检查 long-mode 忽略的段、隐式 SS/固定
ES 和数据字节识别；LEA 组额外暴露 addr32 边界问题，不能把该失败从记录中剔除。
原始日志为 VM `~/vmc-runs/x86-segments-20261106/tests-only.log`。

当前保留的 active 段测试为 13 个用例，包含 PAUSE/MFENCE/LFENCE/SFENCE 在两种模式下
无 marker/无操作，以及 UD2 trap/marker 与原 MASKMOVDQU 对照。严格同源小 runner 的
`b95c84b` 运行 9 个用例失败，348/1338 条断言失败；fdf 仅 fence 组失败，8/1142 条
断言失败。硬化库同源 13 用例已通过，正式数字见上节，不用已撤回原型的用例数凑数。

### 独立 Unicorn 地址参考已执行

[`scripts/validation/x86_segments_oracle.py`](../scripts/validation/x86_segments_oracle.py)
不依赖 Dragon。Unicorn 2.1.4 在非零 FS/GS 基址下执行，
覆盖 8 个值边界、普通地址和映射页末 qword；检查 LOAD/STORE/ADD 的地址和算术标志、
LEA 64/addr32、长模式被忽略的段、固定 ES STOS、隐式 SS PUSH/POP 及内存 CALL/JMP。
390 项互不重复的结果核对全部通过，失败、跳过、排除和归一化均为零。脚本检查唯一记录
及固定计划数量，并检查普通指令结束 PC 和 CALL/JMP 的目标 PC，避免少执行或重复记录。

100 个故意使用平坦地址的值对照中，96 个被检测；另外 4 个是零加数的 ADD，前后数据相同，
值对照无法区分地址，明确计入 `flattened_controls_equal=4`，没有静默删除。
另将 LOAD 前的 RAX 故意设成不同于被加载值的哨兵，64/64 个“丢弃 LOAD”对照均被检测。
该记录说明非零段基址反例和正例的实际参考行为，不是 Dragon 全状态对拍或完整函数成功数。
最终 JSON/日志为 VM `~/vmc-runs/x86-segments-20261106/oracle/final-oracle.json` 与
`final-oracle.log`；早期无 dropped-load 对照的记录另保留，不重复累计。

### 修补前组件与原生矩阵（pre-hardening）

fdf 首版快照的组件 11 个用例 / 1104 条断言全部通过；无 Z3 为 11 个 / 1076 条。
主 CTest 630/630、无 Z3 505/505。首批 19 夹具原生预检被扩充的永久矩阵取代，
不计入最终矩阵覆盖。

以下首版结果不能替代上述新库复验，不把历史名称中的 final 当成 hardened 最终库，
不把已知失败写成通过。

fdf 首版两套真实语料按 AGENTS 要求使用 `LLVM_MODE=v2`，已完成 69 个相同输入、138 策略、
205406 个有效函数检查点、1675 个 V2 raw 模块；冻结旧库只作 Graph-only 对照。
276 组实际库 maps 和全部输入/源码/差异 SHA 已核对。64 图的 canonical/unlifted 变化，
记录的 unlifted 总数为 224690→233150；旧/新警告同为 missing IGNIL 6、undecoded 边 26、
invalid 候选 3、objdump missing 146。**这只是首版身份、IR 合法性及覆盖审计通过，不是
语义兼容通过：8 图有 14 条 site/strategy guard/table 目标撤回观测。该历史发现促成后续
审查；最终决定保守未解析并撤回通用原型，没有宣称本分支已恢复它们。**
驱动及 JSON 已保存到开发机 `metadata/corpus-v2-pre-hardening/forward-audit.{py,json}`；
硬化后的独立全量审计另见上节，不将首版身份审核当作兼容通过。
`optimized_corpus_rerun=false`，不能称为全 69 输入 optimized IR 重跑，更不能声称旧库
LLVM IR 或全程序语义重新通过。段访问原先的 LOAD 改为 UNDEF/unlifted 是预期的精度和
unsupported 变化，不要求 IGNIL 与 marker 总数完全相同。最终须逐项汇总差异及其原因，
完整差异永久保留；配对用旧 canonical 临时文件可以在差异保存后移除。

先前 static raw+optimized 扩展完成的 15 项对应 30 策略、28770 个有效函数检查点，
917 个 raw 与 917 个 optimized 模块 verify；12 个 canonical Graph 发生变化，不等于
发现 12 个错误，须按真实 IR/marker 变化解释。第 16 项 `sqlite_a64_O2` 为 partial，
不计入上述已完成数字。
校准验证模式时显式 SIGTERM 停止该任务，旧 job 退出 143；所有产物保留，receipt 为
阶段目录 `static-corpus-interruption.json`。这些记录只是 static 补充覆盖，不复用为
V2 全量通过，也不把中断算成通过或隐藏失败。

`scripts/validation/x86_segments_diff.py` 与永久夹具将完整返回的正例和 unsupported 边界
分开统计。边界夹具先执行有寄存器、SP 和内存效果的真实前缀，再在未提升机器指令前退出；
比较全部建模字段和 64 KiB 内存。这样的匹配证明正确停止和前缀状态，不能记作完成了该段
指令或完整函数。旧实现继续越过该位置是边界契约差异，不应笼统计成完整 CPU 语义错误。
结果和内存控制必须各自检测单字段差异。

修补前原生矩阵使用 20 个永久 i386/x64 夹具、两策略、V2/static/product、O0/O2 与
8 输入。新主库 6 组共 1920 次比较：576 次完整 root 返回匹配、1344 次明确的未提升指令前
边界匹配。无 Z3 另 1 组 320 次：96 次完整 root、224 次边界匹配。

冻结旧 `b95c84b` 同一 20 夹具的 6 组 1920 次比较，480 次普通正例完整匹配；
96 次 addr32 LEA 均正常返回，但只有 RAX 出现已确认的 CPU 语义差异；1344 次旧库继续
越过应拒绝的段内存位置，属于 Unsupported 契约差异。这两类差异分开保留，没有删掉
旧 LEA 失败，也没有把旧段边界差异算成整套 CPU 语义错误。

结果和内存负对照各 80 次，分别准确检出一个结果字段或一个内存字节的差异。
这份首版 15 组总计 4320 次比较，错误、排除、跳过及归一化均为零；比较全部建模字段与 64 KiB
内存。每组 40 份唯一 `(case,opt)` 模块报告，共 600 份，及对应 600 份 ldd 逐项检查；
15 组夹具和驱动 SHA
一致，实际 DSO 哈希对应首版主库、无 Z3 和冻结旧库。首版审计为开发机阶段目录的
`native-final-audit.json`；名称中的 final 指当时首版矩阵，不是硬化后的最终库。
先前 quota partial 和 19 夹具预检不计入这些比较数。

### 单指令与真实块检查

单指令 x64 原脚本在 `TRIALS=8` 下得到 986 个 OK 模板、0 DIFFERENT、6 unlifted、
0 汇编失败。这是模板分类数，不能作为总执行次数。SSE 原脚本不读取 TRIALS 环境变量，
内部固定每模板 60 次；393 个 OK 模板对应 23580 次 trials，0 DIFFERENT、0 unlifted，
700 个 NaN 结果字段保持 unknown。原脚本的未定义标志、NaN 载荷等处理仍按
[正确性规则](correctness.md)及其既有注释执行，没有新增豁免来通过本阶段。

真实 IGNIL 块对拍分别运行 raw 和 simplify+DSE 两种计划，每计划 450 个块、1235 次
compared、115 个 Unicorn fault，0 DIFFERENT。每计划另记录高地址内存未比较 114、
写出 known 区域 81、跳过 call/system 234、跳过 unlifted 7；这些是原脚本不同层级的计数，
不能混加为完整成功数。两计划使用同一抽样块，不能说成新增 900 个独立真实块。
日志为阶段目录 `realblock-{raw,opt}-lua*.log`。

LLVM 真实块使用 Legacy 路径，扩大为每架构 30 个抽样块后发现既有差异，
**原始报告保留 1 DIFFERENT，不能改成零差异**：

| 架构 | 新库 DIFFERENT | 原脚本其他计数 |
|---|---:|---|
| i386 | 1 | fault 2、unlifted 1 |
| x64 | 0 | high-address 6、fault 4 |
| A64 | 0 | high-address 12、fault 3、page-unmapped 6、unlifted 1 |

Lua i386 块 `0x43319d` 的序列为
`fld tbyte [eax]; lea ebx,[eax+12]; fxam; wait; fnstsw ax; and ax,17664; cmp ax,256`。
LLVM 的 EAX 为 `0x10000000`，Unicorn 为 `0x10000400`；CF 与 SF 均为 LLVM 1、Unicorn 0。
冻结 `b95c84b` 对同样 30 个 i386 块得到逐字相同的日志和差异。新旧两个原始小日志均已保存在
开发机阶段产物 `known-unresolved/legacy-x87/`，SHA-256 相同：
`16cdf76dcf32992c7ebd0e75e3920e8ab1d7c223251dd89b1946697b57c5248a`。

随后已闭环为 Unicorn 2.1.4 的已知参考差异，而不是待修 Dragon 错误。恢复出的 raw80 为
`5b3bb0dbadd274445c7a`，非零有限 exponent 为 `0x7a5c`、显式 integer/J 位为 0，
属于 unsupported unnormal 数据编码。[Intel Vol.1 §8.2.2 / Table 8-3](https://cdrdv2-public.intel.com/825743/325462-sdm-vol-1-2abcd-3abcd-4.pdf)
区分此编码和 pseudo-denormal；[Intel Vol.2A FXAM / Table 3-44](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf)
规定其 C3/C2/C0 为 000，而正常有限数为 010。

独立 x86_64 程序在 Rosetta 下保留相同 FLD 后 raw80，FXAM 分类及完整整数后缀与 Dragon
一致；Unicorn32/64 则把原编码误分成 normal。4 个输入原 raw / 只翻 J 位 / 正常 +1 / +0，
Rosetta 的 `SW&0x4500` 为 `0000/0400/0400/4000`，Unicorn 为
`0400/0400/0400/4000`。配置要求的 ucdev 和系统 Python 结果完全一致；8 个类别探针中
2 个原编码参考差异保留、6 个控制匹配，另有 1 个 exact-seed 完整小块重现原始差异。
第二参考是 Rosetta，不声称真实 x86 硬件，也不外推全部 x87 语义正确。

8 个说明、源码、独立可执行文件及原始输出保存在上述历史目录的 `reference-resolution/`，
逐文件核对其 manifest。这里 unsupported 指 x87 数据编码，FXAM 指令本身合法且结果有定义，
不是 lifter 的 unlifted，也不是架构 undefined 豁免。本阶段没有修改参考 harness、扩大豁免
或归一化，原 LLVM 组仍按 1 DIFFERENT 报告。

## 已知盲区

### Image 表内容需要独立证书

冻结 `b95c84b` 与 fdf 首版在绝对表、signed32 相对表两种编码、两策略下，都能发布过期的
`0x1040/0x1050`：程序在 guard 之前用普通、已支持的 STORE 把外部表 entry0 改为指向
`0x1060`，Unicorn 实际跳到 `0x1060` 并正常返回。全部 128 字节指令保持不变，表经
`AnalysisHints.readMemory` 提供且位于分析代码范围外；这不是自修改代码，也不依赖 addr32
或新 opaque 实现。8 次分析与 2 次独立完整 CPU 返回保留该缺口，没有排除或归一化。

有界索引及同 snapshot 的重复读取关联，只能证明未知值之间的关系，不能证明数据表仍等于
初始 image。重复候选复验如果再次代入同一组未证明的 image 字节，也无法补上该证书。
更近的 STORE/CALL 锚点还可能遮住更早的表写，不能只检查最近 memoryState 是否为 opaque。
原表字节变成 CFG 后继之前，必须有当前已知内存值证书，或范围明确、执行条件清楚的可信
不可变声明；只看 section 名或固定代码假设都不够。该修复尚未实现，是启用新目标发布的阻断。

共 7 份原始证据（1 说明、2 脚本及 4 JSON）已保存到开发机阶段产物
`known-unresolved/table-image-immutability/`，逐 VM SHA/大小核对一致；不计当前通过覆盖。
最终 `7223b80f…` 后端又以 tiny 探针直接复现绝对/相对表各两策略同样的错误发布，CPU
均经 `0x1060` 正常返回；不是只凭源码未变推断仍有缺口。最终证据见双端
`known-defects/known-gap-final/`，与未加入库映射观察器的输出逐字等值控制也保留。

### 地址宽度及尚未覆盖的状态

本阶段不实现段描述符、TLS、FS/GS 基址读取或非平坦段的真实内存访问；相关指令现在通过
显式未知边界阻止错误推断。默认未覆盖段仍沿用现有平坦 Guest 模型，不能据此声称完整
x86 分段机制已经支持。

普通内存 `emitAddress` 的地址宽度回绕是独立问题，尚未在本阶段修复。独立审查使用代码：

```text
48ba0400000001000000c70260100000b8fcffffff67c74008401000008b02ffe0
```

它先把 `0x1060` 写到 `[0x100000004]`，再用 addr32 把 `0x1040` 写到
`[EAX+8]`，其中 EAX 为 `0xfffffffc`；正确有效地址应回绕到 4。最后读取高地址并跳转。
Unicorn 正常返回，低地址仍为 `0x1040`、高地址仍为 `0x1060`、RAX/实际目标为 `0x1060`。
旧 IR 却在 64 位中相加，让第二次 STORE 错写高地址；默认有限值集在 `proven_calls=0`
时仍报告 complete 且目标为 `0x1040`。两处值都来自显式 Guest STORE，没有映像初始化
假设，也没有触及 Unicorn 的高地址限制。原始库哈希、输入、IR 与参考结果见冻结的
`address-audit.md`、`address_audit.py`、`address-audit.json`。
最终新库 tiny 复测仍为 `finite.complete=true` 且错误目标 `0x1040`，CPU 仍为 `0x1060`；
同源 LEA 对照最终已正确得到 4。`known-defects/known-gap-final/` 的 22 个 payload 已双端逐 hash 核验，
manifest SHA 为 `a4ec751fd194c023a6d906bddd2be4afd02f0bc7f71cbc2e6ac9d8c26f640aff`。
原观察器初稿错误和 empty capture 另留为测量方法历史，未拿空输出当通过；这些是确认未修
缺口的证据，不是生产成功测试。

该问题不能因为段访问转为 unlifted 而被遮蔽，也不能用本阶段窄基址/索引 LEA 修复、
IR verify 或普通输入的通过结果作为其修复证据。无基址/索引的地址宽度信号还需要单独
审查；32 位 addr16 和 moffs 地址宽度的 Dragon 失败尚未由该审查独立复现，不能追加为已确认的失败。

[后续地址宽度与 Legacy 边界设计](/Users/c/Desktop/deob/validation-artifacts/x86-segments-20261106/known-unresolved/address-audit/next-address-plan.md)
已归档，并同步到 VM 阶段目录同一相对位置。方案区分数据宽度、有效偏移回绕宽度和线性
指针宽度，建议用显式不可变地址上下文迁移普通内存、moffs、scratch、BT、LEA 与栈操作；
Legacy 整函数严格拒绝 marker 另列独立兼容性变更。方案中的 5 个轻量 Unicorn 探针只确认
CPU 地址规则，不是 Dragon 修后对拍；以上生产修复均尚未实现，不计本阶段完成能力。

检查与执行仍采用固定代码、单线程 Guest 模型，不涵盖自修改/JIT、并发修改或异步重入。
本轮没有堆初始化、一般参数别名、去平坦化或完整去混淆新增能力。

## 冻结档案

开发机 `/Users/c/Desktop/deob/validation-artifacts/x86-segments-20261106/` 的 `final/`
与 `final-noz3/` 已保存 fdf 首版生产库和公共 C++/C/Python 接口快照，现属 pre-hardening。
78 个文件逐份与当时 VM
SHA-256/大小一致，零差异；旧 `snapshot-manifest.json` 保留冻结时的历史状态
`freeze-pending-final-audit`，不是当前 hardened 库的审计状态，也不替代其执行矩阵。
旧 `b95c84b` 双库复用上一阶段冻结快照，没有重新复制。
`known-unresolved/address-audit/` 单列仍未解决的生产地址问题；历史路径
`known-unresolved/legacy-x87/` 已补入参考缺陷闭环说明，不再列作待修 Dragon 错误。
这些审查证据不混入原生完整返回的通过覆盖。

原生输出封存为 `raw-final/native-initial.tar.gz` 和 `native-resumed.tar.gz`，分别
10742 / 2488 个常规文件成员逐份 SHA-256 核验通过。第一轮包含 11 个完成组、1 个 quota
中断的 partial 组及独立 19 夹具预检；首版审计从两轮中核对完整 15 组计划，保留中断与
预检原始记录，不将它们重复累计。

通用关联原型的 `TestOpaqueMemoryVersions.cpp` 已原样移出 working tree 和 CMake，
归档在 Mac/VM 阶段目录 `planned-prototype/`；SHA-256 为
`2ebe9652ee6aa8ea36a9521b4993ff36d7cf4cf0bc1494cf07419ab1c946465c`。
同源旧 runner 的 `[opaque-memory]` 为 13 用例（11 原型用例加 2 个 segment 控制），
b95 为 31/303 条失败断言，fdf 为 40/309；两次均 8 用例失败。这些组合数包含 fence/UD2
控制，不是纯原型计数，也不是当前生产通过覆盖。8 份源码、原始字节日志、summary、hash
清单与说明已双端核对一致；active fence/UD2 测试没有删除。
另补入未构建的 `opaque-memory-prototype.patch`（SHA-256
`4292cc3f472b73a30fadff0dac532df718982b28f4eb0a58c211cbf97f366025`）和
`opaque-memory-blocker-plan.md`，目前共 10 文件双端核对一致。后者给出独立 table-content
certificate 的后续设计，仍是方案，不是已合并能力。

整阶段正式入口位于 VM 与开发机阶段根的 `final-artifact-index.json`、
`final-validation-summary.json`、`freeze-final-validation.json`。它们将最终库证据、
复用基线、历史失败、参考缺陷和已确认未修复问题分开索引：
`validation_complete=true`，`semantic_complete=false`，`compatibility_complete=false`。
Git 收尾状态由外部 receipt 更新；验证完成不表示全部程序已完整恢复或已知缺口已修复。
