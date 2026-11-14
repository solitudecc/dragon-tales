# 默认谓词证明：operand 边界与有界调度（2026-11-14，phase1）

本阶段基于 `fb85840`，修复简单不透明谓词在默认公开 `predicates` 步骤中未被证明的
两个门槛：安全 nowrap 算术/纯 funnel 的降低，以及早期变化条件耗尽查询预算。
实际最终 main backend 已使 manual-O2 的目标 select 消失、状态选择成为常量。
这不是全函数去平坦化完成；其他 dispatcher、业务分支和未知状态仍须分别处理。
图像表处理属于另一个实验原型，未注册为 public pass，也不属于本完成实现或下述收益。

## 固定源与行为边界

| 项目 | SHA256 |
|---|---|
| 首版 operand-boundary CPP，真实默认收益仍为0 | `81320beb30933f3bf5e896a594f07dcb910366bfed1c440e110debe1321391f7` |
| 最终 `lib/LLVMPredicateFolding.cpp` | `298ad88ca922816b2125ccb01fad4b78363a06d209cc1c4798362fb98124e069` |
| 最终 `tests/lifters/TestLLVMPredicateFolding.cpp` | `30b904a472211ac36925ff38dd864eea147395611f0b360165bce53851a3b6b5` |
| 实际 origin/main 基线 backend | `0456b6dff29367bf6814b46e081688bcde54ddf32d935641248f316a26f70429` |
| 实际中间81320 backend | `f78ddd12f6ec53870638690a1fdd6163114ed222f84c4d4e0157c4b8d1ad6788` |
| 实际最终 GCC main backend | `32c22c410e96aae4a6bbff544af815d9ce4f4d02f4fd4303cb4ca1a6984e396b` |

NSW/NUW Add/Sub 先完整降低两个 operand，再用独立的 operand 范围验证不会额外溢出。
范围只来自 literal、直接 AND 常量 mask、zext 位宽或 full range；以扩一位的 APInt
计算端点，不用本结果的 flag、PHI 回边或路径假设自证。Flagged Mul/Shl 仍拒绝。
真实目标的 masked operand 为0..7，加-1为-1..6，因此其 NSW 可独立验证。

Canonical scalar fshl/fshr 先筛选声明、类型、三个参数、bundle/convergent/memory/effect
边界，并完整降低参数，再把结果当独立未知 SSA 输入。相同 Value 保持身份，不同调用
不合并；不建新历史 PHI 递归，不发布值、noundef 或内存事实。Literal/nested undef、
poison、未知调用及未能证明安全的 flags 继续拒绝，freeze 保持原有边界。
普通调用效果、Guest alias 和两次读取的身份限制没有放宽；MBA 模块未修改。

默认仍为 `25ms / 64 queries / 256 nodes / resource_limit200000`，深度64、宽度最多128。
新调度只在同一下降 BV 公式上，以最多八组完整数值赋值实际得到0和1时延后该条件。
抽样从不发布常量；没有双 witness、异常或采样工作不足时走原 SMT。先处理其余条件，
再按首次 consumer 原顺序，用剩余原查询处理 deferred；共享条件仍共享证明。
单一非恒定条件仍实际执行两次 solver 查询，没有把未执行的 solver 算作 query。
第二阶段在查询前已无预算时按 consumer 计 skipped；一次部分查询后的 unresolved
cache 则沿用原约定，不重复计共享 consumer。

额外调度工作独立限为 `min(maxQueries*maxExpressionNodes,1M)`，不侵吞原 lowerer
remaining；每次完整赋值预付 `1+lowered-node-count+atom-count`，queue 最多
`min(maxQueries,4096)`。这是公式/atom 规模的调度工作单位，**不是 Z3 内部总 CPU 上界**。
Ground simplify 仍依赖既有节点、深度和位宽边界；公共 Stats/C/Python ABI 没有扩张。

## 旧失败与实际回归

