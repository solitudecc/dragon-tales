# Guest checked-edge 的有界原边工作队列

源码基线为 `2294900`。上一阶段的逐边地址证明已经能处理指针 PHI，但每次 Guest-memory 调用对一个原 LOAD 只复制一条已证明入边。真实 combo O0 的 state32 先被处理，state14/34 对应的原 STORE 入边仍留在同一 LOAD 的搜索范围内。本阶段扩展调度，不扩展地址、内存或调用证明规则，也不提高外层三轮流水线或原有预算。

## 显式启用与兼容性

通过 builtin `guest-memory` 的数值选项 `edge_worklist=1` 启用。默认值和显式 `0` 均保持原来的每原 LOAD 一次行为；未启用时不创建原边快照或额外队列。仅接受整数 0/1，布尔、字符串、负数或其他整数在 pipeline.add 阶段被拒绝。

C++ 新增重载：

```cpp
lifter.runGuestMemorySSA(options, true);
lifter.runGuestMemorySSA(graph, options, true);
detail::runGuestMemorySSA(module, options, graphPointer, true);
```

保留旧重载符号及默认行为，未扩展 `MemoryForwardOptions` 对象布局。原 C/Python 直接调用接口及默认优化流水线不变；使用已有 PassPipeline 参数通道即可显式请求新选项。

已有 Python 流水线可这样添加该步骤（寄存器提升已由前序步骤执行时）：

```python
pipeline.add("guest-memory", run_mem2reg=0, edge_worklist=1)
```

## 调度与原边身份

先完整执行既有全模块三阶段：direct GEP → expanded-hub GEP → pointer-PHI。额外队列不能抢占这些阶段的证明、复制或 rewrite 预算。

启用时，在每个函数第一次 checked-edge 改写之前冻结：

- 初始候选中的原 LOAD；仍使用原候选计数上限，后续不重新计数。
- 当时的原基本块。
- 每个原块的终结指令指针、successor index 及该位置的原 target。

额外搜索要求终结指令仍属于原块、指针仍相同、对应 successor index 仍有效，且当前 target 等于冻结 target。初轮已重定向的边因此不会被再次消费。corridor 的块及所选连接边也须属于冻结的原图；新克隆 LOAD、块及边不成为新候选。复制体仍可保留当前 CFG 中指向先前克隆的外部 side-exit；这里限制的是候选枚举，而非禁止这些真实控制流边。当前 SSA 修复产生的定义只能通过重新执行现有证明规则来使用，快照不是地址或内存正确性证书。

初始队列按各函数第 0、第 1……个原 LOAD 交错排列。每项重新建立当前 dominator tree、corridor 和地址/历史证明，按原 direct、hub、pointer 次序尝试。成功只消费一条仍存活的原边，随后该 LOAD 排到队尾，让其他函数和其他原 LOAD 有机会执行。失败项本次不重入；首版不实现任意失败依赖的全图不动点。

每次成功都重定向一条不同的冻结原边。边数有限，加上失败不盲重入，因此队列自身有限；它同时受原有候选、入边证明、索引节点、复制数、复制指令、SSA 修复及 maxRewrites 上限约束。预算从初轮到额外阶段共享，不重置，不以增加预算代替解决调度问题。

## 安全与剩余范围

地址、历史入口快照、逐边指针 PHI/SCC、支配、undef/poison、flags、Guest base 身份、调用/别名屏障和 live-out 修复规则不变。目标 LOAD 仍须位于既有允许复制的只读 corridor 中；输出 STORE/RET 不因此变为允许的 corridor 指令。保留未知入边、原 LOAD、边界检查、真实表 LOAD、默认分支和失败出口。

本阶段没有 Guest 表不可变假设，没有跨新克隆的递归扩展，没有一般循环固定点或新调用摘要。原证明/修复上限仍可能让某些边保守失败，并须保留 incomplete/budgetExhausted。若一个失败 LOAD 之后因另一个 LOAD 的 SSA 修复而产生新机会，本轮不保证回访；只有后续显式 pass 调用会重新建立候选。实际索引覆盖须从真实 LOAD/switch 使用关系审计，不能用复制数代替去平坦化完成度。

## 实现来源

