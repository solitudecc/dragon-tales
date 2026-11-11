# Business corridor 独立原生验证（2026-11-12）

本组通过实际公开 `PassPipeline` 启用数值 `business_corridors=1`，验证自动
发现业务 STORE 到动态表 LOAD 的路径。候选字节只是预测；实际 LOAD 比较和
同一 SSA LOAD 的完整 switch/default 保留。此处没有一般完整去平坦化结论。
以前 `guarded-transition-native-20261112` 的模型、输入和封包没有修改。

## 固定范围

6 模式 × 整数边界 0/1/最高位/全1 × 循环上限0..3 × 初始实际表值43/44/99/777
× size65536/511，共 **768 个独立输入**。在 O0/O2、before/after、main/no-Z3
重复执行不增加独立输入数。

5 个正例实际复制八个原块 head/state_check/copy/mutate/state_load/index/
bounds/dispatch。还包含一个 historical snapshot 块、一个 fallback 块，实际
cloned_blocks=10。真实 Guest stateA256→stateB264 经过 LOAD→STORE→LOAD，
并沿 seed 进行后续循环；旧 fresh/loaded 输入与本次 fresh LOAD/freeze 分开。
正例分别为普通、无关 STORE、后改 state、目标 LOAD 前改表、LOAD 后改表。
无关 STORE 使预测从44变99，不能当作状态槽相等的证明。

第六个 call_path 在路径中途调用未知函数，实际 API 保持其未变；它仍完整
执行并比较 callcount、寄存器和内存副作用，但不算新增复制收益。大小失败
输入在首轮原路径退出，本组不冒称覆盖复制后才改变 descriptor size 的路径。

完整比较返回值、callcount 和整个73728字节 arena，其中包含完整64KiB Guest、
寄存器、描述符、配置和七个 Exit 字段。输入中的描述符原生指针按本次真实
地址生成并完整记录；没有归一化、排除或跳过。freeze 输入在这些原生用例中
有定义；此处不宣称任意 poison/undef 的执行覆盖。

## 独立错误控制与结果

八类错误 IR：无条件信任预测、命中跳错 handler、STORE 后重读 selector、
丢弃 stateA→B 写入、旧 fresh 复用、历史/fresh 混淆、错 PHI、错 Exit。每个
错误 IR 先与其独立错误模型一致，再检查正常比较器拒绝所有应有差异。
返回值/callcount/regs/Guest/七个 Exit 字段翻位使用同一比较函数，必须失败。

主库与 no-Z3 的 O0/O2 原生矩阵及各自40流/5120帧逐一保存审计全部通过。18个输出 IR（含保守未变配方）和18个 parsed-before 在两构建间逐字节一致。没有为通过测试修改冻结模型、输入、控制或放宽比较。

| 每个构建的固定计数 | 数量 |
|---|---:|
| before 完整状态观察 | 1536 |
| 实际变换 after 观察 | 1280 |
| 未变 CALL 路径 after 兼容观察 | 256 |
| 完整状态匹配合计 | 3072 |
| 错误 IR observations | 2048 |
| 指定错误全部捕获 | 1410 |
| 比较器控制全部捕获 | 33792 |
| 保存完整流 / 帧 | 40 / 5120 |

3072 次正常比较中，一半1536是业务正常返回，一半1536是受检查 size 失败
退出，不能称为3072次业务根成功返回。逐帧保存审计另外读取实际 Exit.kind
确认这两个数；审计不会增加原生执行计数。

## 旧库与默认兼容

使用相同已编译 public Pipeline driver，在旧 e96 库上实测：6个 default 配方
保持字节不变；6个 enabled 和6个 zero 配方明确拒绝未知
`business_corridors` 参数，未执行替代配方。该结果是缺新选项/能力证据，
不是旧库错误语义。主库 default/0 各6个仍保持原输入；新5个 positive 真正
生成八块副本，CALL例保持不变。max_blocks=1 保持输入并报告 incomplete。
旧/新 default 的6个输出和6个 parsed-before 全部逐字节一致。

每次调用都 verify 完整模块，检查 Module/Function/Args 身份、属性及其他
globals 保持。`complete=1` 只表示本次已选候选流程完成，不表示原 dispatcher
全部消失；原 switch 和 fallback 仍在。

## 来源与复现

- producer/model：`19bb40a10559a6ea3c385f60c590ad3c809a6e69e7fcd5e6d108c468c12888bd`
- actual public API driver：`76057325652c2715c50d5f587a70ae5b067447ad9412529cc62adbe8c19afdd4`
- baseline backend：`e96ec64725e521e88e8104c4e77bc1103c39dc5593642afe4ce9d7ccce5971c8`
- main backend：`a9efefec601995481c6c3acceab55c6cf9a571e32b743c2f708af1b117269282`
- main shim：`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`
- no-Z3 backend：`d3977cb6eab9a28ffe7c0f1ffc7de48d3a0b0962083fa15ab7aefb155ddd6659`
- no-Z3 shim：`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`

独立 VM namespace `/home/cc/vmc-runs/guarded-business-native-20261112`；
`baseline-main/` 保存旧结果，`main/` 与 `noz3/` 分开保存候选结果。

```sh
python3 -B source/guarded_business_prepare.py /home/cc/vmc-runs/guarded-business-native-20261112 main /home/cc/vmc-runs/guarded-business-corridors-20261112/runtime-main-v2
python3 -B source/guarded_business_oracle.py run before main/enabled main/native-O0 --opt 0
python3 -B source/guarded_business_saved_audit.py /home/cc/vmc-runs/guarded-business-native-20261112 main main-saved-audit.json
```

输出目录必须不存在，避免覆盖失败。换 O2 与 no-Z3 runtime 运行其余固定组。
源和运行库均有前后 pins 与 ldd；VM 不复制额外库，driver 只链接 libdragon。
这是 host DL64 LLVM 原生矩阵，不是新的机器指令、IGNIL 或全69样本验证。
真实复杂样本与完整循环闭合情况由主任务另行报告。

完整独立包 `guarded-business-native-20261112.tar.gz`：577文件（576 payload + manifest），SHA-256 `06a6cfba3b8a13fe8a31546055e011452681c757f66bb871db3ea16ea48266b4`。VM 原展开及 tar、Mac tar及完整展开均已 exact file-set/size/SHA 核验。VM根 `/home/cc/vmc-runs/`，Mac根 `/Users/c/Desktop/deob/validation-artifacts/`，同名目录和外置 `*-archive-verification.json` 提供恢复/校验依据。封包使用前阶段已入仓 `guarded_transition_archive.py` 的原版；包内 README 是封包前报告，最终SHA以本文和外置收据为准。没有删除运行库或任何原始证据。
