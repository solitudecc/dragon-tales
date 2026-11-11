# entry-prefix 生产集成验证

本轮在 guarded-entry 里程碑 `aba656a` / 验证 `071c775` 上增加显式
[`entry-prefix`](llvm-entry-prefix.md)。变换隔离检查成功后的首次有界前缀，
以便后续优化消费入口值；保留原循环、未知调用、错误出口和普通回退。
这不是整个函数去平坦化完成，也没有新增全程序堆分析。

## 源码和构建绑定

Mac 开发、Ubuntu AArch64 VM 构建与原生执行，仅使用
`~/dragon-tales-overlap`。候选来源基线 `071c775` 加九个源码文件，逐文件 SHA
保存在 `~/vmc-runs/entry-prefix-production-20261111/candidate-v1-source.json`；
本地对应目录为 `/Users/c/Desktop/deob/validation-artifacts/entry-prefix-production-20261111/`。
最终实现提交为 `bb0c429`。VM 实际 Git HEAD 仍为 `7de235d`，在该工作树应用
guarded-entry 与 prefix 补丁；不是 checkout `071c775` 或 `bb0c429`。证据包
分别保留实际 HEAD、工作树差异和九个源码 pin，避免混淆来源与实际构建状态。

| 运行库 | backend SHA-256 | shim SHA-256 |
|---|---|---|
| main-v1 | `e5d15ecdc1a8ca789169635e335ee1f3ee5c336209a364a0ebaea7526d4d08da` | `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84` |
| noz3-v1 | `890781cb533d9019aadd153212a13493535463c92b130000cc6cf0f64e9a7627` | `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0` |

no-Z3 的实际 CMakeCache 为 `DRAGON_USE_Z3:BOOL=OFF`。两个构建均使用
原有配置和 `-j2`，新 pass 不依赖 Z3，默认六步流水线未改变。

## 旧代码失败与生产回归

最终 C++ 测试 SHA `371911c928ac6e78451fabe83a9051356f0876f1b34d7d658e2631d4f258295a`
只通过已有 PassPipeline 字符串接口调用，因此能在原 `origin/main` `7de235d`
的头文件和运行库上实际编译执行。main/no-Z3 旧运行库均为：
**5 cases / 23 assertions，4 cases / 5 assertions 失败**；失败原因是缺少
`entry-prefix` 注册，严格参数的负例仍通过。这是新增功能回归失败，不是旧
程序算错的证据。旧 Python 测试同样在 builtin 缺失处失败。

旧运行库 SHA 分别为 `cb53da7441501f9cfd5f14fc9641f8b5b9dddbf57a9c040a5d2bf9703422756f`
和 `0ea4b93b85662034f2e96e0b9247b71341ab51e92634d6b771df6e115ccc21b6`。
`api-old/` 保留最终测试、编译/执行退出码、完整 stderr 和前一个 22 assertions
版本的日志，未将两个版本混计。临时解压运行库已核验后移除，恢复位置保存在
`api-old/runtime-retired.json`，原压缩运行库仍保留。

生产 main/no-Z3 定向测试均通过 **5 cases / 107 assertions**。
两个构建的 Python 路由、严格参数、低预算拒绝和重复调用通过。
完整 CTest：主构建 **1051/1051**，no-Z3 **916/916**，均无失败。

## 原生与真实样本分别验收

生产 API 的独立合成原生矩阵 main/no-Z3 各通过 **1824 次完整状态比较**、
**608 次错误变体观察 / 198 个指定错误检出**；各 44 条保存流、2432 帧审计通过。
模型、ABI、内存和统计边界见 [原生报告](entry-prefix-production-native-20261111.md)。
四个真实 ELF 新配方另外通过 **C16/LLVM16 完整匹配**，寄存器16、Guest16、
Exit112 错误控制全检出。四份 Graph/source IR 不变，每模块实际运行25步：
一次 prefix，加旧配方三轮各八步。f-cff O0 首次实际 Guest70032 selector
LOAD/switch 消失；其入口 guard 的 LOAD 仍保留，成功路径在动态 bounds 后
到达原 `bb_67010`，错误 Exit 仍执行。主 agent 独立读取最终 IR 核对这一点。

