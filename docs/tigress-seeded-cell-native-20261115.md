# Seeded Guest cell 联合不变量：独立 native 验证

状态：独立 before 校准、坏 IR 控制、正式 main/no-Z3 after 与 saved 审计全部通过，
完整证据已双端冻结。本批不修改或重计 register-context v1/v2 的模型、输入、结果与失败。

新增 `scripts/validation/tigress_seeded_cell_native_{source.py,driver.py,probe.cpp}`。
复用现有 `dispatch_corridor_native.py` 的文件 pin、完整字节比较控制和
single-XZ/CRC/EOF 读取工具；业务 C、Python 规格及全部初始输入重新定义。
producer 只用已安装 `runClosedImageTableContext`，实际 client 的 API 符号是 UND，
没有链接私有 core 对象；不将两个参数的 full-fallback 模式冒充本批 native 覆盖。
`DRAGON_SEEDED_CELL_BEFORE_ONLY` 构建必须不包含产品 API 引用或定义。

本批 5 个 i64 cell source fixture × 8 个固定输入，共 40 个独立 module-input、8 个
初始物理 arena。每次最多 3 次实际 dispatch，边界词包含 0、1、全 1、符号位、
符号位前一值、交替位和 32 位全 1。初始 cell 值故意为 0x2000，与正确 seed
0x8000 不同；其地址为初始 SP−120，另有独立的 SP−48 状态字。

| Fixture | 新能力 gate | 实际行为 |
| --- | --- | --- |
| seeded-snapshot | 应接受闭合 | 初始 RSI 全字快照写入 cell；Guest[0x9000] 后写可物理覆盖 RegBytes RSI，循环仍读 cell 中旧 0x8000 |
| cell-clobber | 应拒绝且 IR exact | 保留回边单字节覆盖 cell+1，将指针 0x8000 改为表地址 0x2000 |
| missing-seed | 应拒绝且 IR exact | 输入单 bit 决定绕过初始化 STORE；另一实际 predecessor 不得取得 cell 常量 |
| unknown-call | 应拒绝且 IR exact | 外部调用实际改 cell 并记录调用次数；参考实现知道它的行为，分析器仍须拒绝 |
| terminal-old-fresh | 应接受闭合 | 终止块读 old、改 cell、读 fresh，分别留 trace，末尾还真实覆盖 table；终止 fresh 不得获得循环事实 |

每次比较全部 66,112 字节：Guest 65,536、备用或独立 RegBytes 512、全部 7 个
Exit 字段 56、guest return 8。活动 RegBytes 在部分输入中实际指向
Guest+0x8fd0；它不是两个独立缓冲区的模拟。Host wrapper 还逐项核对 State、
Memory、Context descriptor 的指针/数值身份未变，不做指针或 arena 归一化。
保留 seed、header、dispatch、业务、fallback、call、finish counters，实际 target
LOAD、bounds/error、UnknownTarget 和普通 root fallback。

实际执行顺序固定，全部 target CLI 使用 512 MiB 启动、64 MiB 运行下限、
180 秒、CPU 0–3 守卫，显式 pin runtime、Z3、LD 路径且清空 LD_PRELOAD：

1. `source.py --emit <new-raw>`；冻结源/40 组预期/8 个输入/10 份 LLVM。
2. before-only producer parse/verify，API 调用必须为 0。C 与 before LLVM 各
   O0/O2 执行并对独立 Python 规格校准：每 host level 80 个完整 matches，
   2,480 个逐字段破坏比较器控制；随后 saved-only 审计。
3. 在上述两档校准和审计全过后，执行五个坏 IR 变体各 8 输入：每 host level
   40 个 fault observations，18 个预先指定的错误必须在指定字节域检出。
   控制分别为虚构缺失 seed 值、漏部分写、terminal fresh 混成 old、跳 seed STORE、
   seed effect 重复执行（独立 counter，包括模 64 回绕输入）。
4. 新 SDK producer 每 fixture 只调用一次已安装 API；2 个接受、3 个 byte-exact
   拒绝，target/cell 实际 LOAD 各仍 1 个。之后 after O0/O2 各 40 个新完整 matches、
   1,240 个比较器控制，再独立 saved 审计，所有失败原样保留。

以上各阶段已按冻结计划完成。坏 IR 的未触发输入也照常执行，只将冻结的
触发子集列为指定检测，不当成独立业务样本；poison/undef 只由结构测试覆盖，
不执行 native。本批只证明有界 synthetic 联合 cell/table 语义，不证明真实
Flatten 全部循环已去除，也不把上下文复制数量当作闭环。

CLI 沿新 driver 的 `--prepare-before`、`--native --phase before|fault|after`、
`--prepare-after` 和 `--audit <saved-root>`。审计的 `--out` 必须是新绝对路径，
不能向原 RESULTS/stream 或旧 v1/v2 namespace 写入。

## 实际结果与边界

| 独立阶段 | 完整状态 matches | 比较器控制 | 坏 IR 观测 / 指定检出 | API |
| --- | ---: | ---: | ---: | ---: |
| before O0：C40 + LLVM40 | 80 | 2,480 | — | 0 |
| before O2：C40 + LLVM40 | 80 | 2,480 | — | 0 |
| 坏 IR O0 | 不计正常 matches | — | 40 / 18 | 0 |
| 坏 IR O2 | 不计正常 matches | — | 40 / 18 | 0 |
| main after O0/O2 | 40 + 40 | 1,240 + 1,240 | 外部引用已完成控制 | 5 次 prepare |
| no-Z3 after O0/O2 | 40 + 40 | 1,240 + 1,240 | 外部引用已完成控制 | 5 次 prepare |

