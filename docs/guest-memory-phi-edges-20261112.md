# Guest 内存 SSA 的 PHI 入边完整性修复

基线 `e5d49be`。这是输出 IR 正确性修复，不是新增循环去平坦化能力。

## 已复现的问题

普通事实合并以“前驱块”存储事实，且在循环检查前故意不导入回边。旧实现仍会在两个
入口值不同时立即创建 PHI，只填写已分析的前驱；循环回边、未分析的不可达前驱缺项。
即便随后循环证明失败、取消 LOAD 替换，该非法 PHI 也没有删除。若循环事实被判断稳定，
LOAD 还会使用该缺项 PHI。普通合流及规范循环中的重复跳转槽同样需要重复 incoming，
不能只按前驱块去重。

最终新增测试直接链接旧主/无 Z3 运行库，均实际得到 **6 cases / 60 assertions，
4 cases / 7 assertions 失败**；输出包含 LLVM verifier 的缺 PHI 入边诊断。
这是实际 IR 非法证据，没有运行非法 IR，也不声称旧程序执行出错。

## 修复

- 普通不同值合流创建 PHI 前，核对已有事实覆盖全部实际 CFG 入边；缺少回边或未分析
  前驱时放弃这个事实，保留原 LOAD，不用 undef/poison 填充，也不猜测回边值。
- 完整合流按每个真实 successor 槽添加 incoming。同一前驱的重复槽使用同一已证明值。
- 规范两块循环通过原有入口/回边证明后，也按真实前驱槽填写循环 PHI。

原调用、别名、循环不变量、跨块开关、预算、公共 API 和默认流水线均保持原约束。
两个输入值相同的既有路径不需要生成合流 PHI，保持原行为。

## 最终验证

新增6个定向测试在新主构建通过 **109 assertions**，覆盖 self/body 循环各稳定和变化
两种情况、不可达前驱、普通重复switch槽、普通合流兼容、关闭PHI，以及重复循环回边。
主/无 Z3 全 CTest **1062/1062、927/927** 已通过，两套定向均为6/109。
独立原生每套9形态各 **1152** 次完整状态匹配；各384次错误变体观察中288个指定
错误全部检出，12672个比较器控制全检出。各48条保存流、1536帧审计无差异；
审计不增加执行数。两套运行库的9份优化输出IR逐字节相同。

冻结真实B配方的cff O0/combo O0产物，各运行旧、新Guest内存pass一次：四次均
通过输入/输出IR验证，两对完整输出逐字节相同。固定输入路径、ModuleID、配方与
运行库pin，没有归一化。这是兼容证据，不增加旧原生执行数，也不声称去平坦化收益。

普通合流拒绝缺前驱PHI不意味着后续全部禁止优化；独立按入边复制阶段仍可在
某条已证明入边上转发，并保留原LOAD和未知入边。不可达前驱的native夹具覆盖该情况。

证据：`~/vmc-runs/guest-memory-phi-edges-20261112/`，本地副本
`/Users/c/Desktop/deob/validation-artifacts/guest-memory-phi-edges-20261112/`。

实现提交 `5e9a75f`。最终归档、双端核验和主agent独立复核均已完成。

- [独立原生报告](guest-memory-phi-edges-independent-validation-20261112.md)
- [真实保存IR兼容](guest-memory-phi-real-compatibility-20261112.md)

三份归档均核对精确文件集合、全部payload大小和SHA；四份运行库gzip还逐流核对
解压后的完整运行库SHA。主agent另行完整复算两组保存流，仍各48流/1536帧，
不增加原生执行数。源码三项与已测试候选pin一致。

| 归档 | SHA256 |
|---|---|
| guest-memory-phi-edges-20261112/guest-memory-phi-edges-metadata-v1.tar.gz | 27c95f09536add0b54ddd40fe2b22781118a17c0a0702f42c716f50021c1ecea |
| guest-join-coverage-native-20261112.tar.gz | bcd5a469bbc0be4b159373bc9517e8a30179f704bfb383b113f69afa3a600a69 |
| guest-memory-phi-edges-real-20261112.tar.gz | ba67f38a6f89b864036508cc6cca71e1a3c26c2bf1d832415748906dd8ab1d1e |

构建实际来源为VM `e5d49be`加候选patch，记录实际HEAD/status而非倒填提交号。
元数据包不含可重建的旧测试可执行文件；源码、日志、退出码保留，运行库另包保存。
旧运行库raw硬链接在全部旧运行完成且gzip双机核验后退役，恢复路径保存在
`old-runtime-retired.json`；本轮新raw运行库仍保留。

本轮没有新增指令/Unicorn或全69输入语料库campaign。完整循环去平坦化仍未完成；
后续地址依赖去重候选尚未合并或执行旧新验证，不计入本次成功结果。