prefix 的 f-cff O0/O2 分别完整复制21块/748指令、22块/961指令；combo O0
32块/2007指令后预算不足，combo O2 因 non-PHI 外部使用保守拒绝且原 IR 不变。
旧 GuestSSA 12/12、dispatch 12/12 仍是 incomplete，预算事件分别12/16。
不把成功执行或 prefix 的局部 complete 解释为整个函数去平坦化。
详见[真实验证](entry-prefix-real-validation-20261111.md)。

此前 standalone pilot 的首次 selector 简化只算原型精度证据，不与生产计数相加。
旧 guarded SP-only 和 SP+RSI 两组精度门槛失败的记录仍保留，不覆盖为本轮结果。

## 证据保存和范围

main 原生归档 `oracle-main-v1-evidence.tar.gz` SHA
`43c7b3c4c1f26e45f6688e089fd48942ffaa157d6c34b439418a99681205dc5e`；
279 个 payload 加 manifest，代理和主 agent 分别核对了本地逐文件内容及归档。
VM 与 Mac 两端原始流、IR、源文件、失败日志和 SHA 都保留。
no-Z3 独立归档 `oracle-noz3-v1-evidence.tar.gz` SHA
`beff9cabb15d1981d8e3a51d3dae62b7567da0c89a12aff33ac91a7ddb85a30a`，
278 个 payload 加 manifest。主 agent 另行核对两包的准确文件集合、每个归档
payload、大小和 SHA，与本地展开内容完全相同。
真实整包 `entry-prefix-real-20261111.tar.gz` SHA
`81216b4fa0a4d1ef6239504faae9264241f71a52498f79d710f2e9292d097730`，
330 个 payload 共284,727,672字节，另含 manifest；主 agent 独立复核两端压缩
SHA、准确文件集合以及每个 payload。四份生产运行库也已 gzip 双端保存，
主 agent 逐份复核压缩 SHA 和解压后的原始 SHA/长度；记录在本地
`runtime/runtime-gzip-final-v1.json`，原始硬链接暂保留。
最终元数据包 `metadata-final-v1-evidence.tar.gz` SHA
`5f17ae94cc49aa539c3167ab43f29b1c69f4004599a07f86af3ee3b645f4840f`，
99 个 payload 加 manifest。它包含实际 VM 源码/HEAD/差异、两配置、全部本轮
构建和回归日志、旧代码失败，以及运行库压缩首版因 shim 路径写错而失败的原始
脚本和日志；修正仅涉及归档路径。代理双端校验后，主 agent 又独立检查了完整
归档和展开文件集合、每份长度/SHA。没有把归档基础设施失败算成语义失败或成功。

磁盘清理只压缩旧 static-region/prepared-region 的四份独占运行库，逐份校验
压缩及解压 SHA，复制 Mac 后再次校验，再移除 VM 原始副本。恢复收据位于
本地 `retired-runtime/` 和 `retired-prepared-runtime/`，VM 压缩文件旁有相同收据。
没有删除失败日志、缩小对照范围或将磁盘预检中止计作语义通过。
真实 `candidate-v1-prefix` 的193个文件也在代理和主 agent 分别验证整包后，
再次逐项比对展开文件与归档，再退役这一目录，释放283,184,899字节。
双端收据 `entry-prefix-real-20261111-ARCHIVED.json` 的 SHA 为
`6cc3ed6c5b6418ebe7d2b9f7035d16cc79fdcd685618fb5fed94ada72ccbc268`，
包含全部文件 pin 和精确恢复命令；外围源码、契约、报告及整包仍保留。

本轮没有新的单指令/Unicorn、IGNIL 语义或全 69 输入语料库 campaign。
原生矩阵验证此 LLVM 变换的执行结果；IR verifier 只验证结构。对未知调用的
内存副作用、任意堆对象、其他架构指令和完整循环去平坦化均不作新增覆盖声明。