新 after 合计160 matches、4,960 controls；before 的160 matches /4,960 controls、
80 fault observations /36指定检出只计其独立一轮。每档坏 IR 有22个未触发而保持相同
的观测，也完整留存；40 fault observations 对应32个不同 `(source,input)`，其中
同一个 seeded-snapshot 的另一 mutant 不是新业务样本。所有重复 host/build 仍只对应
5 个 source、40 个独立 module-input、8 个独立初始 arena。

before 四组 stream 共240帧、after 四组 stream 共160帧，8组 saved 审计全部通过，
含 single-XZ/CRC/EOF、完整域、input/expected/actual SHA 及指定错控字节。
无 skip/exclusion/normalization，未新跑 Unicorn、单指令或全 corpus campaign。

正式运行库与代码绑定：

- Image core `5035b62ed532e94d837f8a6d64b4194bc86c06d5075712b15bc7a7885b7156a1`。
- main `4e4ad910cb3991826b948c5a246431ee98bc95691b4c88745b3108c7f6a7c198`；
  实际加载 pinned `libz3.so.4.13`，SHA
  `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`。
- no-Z3 `9d85ea3722578a4f2d58f14da976bac7cc2dbb41b26eb53d6abd9be22e017f70`；
  不传 solver，全部5次 API 的实际 maps 均无 Z3。
- 每个 backend 都实际5次 installed API：2个接受、3个 byte-exact 拒绝；target/cell
  循环 LOAD 各仍1个，ordinary 定义与 Function/Argument 身份保持。
  五个 main/no-Z3 最终输出 IR 全字节相等，未做 ModuleID 或文本归一化。
- before-only provider 是从既验四 DSO 包独立恢复的旧 a869，API0；不把旧库提供 LLVM
  解析符号说成旧产品变换已通过。新旧 code 的 C++ 精度门槛另由项目测试报告负责。

只执行了固定 PC=4096 的输入域；未声称直接 ordinary/unsupported-PC 变体已覆盖。
terminal fresh 紧跟真实 STORE 可被 LLVM 合法折常为8192，native 检查的是新旧值的
最终语义，未声称 terminal LOAD 自身必须留在机器码中。

首次恢复工具 v1 缺 archive 顶层前缀，v2 因先前空目录触发 mkdir 拒绝；均在编译、
LLVM parse 和 guest native 之前失败。两份 source/guard/log 与之后独立 v3 成功记录
完整保留。没有改 fixture、模型、输入、期望或比较器使它通过；所有实际 native 阶段无失败。

本组件仍只证明单常量 cell 的 joint closure。实际大 Flatten B 仍因动态 work-array
写可能覆盖指针槽而拒绝；变化索引 cell 的区间不变量没有实现，不能凭源码最终 mask
或上下文复制数量宣称真实循环已完全去平坦化。两个真实 index 槽虽有支配初始化，
初值却是输入相关 `zext32(x xor 0x507f1d47)`；业务 state 的后续0/`&7`写不能升级成
全局 `[0,7]` base。RBP 初始化循环的 encoded-state bound 是另一个未消除障碍。

## 冻结证据

ROCK 前缀 `/home/ubuntu/vmc-runs/`，Mac 前缀
`/Users/c/Desktop/deob/validation-artifacts/`。同名 roots 全文件集合/大小/完整 SHA
双端一致，source stat/hash 前后稳定，无 symlink：

- `tigress-seeded-cell-native-20261115-v1`：443 files /312,099,079B，含独立恢复 a869。
  before 外置 `-mirror-control/DUAL-CERTIFICATE.json`：
  `3333ad0089baf002c0ee67b2d1dde662a535278d831753571a085460fb1c2663`。
- `tigress-seeded-cell-native-after-main-20261115-v1`：168 files /32,909,880B；
  SUMMARY `a0ce08f82ffc898a9a70676f2f9b6d443f014ebcdc669d3cd3cee6f0c9e6507a`。
- `tigress-seeded-cell-native-after-noz3-20261115-v1`：167 files /33,068,910B；
  SUMMARY `24609f4ba1f772e66e1319f79b4254aa3c0304ee9e28b8a9afc21f293327ed49`。
- `tigress-seeded-cell-native-after-dual-20261115-v1/DUAL-CERTIFICATE.json`：
  `5be28375f855fced9745fb660667955bf2b5efa3aaf0a05cfe94cd3fb6a6a990`。
  此证书覆盖两套 after 共335 files /65,978,790B；before 按上项显式引用，不重包或重计。

target 窗口已结束并释放 SDK pin。后续历史回放必须从 root 的精确 runtime 包恢复至
独立 namespace 并显式 LD，不可沿旧可执行文件 RPATH 误用已重链 SDK。
`seeded-cell-final-runtime-products-20261115-v1` 的9成员包 SHA 为
`816d87f65ad268283e985d638f8d3654b09efd3a613bc8b60f8bcb2b23b6d14a`；
这是 parent 独立验证的运行库/测试二进制/3源件包，不是完整 OS 或依赖环境快照。
