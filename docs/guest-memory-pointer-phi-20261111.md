# Guest STORE 指针 PHI 的逐边地址证明

本阶段以 `06542e7` 为源码基线，扩展 checked-memory edge forwarding：当最近的简单 STORE 使用指针 PHI，而待转发 LOAD 仍使用既有受支持的 Guest 字节 GEP 时，可以逐条入边证明两者访问同一地址。不能证明就保留原 LOAD 和控制流。此修改不把指针 PHI 直接剥掉，不假定 Guest 表不可变，不加入堆、帧布局或调用无副作用假设。

## 支持范围

- LOAD 地址仍须满足原有 AS0、i8 元素、单个 i64 字节索引及 64 位索引布局规则，使用已识别的 Guest base。
- 新增 STORE 地址形态是 AS0 指针 PHI 图；叶子必须是原有地址规则接受的 GEP，而且 Guest base 必须是同一个 SSA 值。重新 LOAD descriptor 得到的另一 base 即使运行时可能相等，也不据此证明等价。
- 索引允许无危险标志的 i64 Add/Sub 常量偏移。偏移使用 `uint64_t` 模运算，包括负偏移和 `INT64_MIN`，不依赖宿主有符号溢出。
- 保留最近 STORE、值类型、值已定义、简单非 volatile/atomic 访问和原 corridor 的所有检查。未知调用或可能别名写发生在最终 STORE 之后，仍阻断证明；之前的调用不能让最终 STORE 之后的内存事实失效。
- STORE 值本身定义在将复制的 corridor 内，首版仍拒绝。目标 LOAD 块必须符合原只读 corridor 规则；包含输出 STORE 或 RET 的块不因本功能获得可复制资格。

## 历史视图与逐边证明

先把待 LOAD 索引规范化为“已执行 SSA term + 模 64 位偏移”。在 corridor 内沿本次选中边翻译 PHI；跨越入口 PHI 后切换到旧执行视图。规范化缓存包含 SSA 值与 fresh/historical 标志。新执行的 LOAD、freeze 或不支持的表达式不能冒充历史 term，旧快照也不会代替新执行。

进入指针关系图前，要求相关定义在源前驱可用，并检查可见的 undef、poison 和危险标志。相同 SSA 名称本身不是已定义证明。历史 LOAD/freeze 的既有 SSA 边界规则保持不变。

关系图的节点是 `(pointer, historical term, offset)`，证明对象只服务于当前 corridor 和选中前驱：

1. 遇到 GEP 叶子，验证确切 base 身份、索引依赖与规范化索引相等。
2. 遇到指针 PHI，如果索引 term 是同块整数 PHI，则按**同一前驱块**配对输入。入边数量须一致，不接受重复前驱；不能按叶子集合相同推断逐边相同。
3. 未形成同块配对时，索引 term 必须支配相应入边；指针与配对索引的每个 incoming 定义也必须在该入边可用。
4. 所有分支必须成立。已访问关系仅表示待完成义务，不能视为证明成功。图构建后从合法 GEP 锚点反向传播，要求每个节点都能到达锚点。因此，根的另一条合法分支不能掩盖无锚点的子循环。

依赖缓存和关系缓存不跨候选、前驱或复制复用。任一依赖检查失败就终止整份证明，失败遍历留下的 visited 集合不能被当作成功证书。克隆及 live-out 修复继续使用既有历史快照逻辑。

## 三阶段调度

v1 在原 direct 搜索中立即尝试指针 PHI，真实 f-cff O0 出现精度回退：新候选占用了每个原 LOAD 仅一次的复制机会，导致原有 hub 候选未执行。完整状态匹配不能抵消这一精度回退。

v2 顺序为：

1. 全模块原有 direct GEP 搜索。
2. 全模块原有 expanded-hub GEP 搜索。
3. 仅对前两阶段都未成功复制的原 LOAD，执行 pointer-only 的 direct/hub 搜索。

新阶段不能兑现直接 GEP 候选；原有候选不被新指针证明抢占。候选只在最初快照计数一次，新生成的 LOAD 不进入本轮队列，每个原 LOAD 至多复制一次。所有阶段共享原模块预算，并在执行时重建当前 dominator tree 与证明。原私有单函数 wrapper 仍顺序执行完整三阶段；模块入口使用队列保证跨函数优先级。

