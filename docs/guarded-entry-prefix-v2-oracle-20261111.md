# Prefix v2 独立审查与首轮执行记录

本轮仅为独立 standalone 原型验证，不是生产功能集成，也不是实际 CFF 去除。

## 冻结输入

- 工具：`guarded_entry_prefix_clone_v2.cpp`，SHA-256
  `e34f759218ca9cbe5d0ae05347c0c9285e0e22d9cd509d845abfbc49d9f46b36`。
- 独立 producer/model/runner：`guarded_entry_prefix_oracle.py`，SHA-256
  `fe30bdbf6d612e923c2cd1329be549a48dd3ba67bed02089d7ba8ee92109ef17`。
- VM 记录：`~/vmc-runs/entry-prefix-prototype-20261111/oracle-v2/`。
- 两个预算均执行：6/256 和 32/2048。它们的结果分开保存。

## 静态审查结论

DFS gray 边按真实 predecessor + successor index 保留为原 CFG 出口，不再按
目标块是否有 clone 来决定其去向。出口 PHI 使用该次执行的 mapped 输入，根
PHI 选真实 seed 输入；非恒定输入不被假定为常量。支配 seed 的历史区域继续
拒绝，普通外部 liveout 继续拒绝。

未发现能够绕过上述限制及 verifier 的具体错误语义。此结论不是语义证明。
对出口回原区域成员、原成员普通使用旧 definition 的形状，部分检查仍依赖
最终 dominance verifier；应保留拒绝，不因为 IR verify 成功就推断任意历史值
安全。guard 名字只是 seed 定位，不代表额外事实假设。

## 原始四模式的首次结果

| 模式 | 6/256 | 32/2048 |
|---|---|---|
| entry_phi | 成功复制并 verify | 成功复制并 verify |
| diamond | 拒绝：普通外部 liveout | 拒绝：普通外部 liveout |
| loop | 成功复制并 verify | 成功复制并 verify |
| fresh | 成功复制并 verify | 成功复制并 verify |

`diamond` 的未使用 latch 无前驱，未被前缀收集，却继续普通使用 header/read
定义。这是既有 liveout 准入边界，不是非法 IR，也不是已确认的实现错误。
原冻结 fixture、输入和精度门槛全部保留；whole4 gate 为 **false**。

O0 和 O2 runner 都在读取不存在的 diamond 变换产物时退出，原生比较数均为
**0**，故障执行数也为 **0**。`results-O0/report.json`、`results-O2/report.json`
和 stderr 保留该原始失败。不能将其它三模式的 verify 成功填进语义成功数。

两个单独结构 controls 均按预期 exit 3，未生成输出：

- historical：region 支配 seed，拒绝历史区域；
- liveout：普通值穿过未知调用边界，拒绝外部普通使用。

源码和工具的前后 SHA 文件一致。原型输出含逐 edge 分类记录，未修改生产库。

## 既有十模式的准入检查

原始 canonical guarded-entry 十模式保持原文，按两个预算分别调用 v2，共
20 次准入检查。只有 `loop` 两次成功，其它 9 模式各两次拒绝，未生成产物。
每个原始 IR 在运行前后 SHA 一致。记录位于 `compat-admission/report.json`
及对应 stderr。

这一步没有进行十模式变换后原生对拍。不能将未修改的 guarded IR 填作变换
结果，也不能称完整十模式兼容通过。若后续单列 loop 实测，必须保留这里的
whole10 准入限制，并明确它是 112 输入子组，不是原完整 1120 输入组。

## 后续执行约束

合法 diamond 若追加，必须另名、另目录、记录派生关系，不能覆盖首次拒绝
产物或修改原 whole4 门槛。任何成功子组的原生结果独立报告。实际样本仍因
普通非 PHI 外部使用受阻；合成子组通过不会改变该事实。

## 另名派生组的最终结果

经 root 授权，新增 `guarded_entry_prefix_oracle_v2.py`，SHA-256
`7ae9bb618359e0fa3e5ec2aaffb83ed2fd86f02d5478a9b0366a45db7f862050`。
只将原 diamond 另名派生为 `diamond_no_dead_latch`：删除不可达 latch 及其
专用 PHI，两个剩余真实入口选相同 SSA 的 PHI 直接使用那个实际入口值。
其余 3 个原 IR 和 4 个错误 IR 逐字节相同；256 个输入按名称映射后，独立
模型产生的完整状态也逐字节相同。原 fe30bd 文件及失败记录没有改动。

新四模式在 6/256 与 32/2048 均成功生成并 verify。原生执行只采用 32/2048，
两者不能混报：

| 验证 | O0 | O2 |
|---|---:|---:|
| before/after 完整状态匹配 | 512 | 512 |
| 错误 IR 完整观察 | 256 | 256 |
| 指定必须不同的错误全部检出 | 82/82 | 82/82 |
| 比较器错误控制 | 2560 | 2560 |
| 差异、排除、跳过 | 0 | 0 |

每级 82 个指定错误包括：错误根 PHI 4、漏 STORE 22、陈旧 LOAD 32、固定
frame 24。四种 control 均实际执行，并与独立错误模型比对；未受错误影响
的输入也完整比较，没有排除。正常门槛要求四个真实 seed 出边均进入 clone，
且 loop/fresh 的复制 latch 必须返回原 header。不是 metadata 缺失推断收益。

新矩阵为 synthetic 四指针 ABI，只验证前缀复制；返回值及完整 73728 字节
arena 全量比较，覆盖寄存器、64 KiB Guest、七个 Exit 字段、config 和 padding。
该矩阵不执行未知调用，calls 字段恒 0，不能声称新增未知调用语义覆盖。

## 原 loop112 的独立兼容子组

`guarded_entry_prefix_loop_compat.py`，SHA-256
`043c23ae5a8a87fa4114afd004d1637c5f099d594c58d799e39d92139b2189e2`，
从未修改的 `0d12c51e...` canonical 1120 输入模型精确选择 loop 的 112 输入。
original、guarded、v2 prefix 三变体各在 O0/O2 实测，合计 **672 次完整状态
匹配，0 差异，2688 个比较器 controls 全检出**。保留原四种别名布局、七种
入口/尺寸变体、四个边界 payload 及原 139272 字节 snapshot 范围。

这里未新增 guard-page 子进程或原错误 IR campaign；它们在前阶段的结果不
计入本阶段。其余九个模式仍按上表拒绝；本组不是 whole10 变换后兼容通过。

## 保存流与证据审计

`guarded_entry_prefix_saved_audit.py`，SHA-256
`e08b71dce1e68ef50812385c72cfeee182083cc88a8079156f85145b3a24de91`，
重新读取全部 **30 条 gzip 流、2208 帧**，逐帧核对独立模型、完整长度、顺序
和末尾无额外数据；检查原失败组 native=0、派生源的 256 输入等价、未变的
7 个 IR、前后工具/源 pins、两个结构拒绝及 canonical 20 次准入记录。
审计通过，增加原生执行数 **0**。`saved-audit.json` 保存每流压缩与原文 SHA。

VM 范围仍为 `oracle-v2/`，所有原失败、两个 O 级、合法派生组和 loop112
子组独立保存。v3 的真实 terminal-leaf pilot 是另一个工具和范围；这里的
v2 synthetic 成功不能作为 v3 新增域或实际 CFF 去除的验证结论。
