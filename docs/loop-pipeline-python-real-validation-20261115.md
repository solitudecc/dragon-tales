# Python Loop pipeline 与真实输入接口对照

本轮验证 Python 参数编码、pipeline 报告和指定函数清理接口。新调用均为 LLVM 层分析、变换或保存结果审核；新增 Guest、C、Unicorn 和 observer 执行均为 0。既有真实 B 的原生语义证据仅作外部引用，不重计。

## 最终 Python 场景

`tests/python/test_loop_image_pipeline.py` 最终 SHA256 为
`5ede859f15862f3dd394a9ef31c7ae596b93ddbdace560890b74cddf3dbd4c4c`。
它检查有序 uint64（含高位和重复）、有限 iterable、无限流最多读取 513 项、bool/集合/映射拒绝、UTF-8 与 NUL、table span 溢出、各硬上限和失败前不入队；同时实测 Legacy Unavailable、Static Incomplete、完整闭合和指定函数清理。

最终小机器程序是独立有限场景：入口设 state=0；A 设 state=1 回到头；B 执行 `mov eax,7; ret`；越界出口独立 RET。其机器程序与早期循环夹具不同，不声称整机等价。所有原闭合、统计、ordinary/source 完整 IR 不变断言保留。

缺失清理函数必须抛 `PipelineError`、报告 Error 和含 `requires a defined function`/`absent` 的消息，同时完整 IR 保持不变。既有通用调度器对 mutable pass 的 Error 保守报告 `changed=true`；最终测试也明确检查该合同，未修改调度器。

最终 active 源在两套固定 SDK 各运行一次：main `48cbb8a3aa4002f8942655ba2716088366060676a525319e178d79bad1125db0`（3.024 秒）和 No-Z3 `6db2207b53be087c52bcbb0087a8927a0fa8fff135e64cc902dfec069e9bb91b`（2.944 秒），均全部断言通过、guard exit 0、输入 pins 未变。原 180 秒、512 MiB 启动、64 MiB 运行门槛和 CPU0–3 不变。结果位于 ROCK
`/home/ubuntu/vmc-runs/loop-pipeline-python-final-focus-20261115-v1`。

## 保留的首次失败与校准

- 初始源 `9c8b6e03b01914796c7b36dcfa9f1f29a1e6d82bc6524f4f202ad30f90b5c21c` 在原小循环夹具拒绝：work 2117、budget=0、`lookup may use an unproved outside-table path`。活跃异常帧在关闭上下文前保存 raw/view/报告。直接 C++ API 与新 builtin 对同一实际 parsed 状态同样拒绝，完整 IR 和全部统计一致，拒绝未改 IR。这是组件精度边界，未声明修复。
- root 的仅分支编码 v2 仍拒绝；增加外部 state+1 回边的 v3 在旧 SDK 先拒绝 SP。固定 SP SDK 的同一 v3 源 `1fd52a2545476790dfcadad5a7b0667d0b03b2eed8d4ed98cd0a63416d7cd0cd` 实测越过 SP 后，在 batch 有限状态证明处拒绝，work 2977、budget=0、changed=false。没有通过调整预算隐藏拒绝。
- 有限叶 v4 保留所有原断言，Loop 闭合和 cleanup 已通过，最后因测试误要求 mutable Error 的报告 `changed=false` 失败。v5 按既有合同增加状态、消息、`changed=true` 检查，但新增行缩进错误引起 SyntaxError；此版本 0 Dragon 分析。v6 仅修该行缩进，先做 stdlib AST 检查，再实测全通过。
- 独立 opt 诊断因 LLVM 的 InstCombine 一轮 fixpoint 检查中止，未生成规范化 IR；未关闭该检查重试，也未把它当产品失败。

原源、每版输入、实际失败日志、计划和调用记录分别保存在同名 artifact roots（`loop-pipeline-python-v1-diagnosis-20261115-v1`、`loop-pipeline-python-sp-after-20261115-v1`、`loop-pipeline-python-finite-v{4,5,6}-20261115-v1`）。它们不被最终成功覆盖。v1/v2 的 index 精度缺口仍存在。

## 真实 B 的同 public API 对照

独立客户端 `scripts/validation/loop_pipeline_real_compat.cpp` SHA256
`b89d75da55aea5e329a049ec2a4f9fe1e46691e987e006e048c18dce2a945501`
只链接固定 installed SDK，不包含任何产品私有 `.cpp`/`.o` 或另一个 LLVM 库。两套 LLVMLifter 分别从同一 B before 完整文本 `d7118cf542bcf9f559b11c60bb31ad10331b7603c828bfd2826c35f170cb6cc4` 解析，先检查实际 parsed Module 完全一致；不改 ModuleID，不归一化文本。

