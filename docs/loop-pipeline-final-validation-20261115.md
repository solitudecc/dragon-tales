# 统一 Loop 工作流最终验收与封存

实现提交为 `17757ff1de1563b93d5b9ab3ef1de84a2fb8858b`，基线为 `6d4c5ff`。
本记录和最终归档引用在随后单独的验证提交中保存；不属于归档内的实现源码快照。
实现、证明范围及未支持形状见[实现验收](loop-pipeline-integration-20261115.md)，
操作说明见[统一工作流](deobfuscation-workflow.md)。

## 完整测试与源码绑定

ROCK5B 两个后端顺序执行原注册 CTest，CPU4–7 / `-j4`、原每项时限不变。
main 为 **1332/1332**，no-Z3 为 **1186/1186**；均无失败、禁用、跳过或新增排除。
外层时间分别为 195.096110 和 168.613900 秒。运行前后 381 个实际源文件、八个构建
产物的大小 / stat / ctime / SHA 全等。封存的 381 个编译源内容逐项等于实现提交快照。

构建宿主的实际 Git HEAD 仍是 `49f58f…`，其受控内容覆盖及源哈希另行保存；不能把这次
构建称为在干净 `17757ff` checkout 上重编。Mac 的实现源码快照则在干净 `17757ff`
工作树上采集，包含 1202 个源码文件 / 13,108,940 字节，以及 9 份 Git / 清单记录；
合计 1211 个 regular 文件 / 13,851,721 字节。采集前后原文件 stat / ctime / SHA 全等。

| 记录 | SHA256 |
| --- | --- |
| 完整测试 RESULT.json | `c08b57b7e62ea00a4e225298a573925793d0cb2f4ddd56f96c937d3069e214c7` |
| 双机完整测试证书 DUAL-VERIFIED.json | `c71e64365f050e88910574217c4025126c8a3f6b8d1039030311b44b683016c7` |
| 完整测试 FULL-MANIFEST.json | `abd679f46525de2b2ff668c0a759ce5410065c1ca2fc7a9e945a1853cd90a12c` |
| 实现 SOURCE-SNAPSHOT.json | `cefa0b8f8c98fd9f5258221a91b4218c5e432acb95e906535254dc969bd0b780` |

Mac 完整测试根为
`/Users/c/Desktop/deob/validation-artifacts/loop-pipeline-full-independent-verification-20261115-v1/`，
ROCK 对应 `/home/ubuntu/vmc-runs/loop-pipeline-full-independent-verification-20261115-v1/`。
源快照对应两端的 `loop-pipeline-final-source-snapshot-20261115-v1/`。
证书核对全部 16 个原始测试记录文件 / 3,391,963 字节及 JUnit 实际注册和通过数。

## 独立验证包

以下包均双机逐项核对完整成员集合、大小、内容 SHA 及压缩流 / tar 结尾；先前失败和
旧源码均保留，没有用成功重跑覆盖失败。它们不增加完整测试内部执行或上阶段真实样本
native 的统计。独立 SP native 本轮新增 54 次完整状态匹配、702 比较器控制，以及
36 次故障观察（16 个指定差异、20 个完整未触发匹配）；无新增 Unicorn / 单指令 campaign。

| 包 | 压缩字节 | SHA256 |
| --- | ---: | --- |
| loop-pipeline-same-client-20261115-v1.tar.xz | 5,150,300 | `da6c39b3a6db18d892a2babbca893187c8a045a0a099b2ca86f96afef06bd552` |
| loop-sp-snapshot-same-client-20261115-v1.tar.xz | 4,955,244 | `b70c4a4abb03242bfe1680048a5e694927daa39f1d8af944ca4f5321e69820b0` |
| loop-sp-snapshot-native-20261115-v1.tar.xz | 5,807,224 | `dcea7e5555449e3ebe5646b57ca03a381a5bcad6b9079df02a81a5843440c31d` |
| loop-pipeline-python-real-evidence-20261115-v1.tar.gz | 4,953,871 | `d9c24a4f43105bb95baba184854bad4f85b699c91fa5966ad3cfd635ec765123` |

