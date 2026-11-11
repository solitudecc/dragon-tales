# STORE pointer PHI 独立原生验证

基线代码 `06542e7`，旧 backend 为历史 v1 的 `1b84e712011ef8bd19583924cdb34d8b06eadbdfd23e60e152a569c1d5d4fa5b`。验证只调用 GuestMemorySSA，不修改 Graph、默认流水线或外部函数原型恢复策略。

## 夹具与输入契约

`guest_pointer_phi_native.cpp` 直接生成 LLVM IR，真正的 STORE 使用 pointer PHI；被转发的 LOAD 仍使用 Guest base 上的直接 GEP，与本轮生产证明范围一致。`guest_pointer_phi_diff.py` 用独立顺序模型计算完整字节状态，不读取重写后的 IR。

12 个模式覆盖：

- `different` / `same`：source join 的 pointer/frame PHI 按同一前驱成对选择，两个分支的地址分别不同或相同；LOAD 前还有独立 unknown 入边。
- `alias_before` / `alias_after`：可能别名的指针写分别在目标 STORE 前后；实际输入包含重叠与不重叠地址。
- `call_before` / `call_after`：未知调用分别在目标 STORE 前后。真实 C helper 改写所选槽、寄存器字节、另一 Guest 字节并增加调用次数，全部参与比较。
- `base_reload`：两次独立加载同一个运行时 Guest base，证明不能只凭本次执行值相同推断 SSA 等价。
- `different_base`：STORE 使用另一个传入 base；输入既包含实际别名，也包含 base 偏移 64 的情况。
- `uncorrelated`：pointer/frame PHI 的 left/right 配对交叉，不能任取同 base 的叶子充当地址相等证明。
- `history`：历史 H/S 两个 pointer PHI 构成 SCC，具有两个不同入口 GEP 锚与 side 回入；STORE 使用旧 pointer PHI，LOAD 继续用 fresh frame GEP。输出保留历史/fresh 值和跨块 live-out。
- `history_mixed`：循环某个入口的 pointer 使用 Guest+64 的 base，frame 未相应变化。
- `history_noanchor`：循环指针叶子使用 ptrtoint/add/inttoptr，实际运行指针合法，但没有本证明域认可的 GEP 锚。

初版为 `different/same/alias_before/call_before` 四个普通模式设了显式精度门槛；v1 验证后确认其 LOAD 块包含输出 STORE 和 ret，不满足既有只读、branch 终结的 corridor 域。这是夹具的精度域预设错误，不是 pointer 证明失败。原四项门槛、完整输入和故障控制均保留，不追改为通过。七个负例必须不重定向。`history` 的收益单独记录，冻结前没有承诺它必然获得收益，也没有因此跳过其语义比较。

另有两项不执行原生函数的结构控制：包含 undef 入边的可达历史 pointer SCC，以及没有入口的不可达纯闭合 pointer SCC。前者检查拒绝不确定指针，后者检查合法 IR 的稳健处理；不可达控制不是对可达循环锚证明的单独证据。没有为 undef 构造伪造的期望指针，它们不计入 native 输入或跳过数量。

每个 native 模式：3 个路径 × 3 个 bounds 模式 × 8 个边界值，共 72 输入；总计 864。边界值包括 0/1/2/3、32/64 位符号位、最大有符号值和全 1。历史循环次数由低两位给出，保证有限。宿主分配为完整 64 KiB，descriptor.size 控制目标 LOAD 的被测检查；辅助观测和源 STORE 是直接 LLVM 夹具定义的操作，不冒充完整 frontend 的所有内存检查。

比较返回值、调用计数、512 字节寄存器、完整 64 KiB Guest、全部 7 个 Exit 字段，并检查 descriptor.data/size 不变。实际别名和不同 base 都指向本次完整 Guest 分配中的合法字节，不归一化宿主地址，不比较未定义/并发行为。

## 故障控制与旧版结果

六个故意错误：错选 pointer PHI 入边、删除目标 STORE、删除 STORE 后的别名写、删除 STORE 后未知调用、历史 STORE 错用入口旧 pointer、删除 bounds。各错误独立定义完整期望快照，并预先给出必须不同的输入集合；不能只检查“至少出现一次差异”。

旧 1b84 同源结果：

| 项目 | 结果 |
|---|---:|
| O0/O2 before/after 完整状态比较 | 3456 |
| 正常差异 | 0 |
| 故障观察 | 864 |
| 指定错误检出 | 412/412 |
| 按故障类别检出 | 48 / 96 / 96 / 96 / 28 / 48 |
| 比较器字段与长度自检 | 9507 |
| 排除 / 跳过 / 归一化 | 0 / 0 / 0 |

12 模式旧版均为 loadsForwarded=0、redirected=0、原 target_loads=1。四项正例精度门槛未通过，负例拒绝门槛通过，历史模式未获得收益。两个结构控制均通过 IR verify 且 0 转发。

全部 16 个正常/错误保存流及 1 个 expected 流另行审计：逐字节比对冻结模型，检查完整输入覆盖、每条报告的字段/快照 SHA，以及所有 artifact/source SHA。保存流审计不另算新的执行样本。

## v1 结果与保留的门槛失败

v1 backend 为 `5c4d8da8d9d16d174c2fc6b726591e5c289c88bf06545925090b13a926312509`。初版同源矩阵 3456 次完整状态比较仍为 0 差异，412/412 指定错误检出；所有负例及两个结构控制保持拒绝，history 首次出现 1 次实际转发。原普通四项均 0 转发，因此 `--require-gains` 以 exit 1 结束，`ok=false / precision_ok=false` 原样留存于 `new-attempt001`。独立语义保存流审计通过不等于这个精度门槛通过。