在固定 edab SDK 上，direct Loop 一次、新 builtin 一次得到完整 IR、全部八个 counters、message/status/changed 完全一致：available/complete/closed=1，guarded_bytes=640，preserved_stores=68，constant_values=1，work=985771，budget_exhausted=0。随后对相同状态运行独立官方六步 reference 和 `function-control-flow`，完整 IR 与八个结构统计也相同：733 blocks/16814 instructions/1 switch → 448/10900/0。ordinary 函数、原 Function/Argument/global/named metadata 身份门全部通过；仅允许原先已验证的 canonical `llvm.fshl.i32` 声明导入。

实际 guard 用时 6.964 秒、exit 0。报告：
`/home/ubuntu/vmc-runs/loop-pipeline-public-compat-20261115-v1/real-B/REPORT.json`，
SHA256 `bb2bae4f9a8f299c5b8e4650cb44a1cb8d5c2e59d41a7e34785325474c59c00e`。
两端 saved-only 审核覆盖该根 41 files/10,793,993 bytes 和原 v1 诊断 21 files/128,077 bytes，全 set/size/SHA 相同，核验前后身份与 hash 未变；审核本身没有 Dragon 调用。
审核报告 SHA256 `666cef8b9f5b828ec62203fef64769e1ef4fcca5adaae03dd33c643cc272bac3`，位于 Mac artifact `loop-pipeline-compat-saved-audit-20261115-v1/REPORT.json`。

同一物理客户端仅将 LD 切换到固定 SP SDK 48cb 后再次通过（7.131 秒），完整报告与 edab 对照报告字节相同，SHA256 也相同。这是新的接口兼容调用，仍不增加 Guest 语义比较计数。

## 通用 CLI 的真实 ELF 运行

`scripts/validation/loop_pipeline_real/flatten-o2.json` SHA256
`acaa609807fd2204dec9a7325aef29cf640e05d3306d495538a7959dbd64eb48`
消费已封 ELF `bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`，kernel entry=0x1010、SP=51192、RSI=32768、table=8192。80 个 slot 保持原顺序和重复（75 个 unique targets）；640 字节均与 ELF 和原 16 帧对应。78 个额外 leaders 来自冻结 coverage，并核对独立机器指令起点。

`PROVENANCE.json` 明列 ELF 符号 extent、原始路径与所有 SHA、SP 的 wrapper/kernel CALL 差别及参考证据；这些是请求候选，不是内存不变性、noalias 或执行值证明。CLI 仍必须生成实际 guard 并完成完整 Loop proof。新的 Module 只包含所选 kernel，不要求其 work 与历史多函数 Module 的 985771 相同。

先前的 `--validate-config-only` 不调用 Dragon、不打开 ELF，单独保留。随后固定 CLI 源
`e7ae4b9ab0844d8a4d77db76e37a5fc1b778e85232a99509a7a5d749ce712697`
在两套固定 SP SDK 各运行一次完整实际 ELF 流程，均为 `proof-closed-cleanup-completed`：Loop work=984702、guarded_bytes=640、preserved_stores=68、constant_values=1、closed/complete=1、budget_exhausted=0；cleanup 仍为 733/16814/1 switch → 448/10900/0。source lifter 完整文本不变。main guard 用时 8.564 秒，No-Z3 8.507 秒；两者 exit 0、输入 pins 未变，No-Z3 loader 日志没有 libz3 DSO init。

结果根为 ROCK `/home/ubuntu/vmc-runs/loop-pipeline-cli-real-B-20261115-v1`。
main `main-output/report.json` SHA256
`f28ed5db1c9d88234f06b8439a3fa17c33ec190f7f50035343a4238d95468d60`；
No-Z3 `noz3-output/report.json` SHA256
`ca3a65f90305da2ecfcd979b0b5d7bd3c4a3b8e7c536dd688a337738c9c431d6`。
这是对公开通用脚本和证明报告的验收，不是新的机器执行或自动发现 table 常量的证明。新 Module 的 work 与历史多函数 Module 不同，未归一化该差异。

后续有限小机器程序如需语义验证，应复用 `function_guest_diff.Adapter`/`function_static_guest_diff.StaticAdapter` 的初始完整状态、Unicorn 参考、76 字段及 64 KiB 比较和原 O0/O2 编译协议；新程序必须新做参考，不能借用大 B 的 16 个 UC 结果。另一个 agent 的独立 SP synthetic 原生校准不归入本文件的计数。