## 预算与边界

没有提高现有候选、入边、索引节点、深度、复制或 SSA 修复预算。关系构建、规范化与依赖检查使用既有 proof 预算；缓存只避免重复检查已经访问过的同一义务。达到预算仍报告 incomplete/budgetExhausted，不能宣称整个函数已去平坦化。

本功能不包含一般循环不动点、任意指针算术、跨不同 Guest base 的别名等价、表内容常量化、未知调用摘要或特殊 VM 分析。复杂地址和大依赖图仍可能保守拒绝。保留原 LOAD、边界检查、未知入边及失败路径；新增实际索引覆盖须通过真实 IR 审计验证，不能用复制数代替去混淆成果。

## 已确认回归结果

- 新地址证明组在旧 baseline main/no-Z3 各实际执行 5 cases、169 assertions：3 cases 失败、12 assertions 失败；IR 验证无失败。
- 新排序组使用原 `runGuestMemorySSA`，旧 baseline 2 cases / 14 assertions 全过；v1 两个 case 均失败，共 4/14 assertions 失败，分别捕获同 LOAD 抢占与跨函数 rewrite 预算抢占。
- v2 main 聚焦组 7 cases / 215 assertions 全过；main/no-Z3 相关组各 94 cases / 3992 assertions 全过。
- 最终完整 CTest：main 1025/1025，no-Z3 890/890。391 份构建源文件在 Mac/VM 逐项 SHA 一致。
- v2 backend：main `fadd2d5c1b12c7db913d2279255799f6195926799582e3c46d9b1eceb2136e30`，no-Z3 `a3ec2426ffc8c6ac0c71c69ef568fb5b709e62b90e0593df8db04d0f1c979a77`。

实现里程碑为 `3d7c564`。独立验证见 [native 记录](guest-pointer-phi-native-validation-20261111.md) 与 [四 ELF 记录](guest-pointer-phi-real-validation-20261111.md)：最终旧/新各 6048 次完整状态匹配、796 个指定故障检出；真实 C16/LLVM16、144 个指定字段控制及原八个 switch 契约通过。三轮配方内原有逐轮索引保留，组合 O0 新增状态 32 的实际表 LOAD 索引 90376。状态 14、34 未消费，全部 Guest-memory 调用仍报告 incomplete；这不是完整去平坦化。

原四个 native 正例因为 LOAD 块包含输出 STORE/RET，未满足旧 corridor 支持域，精度门槛始终为 false；追加独立 suffix 正例和历史 SCC 获得收益。原用例、门槛和失败原样保存，不能把新的成功回写成原门槛通过。

主任务证据在 VM `~/vmc-runs/guest-pointer-phi-root-20261111/`，Mac `/Users/c/Desktop/deob/validation-artifacts/guest-pointer-phi-root-20261111/`。`metadata.tar.gz`（VM 文件名 `guest-pointer-phi-root-20261111-metadata.tar.gz`）SHA-256 为 `f2688956f701d503dee2fdd0f80f1ba05363dc88978e546a3cbc29c2b1e513f1`：516 份元数据/源码/测试产物、13552701 字节，两端精确文件集合、大小和 SHA 全部核验。另有 9 个 runtime 压缩分卷共 350217832 字节，`RUNTIME-PARTS.json` 记录压缩和原始 SHA，`RUNTIME-GROUPS.json` 绑定 baseline/v1/v2 的 main/no-Z3 backend 与 shim。两端均核对每份压缩及展开哈希；元数据包本身不包含这些 runtime 字节。

391 份源码和 44 份验证依赖逐项 Mac/VM 对齐，源码/验证依赖完整快照已纳入元数据包。原 baseline 和 v1 展开 runtime 在双端分卷验证后退役，可按目录内 `ARCHIVED.txt` 恢复。中间 v1 全量测试通过但真实精度门槛失败的记录完整保留，说明仅看语义匹配或 CTest 不足以确认分析收益。历史空间归档另见 [storage 记录](guest-pointer-phi-storage-20261111.md)。
