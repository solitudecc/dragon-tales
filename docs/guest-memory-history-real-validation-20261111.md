# Guest 内存历史入口快照：四个真实 ELF 验证

本次验证固定使用既有 `bounded_memory_pipeline_real.py`（SHA `4782b9c01f7e2cdfb70505b3e048ac5f3a7cf74fd6f65bb8321bdb7f85456c86`）与既有原 switch/索引审计器，不改旧 oracle、比较器、输入或故障控制。

候选 backend 为 `1b84e712011ef8bd19583924cdb34d8b06eadbdfd23e60e152a569c1d5d4fa5b`，shim 为 `e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec`。旧 `2e509ceb…9bda` 基线直接从完整 `bounded-memory-final-20261111.tar.gz`（b058 包）读取已验证产物，未重复执行旧组。

配方、原 oracle request、冻结四 ELF 和 transformed C 的 SHA 与基线相同。七个变换依次为 mem2reg、instcombine、gvn、guest-memory(run_mem2reg=0)、dispatch-specialize(allow_plain_stores=1)、control-flow、dce，加只读 checkpoint；三轮、max_pass_runs=32，每模块实际 24 次 pass。18 个 oracle/ABI 依赖及新 runtime 执行前后 hash 不变；另外保存并核对了生产 TU 和新测试，共 20 份 Mac/VM 同源快照。

## 实际结果

新组在约 80 秒完成 C 16/16、LLVM 16/16 完整 root 对照，16 个寄存器控制、16 个完整 Guest 内存控制及 112 个 Exit 字段错误控制全部检出。没有新增排除、跳过或归一化。所有寄存器字段、完整 1 MiB Guest 和 Exit 字段保持旧 oracle 契约。独立后置审计确认四模块八个原目标 switch 的 case 地址集合、默认 Exit source PC 和 selector LOAD 依赖保留；f-cff O0 原三轮精度门槛也通过。

| 模块 | 每轮实际表 LOAD 索引 | 新增最终索引 | CFF switch 数 |
|---|---|---|---|
| f-cff O0 | {70032} → {69760,70032} → {69736,69760,70032} | 无 | 1→4 |
| f-cff O2 | 原 13 个索引，三轮相同 | 无 | 1→17 |
| f-combo O0 | {} → {90120} → {90120} | **90120，状态 0** | 1→2 |
| f-combo O2 | {73904,73952,74048}，三轮相同 | 无 | 1→4 |

预先识别的组合样本候选常量为状态 14/34/32/0。本次实际满足的只有 **0→90120**；14→90232、34→90392、32→90376 未出现，不能说四个状态全部恢复。新增索引由原 `actual_loads` 检查器从保存 IR 复算，确实由 CFF source PC `0x10adc` 对应 switch 使用。后置 precision gate 要求四个预先候选中至少一个实际新增：旧组失败，新组通过。没有假定 Guest 表内容恒定，也没有将复制 switch 计为去平坦化。

f-combo O0 的 Guest loads_forwarded 从旧 `[4,4,0]` 变为 `[4,5,0]`，dispatch branches_rewritten 从 `[91,2,0]` 变为 `[91,3,0]`。四个 before IR 字节相同；三个其他 ELF 的 before/after/三轮 IR 全相同；combo O0 仅第 2、3 轮及最终 IR 改变，第 1 轮仍相同。全模块计数不能当作新增 CFF 状态数量。

新增候选集合来自实现前只读诊断；“至少一个实际新增”的后置检查是在结果审计阶段
补入 review 脚本的，不是冻结原 oracle 返回码原已包含的条件。原有完整状态、故障、
f-cff 三轮和原 switch 门槛都独立保留，未因其余三个候选未成功而改写这些检查。

仍然存在明确限制：12 次 Guest-memory 调用全部 incomplete；dispatch-specialize 10 次 incomplete，仅 f-cff O2 后两轮 success。只有 f-cff O2 整体 pipeline 收敛且轮数预算未耗尽，其余三个达到配置轮数上限。原业务 switch 均保持 1→1。收敛、语义执行一致和分析完备是不同结论。

## 为什么其余三个常量还没消费

新 f-combo O0 最终 IR 中：

- `memory.valid1203` 的 STORE14 地址已是 `%state.edge.live4592 = phi ptr ...`。
- `memory.valid1239` 的 STORE34 地址已是 `%state.edge.live4590 = phi ptr ...`。
- `memory.valid1588` 的 STORE32 地址已是 `%state.edge.live4582 = phi ptr ...`。

这些块仍进入 `bb_89568`。前一轮 SSA 修复把跨原块指针改成 PHI 后，既有 `byteAddress` 的直接 GEP-only 规则拒绝它们；这与 f-cff 的剩余指针 PHI 问题同类。本次没有扩大到 pointer-PHI 关系证明，没有抬高预算，也没有通过额外全程序堆假设来继续折叠。

## 证据和边界

VM 新 namespace 为 `~/vmc-runs/guest-memory-history-reference-20261111/`。`history-real-review.json` 保存逐模块 IR 字节比较、每轮计数/状态、四个候选状态门槛及输入/配方对齐。`history-v1-real-builtin3-audit.json` 保存原有索引与 switch 门槛。完整 raw 流、IR、native 二进制、全部故障控制和日志一并归档，不只保存摘要。

本组是四个既有真实 ELF 的重复回放，未新增 69 输入语料覆盖、机器指令/IGNIL 验证或完整去平坦化结论。新历史 fixture 的旧失败、新通过及独立 native 矩阵由 root 与独立 oracle 记录，不把这些数字混入本组 C16/LLVM16。

最终归档：VM `~/vmc-runs/guest-memory-history-real-20261111.tar.gz`，Mac `/Users/c/Desktop/deob/validation-artifacts/guest-memory-history-real-20261111.tar.gz`，SHA-256 `31045d1616873f1d04f48f7b2a1cfb4c69c6404524adb3ee1eb882be2edd793e`。完整 **621 文件**集合及逐文件 SHA 在两端独立通过，Mac 还有完整展开副本。

两个验证任务意外选用了同一个 reference 根目录，但各自产物子目录与文件名没有覆盖。这个最终组合包包含独立 native oracle 和本 real 回放，而不只是 real 组。较早的 native `ad93824…` 包已经混入当时的 real 135 个原始文件和 completion，却没有本次最终 real 后置审计、review 和 20 份 source 快照；因此它只承担 native 验证证据，**real 完成结论只使用最终 31045d 组合包**。从较早 manifest 到组合包，native/real 原始流全部字节不变；唯一同名变更是 real 的后置 review 脚本补上预先四候选中至少一个真实增益门槛，其余是新增报告/源码快照。原 oracle、原 audit 和任何故障比较均未放宽。

组合包是在 real 执行及最终审计/源码快照写完之后生成；native 写入已结束，另一任务当时的清理在删除前核验失败，没有修改文件。没有冒称提前协调了全目录冻结：一致性由封包前 manifest 与 tar 的全部 621 项逐文件双端核验确认。之后停止共享目录新增工作，按各自子目录独立精确文件集/hash 核验再退役。real 的 135 文件子目录已退役，摘要和 `REAL-ARCHIVED.txt` 恢复说明保留；native 子目录由其 owner 分别处理。只恢复 real 原始产物：

```sh
tar -xzf ~/vmc-runs/guest-memory-history-real-20261111.tar.gz -C ~/vmc-runs \
  guest-memory-history-reference-20261111/history-v1-real-builtin3
```
