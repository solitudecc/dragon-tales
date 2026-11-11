# Prefix v3：独立原生语义验证

范围是 standalone v3 原型，不是生产 pass，不包含真实 Tigress root 的原生
执行。生产事务提交和真实样本的结果必须另外报告。

工具源 SHA-256：
`ea5e87759f034d7d34e8009e1022cca47f16bec10f15e83c5fb3c3f15aa9169a`。
VM 独立目录：`~/vmc-runs/entry-prefix-prototype-20261111/oracle-v3/`。
前阶段 `oracle-v2`、原 fe30bd 四模式 gate 失败以及原 canonical 九模式拒绝
结论均未修改。

## v3 新增域的静态审查

只将 switch 直接通往的 RET leaf 纳入区域，并要求该 leaf 的全部原始 incoming
都来自同一 switch source block；同一 source 的重复 successor 槽允许。
EH、address-taken、alloca、token 和 call 仍被拒绝；新 leaf 计入同一预算，
没有额外预算池。其他 switch 边和 DFS gray 回入边仍指向原 CFG。

实现复用既有 operand、dominance、PHI 和 edge 修补规则；没有放宽历史区域
支配 seed 的拒绝，也没有通用跨区域 liveout 修复。静态审查未发现具体正确性
阻塞，之后才执行下列独立原生验证。

## 冻结同输入复测

四模式 producer/model 仍是 `guarded_entry_prefix_oracle_v2.py` 的
`7ae9bb618359e0fa3e5ec2aaffb83ed2fd86f02d5478a9b0366a45db7f862050`。
全部原始 IR、256 输入和四种故障 IR 与 v2 逐字节相同。

- 两预算 6/256、32/2048 均生成合法 IR；原生执行仅采用 32/2048。
- O0/O2 各 512 次正常全状态匹配，共 1024 次、0 差异。
- 每级 256 次故障观察、82 个指定错误全检出，共 512 / 164。
- 每级 2560 比较器 controls 全检出，共 5120。
- 实际 seed 出边进入 clone，loop/fresh 的复制 latch 返回原 header。

原 canonical guarded-entry 模型 `0d12c51e...` 未修改。精确选择原 1120 输入
中的 loop112 子组，对 original、guarded、v3 prefix 三变体分别 O0/O2 执行，
共 **672 次完整状态匹配，0 差异，2688 比较器 controls 全检出**。该子组仍
比较原 139272 字节 snapshot，包括四种别名布局、七种入口/尺寸变体和四个
边界 payload。没有将其它九个拒绝模式填作“变换后兼容成功”。

## 新增 terminal leaf 的独立模型

源码 `guarded_entry_terminal_oracle.py`，SHA-256：
`eb13c260ca10e2f32ceb49a3c07b747f810903caec4b17153adf051371b1dbde`。

两个模式共 32 输入。header 先修改 Guest 单元，再实际 LOAD/freeze 新值，
与非恒定 seed 相加得到 selector。`terminal_default` 的未知 default 返回
完整错误 Exit；`terminal_duplicate` 的两个 switch successor 槽进入同一个
RET leaf，PHI 按重复 incoming 数量保存。结果、寄存器、Guest 副作用及全部
七个 Exit 字段都依赖真实 selector。短尺寸路径在任何对应 Guest 访问前返回。

独立顺序字节模型不读取变换后的 IR。输入为 x=0、1、最高位、全 1，守卫
hit/miss 和短/完整尺寸。比较 i64 返回值及完整 73728 字节 arena，包含寄存器、
64 KiB Guest、Exit、config 与 padding。无调用，calls 字段恒为 0；不声称
本矩阵覆盖未知调用效果。

| 检查 | O0 | O2 |
|---|---:|---:|
| before/after 正常全状态匹配 | 64 | 64 |
| 错误 IR 完整观察 | 48 | 48 |
| 错误 default 值捕获 | 7/7 | 7/7 |
| 漏 STORE 捕获 | 8/8 | 8/8 |
| 错误重复 PHI 值捕获 | 2/2 | 2/2 |
| 比较器 controls（含全部 Exit 字段） | 640 | 640 |
| 差异、跳过、排除 | 0 | 0 |

错误 IR 均实际执行，并与对应独立错误模型逐字节比较；故障未影响的输入也
完整比较。没有把编译错误、verify 拒绝或超时计为错值捕获。

实际精度门槛要求 seed 重定向到复制 header，switch 的 default 和 hit 槽均
指向复制 leaf，重复槽数量正确。两预算均成功；原生执行仅跑 32/2048。
同一正例在旧 v2 都因外部普通 liveout 拒绝（exit 3、无输出），不是旧 native
错误。两个负例也按预期拒绝：共享 leaf 具有第二个 source predecessor；同一
正例只给 1 块预算，不能完整覆盖 liveout。没有通过放宽准入追绿。

## 汇总与保存流审计

本轮总计 **1824 次正常完整状态匹配**，**608 次故障观察、198 个指定故障
全部检出**，**9088 个比较器 controls**；差异、排除和跳过均为 0。这些数字
只来自本轮 v3 的执行，不累加 v2 或此前 guarded-entry campaign。

`guarded_entry_prefix_v3_saved_audit.py` 独立重新读取全部 **44 条 gzip 流、
2432 帧**，核对独立模型、顺序、长度和无额外尾部；检查 unchanged IR、固定
模型 SHA、工具/源前后 SHA，以及旧 v2 拒绝与两个负例无输出。审计增加原生
执行数 0。

保存流审计 v1 曾将含路径的 SHA 文件整行比较：before 路径为
`terminal/guarded_entry_terminal_oracle.py`，after 为 basename，实际 SHA 相同。
失败脚本、stderr、exit 均保留。新版按 SHA+basename 比较，且模块加载仍验证
固定完整 SHA；没有修改输入、模型、状态比较范围或精度门槛。最终全部通过。

这些结果验证 standalone v3 的上述形状；真实 first-target 的结构化简不能
因为本报告而自动成为真实 root 原生通过，生产模块身份/生命周期也需另验。
