# 循环去平坦化统一入口与 SP 观察点收尾

本轮在 `6d4c5ff` 的已验证核心上补齐统一接口、使用流程和一处入口事实消费缺口。
不生成独立可执行文件，不增加 VM 或全程序堆分析，也不改变默认六步优化。

## 公开使用入口

- `loop-image-context` 是显式 LLVM pipeline 步骤。C++、现有 C JSON 接口和 Python
  使用同一个原生执行器及参数校验器。`view_symbol`、`table_offset`、有序 `targets`
  是待检查候选，不是调用者提供的证明。目标槽保留顺序和重复值。
- C 的 flat JSON 使用 uint64 十进制 / 十六进制字符串；Python
  `PassPipeline.add_loop_image_context` 接受自然整数及有界有序 iterable，最多读取
  513 个元素，拒绝 bool、无序集合、越界地址及超过硬上限的预算，再一次性添加步骤。
- 原生步骤保留全部八个计数器：`available / complete / closed / budget_exhausted /
  guarded_bytes / preserved_stores / constant_values / work`。StaticAnalysis 下拒绝是
  Incomplete，未修改目标；其它执行模式是 Unavailable。`ok / converged` 不表示闭合。
- `function-control-flow` 使用新的指定函数 C++ 重载，只对明确命名的定义执行现有
  六个 LLVM pass。旧无参数方法、全模块 `control-flow`、C 布局和默认流水线保持兼容。
  不存在的函数报 Error；通用 mutable Error 的 `changed=true` 是保守报告，实际 IR
  不变另行检查，不能将该标志当作精确变化证据。
- [`scripts/deobfuscate.py`](../scripts/deobfuscate.py) 读取显式 JSON 配置，依次保存加载、
  静态区域提升、guarded entry、循环证明、函数内清理的 IR 和原始报告。闭合失败即停止
  清理，输出新目录拒绝覆盖，不重试更大预算，不声称自动发现完整目标集。

参数与命令见[统一工作流](deobfuscation-workflow.md)和[流水线接口](pass-pipeline.md)。

## 不改 SP 的精度修复

`SPObservations` 的弱句柄不是 SSA use。函数不修改 SP 时，观察仍直接指向动态入口
LOAD；只替换 accepted 区域中 SSA uses 不会让这个弱观察变成 ConstantInt。
原控制证明因此拒绝了本已由实际入口相等检查保证的同一值。

现在仅当观察 **就是同一条 `initialSP` SSA LOAD**，且新的 DominatorTree 证明实际
accepted block 支配该观察块，才在本次控制证明中取其已检查常量。原 LOAD、运行时
guard、错误 / 回退路径都保留；同址的另一次读取、PHI 和后续 SP 改写不获得该事实。
没有添加 alias、definedness、不可变内存假设或放宽预算。

源码为 `lib/LLVMImageTableContext.cpp` SHA256
`7b927ed3ad3ea3b1b7e3af5de4c537bde31890104ea9d3353243179cf5726ef8`。
这是共享控制证明的受限精度修复，不是机器指令语义变化。

## 已执行验收

| 项目 | 实际结果及范围 |
| --- | --- |
| 新入口同客户端旧 / 新 | 旧 6d 头及 d446 SDK：6 cases / 32 assertions，6 处新步骤注册失败；同客户端只换 edab 后 6 / 349 全过。旧失败是 API 缺失，不是执行错误 |
| SP 同客户端旧 / 新 | edab：2 / 48，仅新闭合阳性失败；同一客户端只换 48cb 后 2 / 77 全过，未守卫 / 改写 / 重读三类负例继续拒绝 |
| C 接口 | 两个新用例 / 48 assertions，通过现有 C pipeline 验证配置、状态、错误与 IR 保留，无新增 ABI struct |
| Python | 最终有限叶场景在 main 和 no-Z3 均通过；有界 uint64 编码、原子校验、真实提升 / guard / proof / cleanup 及错误合同完整检查 |
| CLI 自检 | 16 个配置拒绝、6 个未闭合门控制、3 个输出控制及最大合法边界阳性；不导入 Dragon，不运行 Guest |
| 新 SP 路径独立 native | C 18 + 原 LLVM 18 + 优化 LLVM 18 = 54 次完整 66112 字节匹配；702 比较器控制；36 故障观察 = 16 指定差异 + 20 完整未触发匹配 |
| main 完整 CTest | 1332 / 1332，零失败 / 跳过，195.096110 秒外层时间 |
| no-Z3 完整 CTest | 1186 / 1186，零失败 / 跳过，168.613900 秒外层时间 |