最终新增12 case 的同一份测试在真实 origin/main `fb85840/0456…` 上运行：
12 cases / 688 assertions，6 cases / 13 assertions fail。Catch 返回13；首次 metadata
wrapper 错把6个失败 case 数当作退出码，因此外层返回1。这个基础设施失败原样保留。
随后只读 audit-v2 核对原日志、库和退出码，没有重跑测试/native，也没有修改或放宽断言。

只读报告：
`~/vmc-runs/ordinary-cff-predicate-20261114/validation-v2/before-final-origin-main-readonly-audit-v2/REPORT.json`，
SHA `20920c732d434b474aa8a640ee8a678ae084c02fe6c50bc087cefe94e5939a4f`。
其原始 wrapper receipt 位于同级 `before-final-origin-main-receipt/`。

中间版本也保留：首次边界补丁在 origin 基线的新6 case 有4 case / 8 assertion fail，
after6/840通过；调度新增6 case 在实际81320上119 assertions、2 case / 5 assertion fail，
最终6/122通过。此前16 case 正文逐字节保留，16/1235通过；原最早10 case 为10/395。

| 实际最终验证 | 结果 |
|---|---:|
| Main focused | 22 cases / 1357 assertions pass |
| 无 Z3 focused | 2 cases / 10 assertions pass；保持 unavailable/无改写合同 |
| Main 历史 full CTest | 1163/1163 pass |
| 无 Z3 历史 full CTest | 1028/1028 pass |

新测试覆盖 early-varying 抢预算、逆序、shared consumers、单条件真实 query 数、zero/low
budgets、采样工作耗尽后回原 SMT，以及 poison/unsupported/call 拒绝。focused 数量不
加到历史 CTest 总数。没有为了通过新测试删掉旧 case 或提高默认预算。

## 独立 synthetic native 与故障对照

[producer](../scripts/validation/predicate_boundary_probe.cpp) 调公开 pass，
[独立 Python oracle](../scripts/validation/predicate_boundary_native.py) 用 unsigned
算术规格计算 expected。18 fixture 为32/64/128位、左右 funnel、恒定/变化/不同 SSA 输入
三类；每个32输入，host O0/O2。每帧比较完整66,112字节 arena，包含 opaque observer
的实际效果，不是 machine-lifter 或新的 Unicorn 指令语义 oracle。

| 本轮独立执行 | 完整正常匹配 | comparator-only controls |
|---|---:|---:|
| origin 基线 before-only 校准 | 1152 | 14976 |
| 实际最终 main backend | 另1152 | 另14976 |

固定域为576种 module/input、96种初始 arena；host 优化级别和 before/after 重复执行不
扩充业务输入域。恒定组必须折叠，变化/不同 SSA 组必须保留并实际覆盖两种条件结果。
两个独立 saved audit 均核对完整帧、13个控制偏移、精确 header/domain 顺序、XZ CRC/EOF
及文件前后 pins。审计本身不新增 native。报告分别为：

- `native-baseline-v1/` 与 `saved-baseline-independent-v1/REPORT.json`，audit SHA `10f4cfeba6a253d96f44f055dda3e527e288160b9cdd301f5602d321ad96f2be`。
- `native-formal-v2/` 与 `saved-formal-v2-independent/REPORT.json`，audit SHA `f6b7a064d58c60dd480f8222f0fde7c9e432045c3fe5546ddf7af847f76bdfbc`；同目录另保留正式 loader binding。

[语义故障](../scripts/validation/predicate_boundary_faults.py) 另在32-left-positive
冻结输入上交换 select 两臂、增加一次 observer，分别 host O0/O2，各32输入：
128次故障 native 观察，128个指定差异全部检出，原 expected 匹配数0。
这些不是正常匹配，不加入上表1152。故障域64种 fault/input、32种初始输入，未换规格
或缩减66,112字节比较。`fault-native-v1/RESULTS.json` SHA
`b4125f9b6fc78c8d79d31abd877f616752fe7f959c1f57bf3ac5c4553f50582e`；
saved audit `fault-saved-independent-v1/REPORT.json` SHA
`0c128093fd1c8c31399fbccd527d058bf9deeb3300c1665cd3b995afe9883488`。
以上相对路径均在 `~/vmc-runs/ordinary-cff-predicate-20261114/` 下。

