# Guest checked-memory 原入边工作队列的独立原生验证

此记录验证同一原始 LOAD 的多条真实 STORE 入边，不把复制次数等同于去平坦化完成。生产证明规则、Guest 入口内容和未知调用语义均不增加假设。旧入口与显式新开关是不同 API 调用；测试不声称旧库支持新开关。

## 冻结夹具及契约

生产实现之外的 LLVM producer 为 `scripts/validation/guest_edge_worklist_native.cpp`，顺序字节数组参考模型为 `scripts/validation/guest_edge_worklist_diff.py`。producer 复用既有历史夹具的 IR 构建辅助函数；参考模型独立执行写入、调用、循环和退出行为，不从优化 IR 读取预期值。

四个具名模式：

- `different`：同一 checked LOAD 的两条正入边分别 STORE 14、34，路径选择不同地址。
- `same`：两条正入边写同一地址但值不同，防止把某条边的值用于全部路径。
- `hub`：上述多入边先通过合法的共享 hub，再到有独立未知前驱的 checked block。
- `history`：两条循环 latch 分别 STORE 14、34，保留 fresh Guest LOAD、freeze、历史 PHI、live-out 和 side 路径回到原始共享块。循环上界为输入的低两位，有限且覆盖 0–3 次。

前三种夹具另保留 STORE 后可能别名写、STORE 后未知调用以及无 STORE 的负入边。未知 helper 的实现会写寄存器、Guest 字节和目标槽；分析器未获其效果摘要。别名参数既有与目标重合也有不同地址的实际输入，但没有 noalias 属性。负边均不得被当作已证明 STORE 值路径改写。

目标 LOAD 所在块只含读取、纯计算及 branch；输出 STORE 和 ret 位于独立后继，符合原有可复制 corridor 的准入要求。所有原始目标 LOAD 和未知/失败路径保留。冻结的精度门槛要求每个模式在一次调用中消费两条已知原入边，至少转发两次，原始目标 LOAD 恰保留一个。该门槛独立于完整语义比较。

## 输入与比较范围

570 个冻结输入，覆盖路径、短一字节/恰好可读/完整 Guest 范围，以及 0、1、2、3、8/16/32/64 位符号边界、最大有符号数、全 1。实际 host Guest 始终分配 64 KiB，描述符边界独立控制；没有越过 host allocation 或未定义输入。寄存器和 Guest 为独立分配。

每个输入比较 before/after IR 各自以 Clang O0、O2 编译后的完整快照：返回值、调用次数、512 字节寄存器、64 KiB Guest、七个 Exit 字段，共 66120 字节。每组为 2280 次正常执行比较。

九种故意错误控制包括：把第二条边的 STORE 34 改为 14、删 STORE、删别名写、删调用、绕过边界、混用 fresh/history、使用陈旧 fresh、破坏重入 frame 更新、破坏 live-out。共 2220 次控制执行，其中 776 个预先由参考模型指定为必须产生差异；其余输入仍完整执行且验证故障模型结果。每个故障快照必须与独立故障模型逐字节相等，不能只证明“有某个差异”。另有 6273 个比较器字段/长度控制。

正常、故障、预期快照均以 gzip 完整保存。`guest_edge_worklist_saved_audit.py` 独立重读全部 22 条执行流及预期流，复验输入顺序、字段差异、快照哈希、所有产物及冻结源码哈希。这属于证据审计，不新增原生执行计数。排除、跳过、归一化均为零。

## 旧版本结果

基线 backend SHA-256 为 `fadd2d5c1b12c7db913d2279255799f6195926799582e3c46d9b1eceb2136e30`。使用旧二参 `runGuestMemorySSA(module, options)`；新执行将使用同源编译宏选择四参 `(module, options, nullptr, true)`。模型及 IR 构建不受宏影响，必须复验 before IR 字节相同。

旧版本四种模式均仅消费一条正入边（forwarded=1），负边全部保留，原目标 LOAD 数均为 1，`complete=1`、预算耗尽数 0。因此冻结的“两条原入边”精度门槛为 **false**，这是本轮预期的旧精度失败，并非旧原生语义错误。基线调用没有要求增益，因此兼容性报告的 `ok=true` 与 `precision_ok=false` 同时保留；不把兼容性通过说成精度门槛通过。

旧版本完整状态 2280/2280 一致，故障 776/776 检出，2220 个故障快照全匹配独立故障模型。保存流审计通过。

## 新版本与归档

候选 v1 backend SHA-256 为 `ce87209b4dc79fa7280d6b53e2fa6edb8e77c9f9058e6c42b6be1894494d9186`。四个模式均一次消费两条已知原入边，`forwarded=2`，负边零改写，原始目标 LOAD 数为 1，`complete=1`、预算耗尽数 0；冻结精度门槛 **true**。完整状态 2280/2280 一致，2220 个故障快照匹配独立故障模型，其中 776/776 指定错误检出，6273 个比较器控制通过。

新旧两次使用相同冻结 producer/model：SHA-256 分别为 `a0a3fb64487fe349be7f66a0d1b777912c297134dbcfd8cbeb9ce6a47e380b31`、`756dfeb8a7633d98290b2b485373cef298d70482b99d975cd24243e6da4ffe64`。全部 4 份 before IR、9 份故障 IR、23 条解压快照流在旧新间字节相同。新旧各自保存流审计通过。独立结构复读确认两个复制 LOAD 块实际使用对应的 14/34，而非仅看统计数；各自保留真实 size 检查及 failure 出口。循环的两份新执行继续执行 fresh LOAD/freeze，side 回入和未知 seed 仍进入原 CFG。

独占 VM 目录：`~/vmc-runs/guest-edge-worklist-native-20261111/`，包含 `old-attempt001`、`new-v1`、冻结 harness、逐流审计和旧新配对审计。完整包为邻接的 `guest-edge-worklist-native-20261111-complete.tar.gz`；Mac 副本目录为 `/Users/c/Desktop/deob/validation-artifacts/guest-edge-worklist-native-20261111/`。双端完整 file-set/size/SHA 核验均通过：268 个文件，payload 17348861 字节；archive SHA-256 `ee0342b31c1071b5e4021d8f2c413ef8f0a76b3aaa7ce1ea537aba6cd27b34bb`，manifest SHA-256 `c0976af20a908a1dcad5d691682add880fe5952058af309c770653ea749f3632`。原数据在核验前不退役；backend 由 root 独立冻结归档，不在此包重复保存。

本矩阵只覆盖 host DL64 的上述 Guest IR 域，不替代机器指令/IGNIL、完整语料库、全部非原生布局或真实 ELF 的状态覆盖验证；未知外部调用仍是证明屏障。原生执行正确不等于分析完成，也不保证所有 dispatcher 已去除。
