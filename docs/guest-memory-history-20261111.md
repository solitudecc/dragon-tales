# 跨循环历史地址的逐边内存转发

实现提交 `c958da9`，接续合流顺序修复 `0eb61de` / 验证 `68c737a`。
本轮仍在同一函数内工作，没有增加调用入口假设、表内容假设或循环不动点。

## 修复的情况

前驱 STORE 使用上一次执行的 frame/index，随后通过共享 PHI hub 回到定义这些值的块。
原 checked-memory 路径拒绝所有此类“定义位于将复制区域中”的输入，即使该定义已经
支配 STORE 所在的原前驱。真实 f-combo O0 包含这种历史地址关系。

现在只允许首块选中 PHI 导入已支配原前驱的历史定义，先在独立入口块创建单入边
SSA 快照，再复制本次执行的块。快照保存 SSA 值，不读取或复制 Guest 内存。
新执行仍重新计算地址、执行 LOAD/freeze 和 bounds 检查；未知路径及原 LOAD 保留。
首 PHI 跨入历史视图后，不再按本次入边展开旧 PHI；地址证明分别维护 fresh/history
访问集合，只匹配已有 SSA 值及精确 Add/Sub 结构。带 poison 标志、新观察、错误
地址、可能写内存的 hub 和非首块自/未来 PHI 仍拒绝。

PHITransAddr 继续用于无快照的既有路径。快照路径使用上述执行视图，避免把两次
执行混为一次。SSAUpdater 修复原块、循环和块外使用；新增 snapshot 块、PHI、分支
都计入既定块/指令/复制/repair 上限，没有提高预算。定义在 corridor 内的 STORE
值仍保守拒绝。本轮没有放开“hub 必须有至少两个前驱”的既有规则。

具体真实块和值见 [阻塞诊断](guest-memory-history-diagnosis-20261111.md)。

## 回归与独立执行验证

| 检查 | 结果 |
|---|---|
| 相同最终三例在旧 2e50 库执行 | 81 assertions，2 cases / 14 assertions 失败 |
| 新历史回归 | 3 cases / 217 assertions 通过 |
| main / no-Z3 相关组件 | 各 87 cases / 3777 assertions 通过 |
| main CTest | 1018/1018，97.21 秒 |
| no-Z3 CTest | 883/883，85.64 秒 |
| 独立主构建原生 old/new | 各 6480 完整状态匹配、2196/2196 指定错误检出 |
| 源码绑定 | 本地/VM 389 个构建源码、41 个验证依赖文件全部 SHA 一致 |

旧测试使用已有 private API，可以编译。第一次单独编译因 Catch include 路径错误
未执行测试；修正到既有 CMake 指定的只读依赖路径后才得到上述实际旧断言失败。
两个构建日志和旧执行日志分开保存，不把第一次编译错误当作回归成功。

独立 native 比较完整返回值、寄存器、Guest 内存和七字段 Exit；没有排除、跳过
或归一化。初始冻结六模式中的三个 deep 夹具包含单前驱 hub，不满足原有准入规则。
原夹具、原模型、原整组精度门槛全部保留，原 whole-six precision 仍 false。
另名追加三个合法多 hub 正例，在旧、新库上同源执行并独立验收；单 hub 三例和
新增合法多 hub 三例均实际转发目标 LOAD，原 deep 三例继续保守拒绝。
不能把这一结果描述为原六例全获精度，或一般深层路径均可处理。
细节及故障对照见 [独立验证](guest-memory-history-independent-validation-20261111.md)。

## 真实收益与下一项

冻结四 ELF 同配方三轮回放：C16/LLVM16 完整 root 匹配，寄存器 16、Guest 内存 16、
Exit 字段 112 个指定错误全部检出。原八个目标 switch 的 case/default/LOAD 审计通过。
f-combo O0 的实际目标 LOAD 索引按轮为 `[] → [90120] → [90120]`，新恢复状态 0
对应的地址；memory forwarding 从旧 `[4,4,0]` 变为 `[4,5,0]`。
其余三个 ELF 的 before/after/每轮 IR 均与旧 2e50 逐字节相同。

f-combo O0 的 CFF switch 从 1 变 2 是上下文副本，原分发器没有消除。
四模块各执行 24 个 pass；全部 12 次 Guest-memory 调用仍 incomplete，仅 f-cff O2
整体流水线收敛。没有重写历史 raw/严格计数门槛的失败记录。

状态 14/34/32 对应的预期索引 90232/90392/90376 没有出现。其 STORE 指针现在分别为
`%state.edge.live4592`、`%state.edge.live4590`、`%state.edge.live4582`，均是 `phi ptr`；
现有 `byteAddress` 只接受直接 GEP，仍保守拒绝。这和 f-cff 的剩余地址关系属于同类
后续组件：按选中入边证明 pointer PHI 的 Guest base/index，不能直接剥掉 PHI。
详见 [真实验证](guest-memory-history-real-validation-20261111.md)。

已知索引仍不等于已知表项值；表读取的内容证明边界及现有 API 风险另见
[表内容证明评估](guest-table-content-proof-review-20261111.md)。

## 构建与证据边界

主 backend：`1b84e712011ef8bd19583924cdb34d8b06eadbdfd23e60e152a569c1d5d4fa5b`；
shim：`e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec`。
无 Z3 backend：`11107f6cc1017c0da740c69dddb83fd666e144e3afb71d7e590603d8c41494e1`；
shim：`d4c4319f2741a347a16e5bed8d1a30ccf85c52cd2f526e5a6efc9c015d846772`。

根目录 `~/vmc-runs/guest-memory-history-root-20261111/` 保存新旧测试、运行库分卷和
源码绑定；本地副本在 `/Users/c/Desktop/deob/validation-artifacts/guest-memory-history-20261111/`。
完整根证据采用分卷：`guest-memory-history-root-20261111-metadata.tar.gz` 包含
80 个元数据/源码快照/测试产物文件，SHA
`6a62bf5a804f6b6a3a99ad1c09a9238b41359fe85780e2182f95c43f3896cb89`；
另有 `join-runtime-{main,noz3}` 与 `history-runtime-{main,noz3}` 中八个 `.so.gz`
运行库分卷。`RUNTIME-PARTS.json` 固定每卷长度、压缩 SHA 和解压后的 SHA。
两机均核对完整集合与内容；Mac 的 join 分卷在 `join/`，历史分卷和元数据在 `final/`。
元数据压缩包本身不重复包含运行库，恢复时需保留全部分卷及所引用的旧 2e50 档案。
没有新跑 69 输入语料、单指令或 IGNIL campaign；这次修改 LLVM 分析，验证范围为
相关组件、完整 CTest、独立循环字节模型和上述冻结真实 root。没有新增跨调用、堆、VM
能力，也不宣称完整去平坦化或全框架确定性。
