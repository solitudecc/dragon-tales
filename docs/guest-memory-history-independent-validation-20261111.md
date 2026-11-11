# Guest checked-memory 历史快照独立验证

本轮验证目标是在 `H→S→checked LOAD` 复制路径中，首块 H 的 PHI 可以选择由后续原块 S 定义、但已支配入边前驱的历史执行值。历史入口值必须与新复制的 S 中 fresh LOAD/freeze/表达式分开；原 CFG 回入和外部使用仍须完整保留。

## 原六模式冻结的对拍方案

新增 `scripts/validation/guest_memory_history_native.cpp` 与 `guest_memory_history_diff.py`。producer 直接构造 LLVM IR，调用真实 GuestMemorySSA，保存 before/after；独立 Python 模型用顺序循环和字节数组计算期望状态，不读取重写后的 IR。

共六个模式：PHI/普通 LOAD/freeze × 单/双 hub。主体为 `entry→S→checked→valid→body→H→S`，另有未知 `seed→H` 及 `body→side→原 S`。S 定义后续所需地址及 fresh 值；body 更新 fresh-cell，再向历史 state-slot 写 14；H 的 PHI 使用旧 S 值。side 路径只在第一次迭代把 frame 加 8 后回原 S。输出同时依赖历史值、当前值与跨原块 live-out，防止错误替代恰好没有可见影响。

每模式 162 输入：3 个入口/回入模式 × 3 个 bounds 模式 × 循环次数 0/1/2 × 6 个整数边界值。共 972 输入，在 O0、O2 分别执行 before/after，计划 3888 次完整状态比较。bounds 覆盖正常、首次 LOAD 失败、首次成功而 side 回入失败。宿主真实分配始终为 64 KiB；这是直接 LLVM 变换夹具，辅助观测单元及输出写入不模拟所有 frontend bounds 检查，目标 state LOAD 的原 bounds 与失败行为必须保持。

比较完整返回值、512 字节寄存器、64 KiB Guest、7 个 Exit 字段；descriptor.data/size 另检查不变。无调用，快照 call 计数为 0，不宣称调用效果覆盖。没有 poison/undef、并发或宿主未映射访问。

六种故意错误作用于 `freeze_one`：用 fresh 替换 history、用 history 替换 fresh、删除 state STORE、删除目标 bounds、错误 side 回入 frame、损坏跨块 live-out。每个错误模型独立指定精确完整快照和应出现差异的输入集合。O0/O2 共 1944 个错误观察，预定差异分别为 132/132/144/132/48/324，总计 912。比较器逐字段及错误长度自检共 10695 项。

精度门槛独立于语义：body 的 H 入边确实重定向、loadsForwarded 大于 0，并且标记的原目标 LOAD 仍唯一存在（新复制路径对应 LOAD 被消除），unknown seed 入边保留。`--require-gains` 将该门槛计入最终结果；它不会在精度失败时跳过 native 比较。只复制其它块或转发无关 LOAD 不满足门槛。

## 当前结果

旧 backend `2e509ceb03bc5b47d2e95fdacfd579860e520f238ee239746c273b6034ba9bda` 已完成同源基线：

- 3888 次完整状态比较，0 差异。
- 1944 个错误观察，912/912 个预定错误检出。
- 10695 比较器自检通过；排除、跳过、归一化均为 0。
- 六模式均 loadsForwarded=0、redirected=0、target_loads=1，旧精度门槛未通过。
- 六模式 available/complete 均为 1，budget exhausted 均为 0。
- 所有 16 个正常/错误保存流和 1 个 expected 流已另用冻结模型复核：精确覆盖、快照字节、报告行与全部 artifact/source SHA 通过。保存流复核不另计为执行样本。

原 `deep` 的 middle 只有一个前驱，不满足既有 `prependHub` 每个前置 hub 至少两个前驱的 admission。这不是历史快照算法应扩大支持的范围。原六模式、旧源和整组精度门槛全部保留；不能把只通过单 hub 三项写成原六项精度已通过。

## 追加合法多 hub 组

另加 `phi_branch`、`load_branch`、`freeze_branch` 三模式，每个 hub 都有独立 unknown 前驱。增加 path3 从第二个 hub 进入，并保留原六模式执行。新增三模式各有 fresh/history 混用和删除 STORE 的故意错误变体；全部错误模型继续要求精确完整快照与预声明差异集合。

追加后的同源矩阵共 1620 输入、6480 正常比较、4536 故障观察，预定差异 2196，比对器自检 17823。旧 2e50 已全部通过语义与故障控制；原 `precision_ok`、单 hub `single_precision_ok`、追加组 `additional_precision_ok` 均为 false。原六个 before IR 与首版逐字节一致，原 5832 条正常/故障记录的 key、完整快照 SHA 和字段差异全部相同；另存 `original-preserved-audit.json`。