两个 full suite 按原参考 CPU4–7 / `-j4` 顺序运行，原每测试时限不变，外层
900 秒 / 512 MiB 启动空闲空间 / 64 MiB 运行下限不变。381 个实际源文件的
stat / ctime / SHA 和八个构建产物运行前后精确相同。
证据根 `/home/ubuntu/vmc-runs/loop-pipeline-full-20261115-v1/`，结果 SHA256
`c08b57b7e62ea00a4e225298a573925793d0cb2f4ddd56f96c937d3069e214c7`。

SP native 使用九个独立 arena，包括实际 RegBytes / Guest 重叠，以及 SP、输出指针、
两个表槽、PC、size 的失败情形。完整独立 C / Python 模型、原始 LLVM 校准和故障
对照先通过，再调用一次已安装的公共 API，最后执行优化 LLVM。没有排除、归一化、
新 Unicorn 或新机器指令测试。详见[独立 native 协议及结果](../scripts/validation/loop_sp_snapshot_native/README.md)。
该数据与上一阶段真实 B 的 66136 字节 / 16 输入 native 证据属于不同协议，不能合并
帧数或冒称重跑。full suite 的内部执行也不另计入上述独立 native 数字。

## 真实样本与边界

同一真实 B raw guarded 输入上，direct Loop 与统一 pipeline 的完整 IR、所有统计
和状态精确一致；独立官方六步参考与指定函数清理也全文一致。修复前 edab 和修复后
48cb 的报告字节相同，work 都为 985771，guard640 / stores68 / constant1，无预算退出。

通用 CLI 从 ELF 重新建立只含 kernel 的独立模块，两后端均闭合并清理：work984702，
guard640 / stores68 / constant1；733 块 / 16814 指令 / 1 switch 变为
448 / 10900 / 0。模块范围不同，不能将 CLI 的工作量与包含 wrapper 的 985771 混用。
这些本轮真实接口检查不新增 Guest、C 或 Unicorn 执行次数。

首次 Python 夹具并未满足当前索引证明的精度要求；其原失败、未改源码、同 direct API
与 pipeline 一致的拒绝均保留，不声称这些形状已经修复。新增独立有限叶机器程序
用于成功路径验收；它与初版不是整机等价程序。错误报告的初版期望与通用 mutable
Error 合同不符，最终同时要求保守 changed=true、指定错误及完整 IR 不变；原失败
和一次准备脚本 SyntaxError 也保留。详见
[Python、接口兼容与真实 CLI 验证](loop-pipeline-python-real-validation-20261115.md)。

这是一套可调用、可配置、可诊断的通用基础入口。未证明的状态 / 别名 / 未知调用仍
保守保留，历史 cff/combo 组合是否全部消除分发循环需要各自重新验收；本轮没有
宣称任意混淆样本都可自动处理，也没有新增全 69 输入或单指令语料库验收。

## 证据封存

同客户端接口包：`da6c39b3a6db18d892a2babbca893187c8a045a0a099b2ca86f96afef06bd552`，
5150300 字节，完整 83 成员。SP 同客户端包：
`b70c4a4abb03242bfe1680048a5e694927daa39f1d8af944ca4f5321e69820b0`，
4955244 字节，完整 40 成员。SP native 包：
`dcea7e5555449e3ebe5646b57ca03a381a5bcad6b9079df02a81a5843440c31d`，
5807224 字节，完整 140 成员。各包均在 Mac / ROCK 独立核对完整成员、大小、SHA、
tar padding 与单 XZ EOF；原始失败不删除，SDK 另包引用。

最终八产品与当前源码快照的独立封存作为随后验证记录提交，不把目录或 manifest
当作证明证书，不以这些源码冒称完整 OS / 依赖环境备份。