## 追加合法 suffix 组及最终同源结果

新增另名 `_suffix` 九个模式：四个相应正例与五个相应别名/调用/base/交叉配对负例。只有追加版把 LOAD 块保持为 load、pure computation、branch，将寄存器/Guest/Exit 输出 STORE 与 ret 放到独立 output 后继。这样，负例的拒绝也能实际进入地址/效果证明，不再只因输出副作用提前停止。原十二模式不变。

新增五种对应 suffix 故障变体，原六种错误控制不变。最终矩阵为 1512 输入、O0/O2 before/after 共 6048 次完整状态比较；1584 次故障观察，预定 796 个差异；16635 项比较器自检。追加旧版与初版的 12 个 native、2 个结构 before IR 全部逐字节相同，原 4320 条正常/故障报告记录的 key、完整快照 SHA、差异字段完全相同。首次不变性审计误将 before 文件总数写成 12，实际包含另两份结构 IR；保留该审计源码与说明，修正计数为 14 后，原字节/快照比较未放宽并全部通过。

最终 v2 backend 为 `fadd2d5c1b12c7db913d2279255799f6195926799582e3c46d9b1eceb2136e30`。v1 不运行追加组，追加组只作旧 1b84 / 最终 v2 的同源比较：

| 项目 | 旧 1b84 | 最终 v2 |
|---|---:|---:|
| 完整状态比较 | 6048 | 6048 |
| 正常差异 | 0 | 0 |
| 故障观察 | 1584 | 1584 |
| 指定错误检出 | 796/796 | 796/796 |
| 比较器自检 | 16635 | 16635 |
| 原四项错误域精度门槛 | false | false |
| 追加 suffix 四项精度 | 0/4 | 4/4 |
| 历史 pointer SCC 收益 | false | true |
| 原/追加拒绝门槛 | true | true |
| 排除 / 跳过 / 归一化 | 0 / 0 / 0 | 0 / 0 / 0 |

v2 四个合法 suffix 正例与 history 各 forward 1、redirect 1、保留原唯一目标 LOAD；其余模式为 0 转发。`--require-suffix-gains` 只约束新增四项，原 `precision_ok=false` 独立保留，没有报告原普通四项已获得精度。history 的收益来自实际测量，不事后加入既有四项门槛。

旧/最终版冻结五份源完全相同，21 份 native 加 2 份结构 before IR 共 23 份逐字节相同。每版 26 个正常/错误保存流和 1 个 expected 流完成独立全字节、覆盖、报告行及 artifact/source SHA 审计；前后运行库 SHA 一致。另以保存 IR 审计实际五条改写路径：STORE 确实用 pointer PHI，复制目标 LOAD 被 50/14 替换，原 LOAD、unknown 入边、bounds/failure 保留；历史 entry snapshot 和 fresh LOAD/freeze 保留。这不是只检查统计计数，也不增加原生执行样本数。

## 固定源与证据

- producer SHA-256：`9296ae4aa8d86fb201788dca63dddea8d62d61f028dc20a128225195c75813c2`。
- Python 模型 SHA-256：`8e9fe729d824dc223662f78656c031fa8d9f35fb41a6f708507e969566222330`。
- 追加版 producer SHA-256：`7c50486cbce5805bb44ae2eeec4a98051028ac3b86f213c2bf217ae9b5790338`。
- 追加版 Python SHA-256：`b87f06532e1b7abad4fb222fb464d7664973ff349170bc24e3050f4b38b86f8b`。
- 本轮冻结五份源，包含复用的 history producer、history 初始字节辅助与 checked-memory 比较器。完整 SHA 在每次 report 的 `source_sha256` 中。
- 专属 VM 根：`~/vmc-runs/guest-pointer-phi-native-20261111/`；旧结果 `old-attempt001/report.json`、`old-saved-audit.json`。未与真实 ELF 组共享目录。
- v1 原组失败结果：`new-attempt001/report.json`；其语义保存流审计为 `v1-semantic-saved-audit.json`。
- 追加同源组：`old-suffix001` / `new-suffix-v2`，各自 report 与 `*-suffix-saved-audit.json`；独立实际 IR 审计为 `old-precision-audit.json`、`v1-precision-audit.json`、`v2-precision-audit.json`。

完整独立归档为 VM `~/vmc-runs/guest-pointer-phi-native-20261111-complete.tar.gz`，Mac 副本在 `/Users/c/Desktop/deob/validation-artifacts/guest-pointer-phi-native-20261111/`。两机分别逐一核对精确 642 文件集合、文件大小及 SHA，payload 44183051 字节，结果一致。归档 SHA-256：`228adb2701f7be617091c41ebe83a51ddf00ba3a1bcd943cda3e55a4a8f2a836`；清单 SHA-256：`94e4e424ef5786ab092fb2358d71b5b6b04476b75ddc92914c3558db85b25704`。

归档包含原旧版、原 v1 失败门槛、追加旧版、追加最终 v2 的全部源快照、producer、LLVM IR、原生测试库、压缩完整状态流、日志、报告与独立审计。原始展开数据未删除；借用的 Dragon backend 不重复收录，由主任务冻结保管，report 记录其路径与 SHA。本目录没有混入真实 ELF 组。

这不是新指令、IGNIL、非宿主布局或全语料验证；只覆盖宿主兼容 DL64 的有界 LLVM 变换。真实 ELF 收益与完整 CTest 由主任务另行记录。