本实现只借鉴成熟数据流算法中的工作队列、失效后重建及公平轮转思想，根据本仓库原边身份、历史快照和预算合同独立实现。没有复制 GPL 项目的源码、测试或实现片段，也没有引入新的第三方代码依赖。

## 当前验证边界

test-only 全部调用既有 LLVMLifter/PassPipeline API，通过字符串选项请求新功能。旧 main/no-Z3 各实际执行 5 cases / 59 assertions：4 cases、15 assertions 在 `pipeline.add` 接受新选项处失败；非法参数负例通过。这只证明旧版本不支持该 API 选项，**不是旧代码产生错误语义的证据**。

新 main/no-Z3 库加载同一 test-only 二进制，各 5 cases / 162 assertions 全过。覆盖：direct/hub/pointer 的多个原 STORE 入边，调用/别名后写负边，原三阶段与跨函数轮转优先级，rewrite 上限的 incomplete，显式关闭与旧入口 IR 字节相等，以及参数校验。

最终相关测试 main/no-Z3 各 99 cases / 4154 assertions 全过，完整 CTest 分别 1030/1030、895/895；392 份源码及 49 份 harness 在 Mac/VM 逐项 SHA 对齐。旧 API 拒绝日志、旧单边精度基线和新语义执行结果分别记录，不能合并计数，也不能把新接口不可用说成旧语义失败。

## 最终收益与限制

实现里程碑为 `26386b2`。[独立原生验证](guest-edge-worklist-native-validation-20261111.md)中，新旧各 2280 次完整状态比较一致、776 个指定错误全部检出；四种夹具均从一次消费一条原入边提升到两条，未知调用/别名负边保留。

[真实 ELF 验证](guest-edge-worklist-real-validation-20261111.md)沿用四个输入二进制、外层三轮及 max_pass_runs32。默认模式的 20 份 IR 与基线逐字节一致。启用模式新增 combo O0 的 state14/index90232、state34/index90392 和 state22/index90296，保留 state0、state32 及所有旧逐轮索引；cff O0 的逐轮实际索引覆盖从 1/2/3 提升到 11/16/16。两组各 C16/LLVM16 完整状态匹配及 144 个指定破坏控制通过，没有新增排除、跳过或归一化。

所有 12 次 Guest-memory 调用仍为 incomplete；10/12 次 dispatch 调用仍为 incomplete，所有 `switches_rewritten` 均为 0。这是实际索引证明覆盖的改善，尚未完全去平坦化。最初解析保存文本 IR 的默认兼容性检查因 predecessor use-list 顺序变化失败，失败材料保留；最终使用原始完整 lifting 工作流通过严格字节门槛，没有归一化放宽比较。本阶段没有新增 69 输入全语料库或机器指令验证；它们也不能代替这里的调用/Guest 完整状态验证。

## 根验证归档与存储

根材料在 VM `~/vmc-runs/guest-edge-worklist-root-20261111/` 和 Mac `/Users/c/Desktop/deob/validation-artifacts/guest-edge-worklist-root-20261111/`。`metadata.tar.gz`（VM 邻接名为 `guest-edge-worklist-root-20261111-metadata.tar.gz`）双端逐项核验 488 份 payload 文件，SHA-256 为 `3cad60f72721ba357ab896b100304209a23aa45472d199c4988e8efd8749e33a`。包含源码/harness 快照、旧失败、两种构建完整测试日志及运行脚本。

运行库另存四份无损 gzip，共 118386309 字节，以 `RUNTIME-PARTS.json` 绑定压缩与展开哈希；根 metadata 包不重复包含运行库。`BASELINE-RUNTIME-REFERENCES.json` 引用上一阶段已双端保存的四份基线运行库。新四份及基线四份均核验，不能仅凭 metadata 包宣称运行库完整备份。独立 native/real 完整包及哈希见各自文档。

为控制 VM 空间，仅在完整包双端核验及展开目录逐项匹配后，退役上一阶段 pointer-PHI 两组重复 real 原数据（270 文件）及 runtime-v2 两份原始库；对应归档、压缩库和恢复说明保留。本阶段两组 real 重复展开目录也经相同核验后退役（272 文件）；失败的文本解析试验与外层报告仍保留。具体记录见根 `previous-real-retirement.json`、各原目录 `ARCHIVED` 说明及 real 的 `RETIRED-DUPLICATE-RUNS.json`。