前三个包位于 Mac `/Users/c/Desktop/deob/validation-artifacts/`；后两个 SP 包在 ROCK
各自同名 evidence 根下。SP combined sidecar `CERTIFICATE.json` SHA256 为
`bd60d91464725239e062ff84e8c5f5dda51ddf9863fec02c1ac3716469898e8c`；
精确双端路径见[SP 验证说明](../scripts/validation/loop_sp_snapshot_native/README.md)。

Python / 真实 B 包位于两端的
`loop-pipeline-python-real-evidence-control-20261115-v1/` 中，Mac 前缀为
`/Users/c/Desktop/deob/validation-artifacts/`，ROCK 前缀为 `/home/ubuntu/vmc-runs/`。
含 229 个 payload / 32,796,127 字节和一个 manifest，共 230 个 regular 成员。
双机证书 `DUAL-HOST-CERTIFICATE.json` SHA256 为
`0487420b156e98c2b0663662d0e5808f421cf55d396897f32e7ce53995a1b699`。
该包完整保存最初 Python 精度拒绝、准备失败、最终有限叶场景、direct / pipeline
兼容检查及 main / no-Z3 CLI 输出；它不把初版索引形状声明为已经修复。

## 最终八产品与源码归档

归档名为 `loop-pipeline-eight-products-current-source.tar.gz`，压缩大小
**358,448,047 字节**，SHA256
`fa93df0fd79d8a6969703874139ff18ff3f47bb6a55bf53067a1f91e75a7f9ba`。
双端位置：

- Mac：`/Users/c/Desktop/deob/validation-artifacts/loop-pipeline-final-sdk-source-archive-20261115-v1/loop-pipeline-eight-products-current-source.tar.gz`
- ROCK：`/home/ubuntu/vmc-runs/loop-pipeline-final-sdk-source-archive-20261115-v1/loop-pipeline-eight-products-current-source.tar.gz`

完整 1260 个 regular payload 成员 / 960,466,377 逻辑字节，包括四个 DSO、四个测试
程序、完整实现源码 / Git 快照、两个 SDK 清单、16 份原始完整测试记录及其它明确列出的
小证据和恢复说明。无链接、无内容归一化。独立 Python / real 与 SP native 包以精确
外部引用绑定，不重复嵌套；恢复时须同时保留对应外部包。

| 封存记录 | SHA256 |
| --- | --- |
| MANIFEST.json | `c4ed2007db6492e9c89a3d65ab6286c4a724fb1a43d47eb45ddea3d346ecdf50` |
| FINAL-CONFIG.json | `9f7e92ef9aea0a8fe7c39c2d9a28d2c8d3c3d63ebcbe912a7a9be87d787471d5` |
| DUAL-HOST-CERTIFICATE.json | `98bf8cfdb76a07f4975aecf6ea06a666a530a78c30034925d93e9429ee3e9e86` |
| 两端各自的完整解包 VERIFY 记录 | `3aab305828be02b523074dbf1537a45116714f49aad773386d9c2d45c8ce0599` |

证书为 6738 字节，位于两端对应前缀下
`loop-pipeline-final-sdk-source-archive-control-20261115-v1/DUAL-HOST-CERTIFICATE.json`；
CONFIG 位于同目录。两端独立完整解包核对成员集合 / 大小 / SHA、单 gzip CRC / EOF
及无尾随数据，均通过。归档原输入前后大小 / mtime / ctime / SHA 全等；381 个编译源
和八产物与完整测试绑定均成立。封存阶段无新增构建、测试、Guest 执行或删除。
它不是完整 OS、venv、LLVM / Z3 / Unicorn 依赖或目标程序环境备份；恢复只写入新目录。

## 可直接查看的真实样本结果

Mac 输出目录：
`/Users/c/Desktop/deob/validation-artifacts/loop-pipeline-cli-real-B-20261115-v1/main-output/`。
`00-raw.ll`、`01-guarded.ll`、`02-loop.ll`、`03-clean.ll` 和各阶段完整 JSON 均保留。
两后端的 kernel-only CLI 都在同一配置下闭合：733 块 / 16814 指令 / 1 switch
清理为 448 / 10900 / 0。这是带实际入口字节检查的独立 view；普通入口、未满足 guard
的回退和未知调用的保守效果仍然存在。旧的含 wrapper native 验证 IR 没有覆盖。

本轮完成统一入口、SP 精度修复、验证与交付，不宣称历史所有 cff/combo 或任意新样本
均自动去平坦化。特殊保护后续仍可由自定义 pass 处理。
