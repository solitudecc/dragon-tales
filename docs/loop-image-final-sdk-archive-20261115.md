# Loop Image 最终 SDK 产品封存（2026-11-15）

本包保存实际 main / No-Z3 各一个完整 backend、shim 和最终 Catch 测试程序，共六个完整构建产物（四个 DSO、两个 test executable），以及 25 个编译组件源文件。它不是完整当前 Git 树、build tree、OS 或依赖环境备份。原 SDK 清单 `fdd8ddddbe64684ccaedb5f09eb1179b0183273dfc5b0efd49618c58c76ae932` 原样收入包内。

- 包：`loop-image-final-six-products-and-25-sources.tar.gz`
- 压缩大小：335,829,226 B；SHA256：`5fc54ef2f322b4ff71075cbd46da4a1d369c822c64a1dd5eeb9bf04352f6c529`
- 完整 regular 成员：45 个，896,219,980 B 原字节；43 个原 SDK 成员加两份 scope / restore 说明，无 link。
- 成员清单 SHA：`eafffad441f580b8617f5ca40cf8576257a5731c6e4244ab8c2ffc101d54fefd`
- 双主机证书 SHA：`69698410b21423439ff2db47454b68ee0f61a6e48d20bfe9713165be4be3d46a`

ROCK 包位于 `/home/ubuntu/vmc-runs/loop-image-final-branch-sdk-archive-20261115-v1/`，Mac 镜像位于 `/Users/c/Desktop/deob/validation-artifacts/loop-image-final-branch-sdk-archive-20261115-v1/`。两端对应的 `loop-image-final-branch-sdk-archive-control-20261115-v1/DUAL-HOST-CERTIFICATE.json` 记录各自独立执行的完整成员集、大小、SHA 和单 gzip CRC / EOF / 无尾随数据验证；两份验证 receipt 均为 `96e459788ae12dcbbfb7ac1c57e1a1749dfb030e3d11bf6bdeda1e3978466ffe`。

| 产物 | main SHA256 | No-Z3 SHA256 |
|---|---|---|
| backend | `d446226f9ff402bf1e096e197993aa8f2301d2ab81af3c573b50ae13ed2e0ed4` | `d98b8e934cbbd861c3353e092b891e7a5f19534ad42aa0d8788581bb56762bec` |
| shim | `9d95c6402f624f277900746e3493f2e65039a8e51b93561fde66454e84583efa` | `e29884c99128c637a5e1469ec5320288a7d42bd31dc640015a98c6747a3a6675` |
| dragon_tests | `e38d19a512328ac4306fbbff52b06799a6f744e226295a4e07b9720a0b6607f6` | `9c6225ae0fad5bba87817de00ffc1d4cc235b2c9775063a2a63c3b0a56103b90` |

打包按 CPU0–3、nice19、idle I/O、单 worker gzip3 执行，guard 69.08s / exit0；ROCK 独立解包验证 guard 21.28s / exit0。源成员逐文件 pre/post 的 dev、inode、nlink、mode、size、mtime、ctime 与完整 SHA 精确一致；两份源 receipt 原字节同 SHA `d7be607c1232ce36e62bb463f07984d5207299dd951da0c12ed7e50524bd47b2`。没有修改冻结 SDK、active build 或任何源码，也没有删除任何文件。

完整 suite 结果另在已双端验证的小证据中：main 1317/1317、No-Z3 1171/1171，零失败、零跳过，证书 `5ef6e9c60cb5045c999842c2275f4411af297529425ce2078dc5851190105d65`。本次封存没有重跑 build、pass、测试或 Guest native，不重计独立 native 证据。旧严格函数集合门失败和后续校准成功的原记录均保留。

恢复必须先核压缩 SHA、完整成员集和每成员大小 / SHA，再恢复到新的 versioned namespace，禁止覆盖 active build。显式设置 `LD_LIBRARY_PATH` / `DRAGON_C_LIB`；历史 RPATH 不是运行库绑定证据。main 的精确外部 Z3 为 wheel `libz3.so.4.13`，SHA `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`；系统库、LLVM / Python / Unicorn 环境未打包。完整前序实际源树在 `bd8d9b615ef7796fee22b47e2d78ae14242a45677a8dc44fdc932c604ea3311b` 包内，本包仅保存其 25 个组件替换 / 新增源；不以这些文件冒称完整 checkpoint Git 树。