追加版的 `--require-gains` 仍检查原六项整组门槛；新 `--require-additional-gains` 只检查另名三项，不会把原整组失败改成通过。旧追加版 28 个正常/错误流及 expected 流已重新核对全部字节、输入覆盖、报告行、artifact/source SHA。

## 修复后同源结果

历史 v1 backend `1b84e712011ef8bd19583924cdb34d8b06eadbdfd23e60e152a569c1d5d4fa5b` 使用完全相同的追加源、输入和 controls，运行 `--require-additional-gains`：

| 项目 | 旧 2e50 | 历史 v1 |
|---|---:|---:|
| 完整状态比较 | 6480 | 6480 |
| 正常差异 | 0 | 0 |
| 故障观察 | 4536 | 4536 |
| 指定错误检出 | 2196/2196 | 2196/2196 |
| 比较器自检 | 17823 | 17823 |
| 原六项整组精度门槛 | 未通过 | 未通过 |
| 原单 hub 三项精度 | 0/3 | 3/3 |
| 追加合法多 hub 三项精度 | 0/3 | 3/3 |
| 排除 / 跳过 / 归一化 | 0 / 0 / 0 | 0 / 0 / 0 |

旧/新九个 before IR 均逐字节相同。新单 hub 与合法多 hub 六模式各转发 1 次并重定向 1 条 body 入边；原唯一目标 LOAD 与 unknown 入边保留。原 deep 三模式继续不复制，原 whole-six gate 仍未满足，不能报告为九模式都获得新精度。

新矩阵全部 28 个正常/错误流和 expected 流已完成相同的保存证据审计。另用 `precision-audit.py` 独立读保存 IR，确认六条新路径实际经过 entry snapshot PHI、保留 fresh LOAD/freeze、保留 bounds 与 failure，复制的目标 LOAD 消失且结果使用 STORE 值 14；不是仅根据计数器声称收益。旧九模式没有这些路径，原 deep 三模式新旧均无复制。该审计不增加 native 执行计数。

## 源与证据

- producer SHA-256：`1e8d3e2806466a2ed1c7869c4ee97e7dab0f7c36b8781ea5e54fbba88728b394`。
- Python SHA-256：`0c3fcbdb6727d50ed410577736756e5da90171bdf1241986a4565793f4a34fd2`。
- 原有比较器 SHA-256：`af89164eebe5f8fed81093887a38b61bb8a0fe09c657226307773dca8b4c925d`。
- VM：`~/vmc-runs/guest-memory-history-reference-20261111/old-attempt001/report.json` 与 `old-saved-audit.json`。
- 追加版 producer SHA-256：`c03afe7ed2b3723f691b938154914dc9044e837bf366b9f0fcfe4d9bb941cd48`。
- 追加版 Python SHA-256：`92e58dfc1c9602c088383c3c4efa902ae5fae7e3a76c92e46782589ef5c53460`。
- 追加版旧证据：同根目录 `old-expanded001/report.json`、`old-expanded-saved-audit.json`。
- 追加版新证据：同根目录 `new-expanded001/report.json`、`new-expanded-saved-audit.json`，以及 `old-precision-audit.json` / `new-precision-audit.json`。

native 证据归档：VM `~/vmc-runs/guest-memory-history-reference-20261111-complete.tar.gz`；Mac `/Users/c/Desktop/deob/validation-artifacts/guest-memory-history-reference-20261111/`。两机分别验证精确 592 文件集合、逐文件长度和 SHA，payload 为 139792802 字节，结果一致。归档 SHA-256 为 `ad93824ca07f2e9bd0b0a3b254826b39bc54c5f4b6df15ede7f12a62a6f1494c`。归档保留首版、追加旧版、新版全部源快照、producer、IR、原生测试库、完整状态流、日志、报告和审计；借用的 Dragon backend 只记录绑定路径/SHA，由主任务冻结保管，不重复收录。

后续核查发现两个验证任务共用了 reference 根；此包另含当时 real 的 135 个原始文件，
但没有最终 real 审计与源码绑定，不能由此包声明 real 验证完成。全 reference 的最终
完成结果使用 [真实验证记录](guest-memory-history-real-validation-20261111.md) 中的
`31045d…` 组合包（621 文件）；native 原始流与本包完全相同。双端核验后，另按
三个 native 子目录的精确集合、长度和 SHA 复验 423 文件，才退役重复展开目录；
保留 `RETAINED-SUMMARY.json`、原整组六例精度 false 及 `ARCHIVED-RAW-DIRECTORIES.txt`。

本轮没有新增机器指令、IGNIL 或全语料验证；仅使用宿主兼容 DL64。真实样本收益由主任务单独测量。本夹具不证明一般循环不动点、未知内存不变性、非 PHI 普通跨未来定义、后续块 self/future PHI 或任意调用语义。
