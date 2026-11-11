# entry-prefix 生产 API 独立原生验证

本报告与 standalone v2/v3 分开计数。这里的输出由实际冻结 backend 的
`LLVMLifter::runEntryPrefix` 生成，没有用 standalone 工具替代生产调用。

## 绑定的生产版本与驱动

运行库目录：
`~/vmc-runs/entry-prefix-production-20261111/runtime-main-v1/`。

- backend SHA-256：`e5d15ecdc1a8ca789169635e335ee1f3ee5c336209a364a0ebaea7526d4d08da`
- shim SHA-256：`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`
- 最终 driver SHA-256：`c3de98f01ba23cd2bd68145df2ac4a86e289ebe68f4ac41a6900b5d9551cd941`

`entry_prefix_production_driver.cpp` 只链接该 backend；`parseAssemblyInto` 将
冻结 IR 解析进已设为 StaticAnalysis 的 lifter 原有 Module/LLVMContext。
驱动要求仅选中函数具有唯一 `guarded.accept`，不替换 Module、不装载第二套
LLVM shared library。每次运行记录以下检查：

- Module、全部 Function/GlobalValue 顺序和身份、所有 Argument 对象保持；
- 非目标函数/global 的完整打印文本不变，目标属性/链接/section/comdat/调用
  约定不变，named metadata 和目标函数 metadata 不变；
- 运行后完整 verifyModule；stats 与预先确认的唯一候选一致；
- 无改写时整个 IR 原文不变，失败和重复 skip 不产生输出文件；
- 合法截断复制可以写出，但 JSON 保留 `complete=false`，不因原生对拍成功
  而提升成完整分析。

检查只针对夹具实际拥有的对象。这个矩阵不凭空提供全种类 global initializer
或复杂 metadata 覆盖；它们的生产单测和事务代码审查应单独报告。

## 冻结输入和结果

所有输入 IR、模型和输入集合与已保存的 standalone v3 一致，运行前后逐字节
核对。6/256 与 32/2048 分别保存输出/stats，原生执行只使用 32/2048。

| 独立子组 | 正常匹配 | 故障观察 | 指定故障全部检出 | 比较器 controls |
|---|---:|---:|---:|---:|
| 新四模式，两 O 级 before/after | 1024 | 512 | 164/164 | 5120 |
| 原 canonical loop112，三变体×两 O 级 | 672 | 0 | 本组不新增故障 IR | 2688 |
| terminal default/重复 PHI，两 O 级 | 128 | 96 | 34/34 | 1280 |
| 本轮合计 | **1824** | **608** | **198/198** | **9088** |

差异、排除、跳过均为 0。正常和故障观察均全量比较，没有只比较返回值。
新四模式及 terminal 比较 i64 返回值和完整 73728 字节 arena；canonical
loop112 保留原 139272 字节 snapshot，包括寄存器、Guest、全部 Exit、别名
布局和外围状态。终结 default 的错误 Exit 和全部七个字段均被比较器控制
覆盖。此矩阵没有执行新的未知调用样本，不能扩大成跨调用效果验证。

新四模式仍是 `entry_phi`、`diamond_no_dead_latch`、`loop`、`fresh`。原
`diamond` 首次拒绝/native0、原 whole4 gate=false 的历史没有改动。原十模式
仍仅 loop 可供该子组变换；其余九模式拒绝不计作兼容成功。

实际改写门槛要求 seed 进入 clone、loop/fresh 回原 header、terminal 的
default/hit 指向复制 leaf、重复 successor 数量正确，不能用 unchanged IR
满足正常对拍门槛。

## 拒绝、重复运行及身份

13 次成功生成及 5 次无输出检查，共 **18 组** driver JSON 全部核对。
四个结构拒绝为历史 region、普通 liveout、共享 leaf、预算不足，均
`complete=false`、`changed=false`、IR 原文不变、无输出。再次处理已复制
terminal 的 seed 会 `skippedSingleEntry=1`，`complete=true`、`changed=false`，
原文不变且不生成第二个输出。

6/256 的安全截断复制保留其 incomplete 状态；上述 32/2048 正常输出均完整。
实际运行的所有成功输出都检查 Module/Function/Argument 身份及完整 IR。

## 保存流审计与失败历史

`entry_prefix_production_saved_audit.py` 复核 **44 条 gzip 流、2432 帧**，逐帧
核对独立模型、完整长度、顺序和无额外尾部。另检查 18 组身份/stats、固定
模型 SHA、输入不变、driver 实际链接路径及 backend/shim/source 前后 SHA。
审计通过；审计本身增加原生执行数 0。

driver v1（SHA `1b1312395aa79150b351a484829572b6e1b3801dfeca6450441e93ba107088bd`）
首次编译失败：本地 helper 名 `quoted` 经 ADL 被解析成 `std::quoted` 的
非 const 重载。保留 v1 源码和完整编译日志。唯一修正为 helper 改名
`jsonString`；输入、变换 API、模型、范围和精度门槛均不变。这是驱动基础
设施失败，不是生产语义失败，也不计入旧代码失败回归。

main 证据位于 `~/vmc-runs/entry-prefix-production-20261111/oracle-main-v1/`。
本报告不包含真实 Tigress root 的原生执行。x64 形状的 IR 在 AArch64 VM
编译成本机代码执行；没有新 Unicorn、IGNIL、其它架构指令或全语料库
campaign 声明。

## no-Z3 相同生产矩阵复测

另冻结 `runtime-noz3-v1/`，确认 `DRAGON_USE_Z3:BOOL=OFF`：

- backend：`890781cb533d9019aadd153212a13493535463c92b130000cc6cf0f64e9a7627`
- shim：`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`

复用相同 `c3de98f...` driver 源和逐字节相同的已编译 driver 二进制，通过
`LD_LIBRARY_PATH` 切换到 no-Z3 backend，并保存实际 `ldd` 路径及二进制 SHA。
固定 IR、模型、输入、错误控制和比较范围完全不变。

no-Z3 独立得到 **1824 次正常全状态匹配、608 次故障观察、198/198 指定错误
捕获、9088 比较器 controls**，差异、跳过、排除均为 0。其 18 组身份/stats
检查、全部 44 流/2432 帧审计同样通过。两配置属于同一输入覆盖的重复执行，
不能算成新增样本或形状覆盖。

新版保存流审计提供显式 `--runtime-flavor main|noz3`，每个分支只允许上文
固定 runtime SHA 和对应路径；no-Z3 另检查复用二进制及 CMake OFF 记录。
默认仍是 main；新版 main 分支也对旧 main 证据只读复核通过，结果保存到
no-Z3 目录。原 main 的归档和旧审计文件均未改动。

no-Z3 证据位于
`~/vmc-runs/entry-prefix-production-20261111/oracle-noz3-v1/`，独立归档；
不会覆盖 `oracle-main-v1`。生产源码身份、全量单测及真实样本证据由 root
另行汇总，不能由本矩阵的成功替代。