## 真实默认收益与版本归因

保留首次81320实际默认失败：目标到达时 query 已64、remaining16078，lowerer尚未运行，
folded0、目标 select 原样。这一轮不能称为真实样本已修。最终298调度在相同默认预算下，
目标到达时 query30，lower accepted/limited=false；第31次 SAT、第32次 UNSAT 证明条件恒真。
总 query 仍64，`%1754 select` 变为 `i32 138502176`。Sampler cap16384、used4026、
321次完整赋值、deferred64，exhausted/exceptions/proofConflicts均0。

Prototype 用显式小 LD_PRELOAD hook；正式复核使用实际 GCC `32c…`、空 LD_PRELOAD，
`fold_image` 绑定到正式 libdragon，无 hook 或共享 LLVM 映射。二者完整
before/pre-predicate/after IR 与报告4/4字节相同；不把 prototype 冒认为正式 backend。
`FORMAL-TRANSFER.json` SHA `29f1ed6fa34152ddef98f88421f1388eeee1ffe3e9aa3de47904bf18bddb7e75`。

实际四图沿用默认预算，origin `0456…` folded 为0/0/0/0；最终仅 manual-O2 新增1，
其余为0。这测量的是不透明谓词消除和真实状态选择常量化，不是复制数或 full CFF。
四图记录为 `real-predicate-baseline-v1/RESULTS.json` 与 `real-predicate-v1/RESULTS.json`。

冻结真实回放完成128/128完整 native + root RET、64次 UC/Python、384个 snapshot
controls，new C runs0，skips/exclusions/normalizations均0。独立 saved 审计重核完整
66,136字节 expected/native、76个寄存器字段、全部65,536字节 Guest、8个 Exit 字段，
以及 budget0、root RET 和每个指定单字段控制，全部通过。审计没有新增执行。

原 parser-before 的旧 ModuleID 门槛保留；新的公开 module producer 所构造的
`predicate-module-probe` ID 在对应 before/after 阶段分别严格核定，再核 source、argv、
库和链 SHA。没有 strip 或归一化，也没有改旧 checker 来接受新 schema。
[独立 checker](../scripts/validation/predicate_real_replay_saved_audit.py) SHA为
`8b17875134cb3d530868e897693aa298cee36150bb3b2362205da5f76dae5caa`。
`real-replay-saved-independent-v1/REPORT.json` SHA为
`99e019b96fbdc11034d0b1a91d5de0cdcdc28d9792a99899d95bdb64f6f77e9d`。
首次 ready sealer 误用了旧 B recipe 的20/128/100000，metadata 断言在执行回放前
拒绝；失败保留。新 ready-v3 按原公开默认25/64/256/200000严格封定，没有改变生产预算。

## 证据与待补 seal

子任务冻结证据根：`~/vmc-runs/ordinary-predicate-boundaries-20261114-v1/`、`…-v2/`；
Mac 对应 `/Users/c/Desktop/deob/validation-artifacts/ordinary-predicate-boundaries-20261114/prototype-main-v{1,2}/`。
v2为78 payload / 5,471,593 bytes，双端逐文件 set/size/SHA 核验一致；
`FINAL-SUMMARY.json` SHA `b3b5a0c97899d2e67121a01f908c2d40e7b02e9c68fbff5d4e18e55a9c2672ce`，
`FILESET.json` SHA `176f8b517ea38d910098a9634ef931dea68ac0598295226cc73634144c6244c9`。
v1首次零收益和所有旧失败独立保留。

实现验证已经完成；完整 runtime/source/native/real 归档及双端核验仍在封存，合并前
将另补归档路径、payload计数、完整SHA及实现/证据 commit。归档未封定前不称最终 seal。
有界任务采用512MiB启动/64MiB运行/每CLI180s，full采用1GiB启动；本轮无排除或归一化，
未新跑69-input corpus、单指令或 IGNIL 全量 campaign，也未计入图像原型的试验结果。
