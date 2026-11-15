# 大样本循环去平坦化：独立执行验证

本报告承接 `loop-image-branch-milestone-20261115.md` 的代码检查点。
核心实现为 `5b748fe`，产品源码和默认预算在以下验证期间不变。

## 样本与输出范围

样本为 Flatten switch O2，ELF SHA256
`bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`。
所选 B 是带入口检查的独立分析视图，检查 SP、表内容和输出寄存器；
入口条件不满足时调用原函数。没有把这些条件当作任意调用者都满足的假设。

main / no-Z3 的完整产品 IR 字节相同，SHA256
`714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`。
原始分析函数保留，B 视图中循环内 switch 为零，仅保留入口 switch；
所有 73 个存活表读取点保留。证明工作量为 985771，低于原有 1000000 限额。
缺少输出寄存器检查的 A 仍保守拒绝。

## 名义输入：已经执行

冻结的 16 个边界输入分别在 O0 / O2 下执行。O2 先使用官方
`opt-21 -passes=default<O2>`，两级均使用 `llc-21 -O0` 和 `gcc -O0`，
与原始 IR 的校准协议一致。比较参考是已经保存的 Unicorn 完整状态，
没有重新生成或修改参考。

| 组 | 完整状态匹配 | 比较器故障对照 | 实际入口观察 |
| --- | ---: | ---: | --- |
| 普通去混淆输出 | 32/32 | 96/96 检出 | 无插桩 |
| 独立观察版本 | 32/32 | 96/96 检出 | 每次 accepted=1、fallback=0 |

每帧比较全部 66136 字节：24 字节头、512 字节原始寄存器、65536 字节
Guest 内存、7 个 Exit 字段及剩余执行预算；另保留原比较器的 76 个寄存器字段、
额外 64 字节和 ROOT_EXIT 检查。没有跳过、未定义排除或归一化。
每次正常匹配后，分别故意破坏 RAX、Guest、Exit，要求对应比较器报错。

观察版本使用两个单独计数器，记录真实 accepted / fallback 路径。
验证适配器仅把 wrapper 中唯一一条对普通 kernel 的直接调用改到 B；
B 的回退调用及普通函数体保持原样。生产 IR 文件没有被适配器修改。
这避免了 wrapper 继续调用普通函数而得到虚假的“去混淆结果正确”。

本阶段新增 64 次 native 执行，主 / no-Z3 的相同 IR 只执行一份；
不把原来的 32 次 before 校准、16 次 C 或 16 次 Unicorn 再计入本阶段。
源文件、输入、工具和动态库 SHA 在运行前后相同，外层守卫运行 32.294054 秒。

ROCK 证据根：
`/home/ubuntu/vmc-runs/loop-real-native-nominal-plan-20261115-v1/`。
`run/RESULTS.json` SHA256：
`b45c24f5c0be21c30d9e4e92e07e82093ba997ac42427ff198c9f36815c82ccb`。
`guarded-run/GUARD.json` SHA256：
`5041a4a32c6d77bc6967e70db86decc909e26238814c98bacf1b35e5cc2414c6`。

## 入口改变与运行时故障对照：已经执行

另取冻结 input0，显式记录每个变化字节，分别在 O0 / O2 执行：

| 情形 | 改变 | 执行及结果 |
| --- | --- | --- |
| 实际使用的表槽 48 | Guest[8576..8583] 的 i64 从 0x1741 改为 0xdead | 2 次原 LLVM 参考与 2 次回退版本完整相同；后者实际 0/1 |
| 栈指针加 8 | 寄存器偏移 32 的 RSP 从 0xc808 改为 0xc810；把原栈顶 0xf000 复制到新栈顶，保留原位置 | 2 次原 LLVM 参考与 2 次回退版本完整相同；后者实际 0/1 |
| 故意遗漏 wrapper 调用改向 | 只把验证副本中的唯一 wrapper callee 改回普通 kernel | 2 次完整状态仍匹配，路径计数为 0/0，1/0 验收门确实拒绝 |
| 故意把 accepted 计数改为加 2 | 只改验证副本中该计数器的常量 | 2 次完整状态仍匹配，路径计数为 2/0，1/0 验收门确实拒绝 |

表槽改变后的普通 LLVM Exit / budget 为 `[2,1,4390,57005,1,0,0,0]`，
Guest PC 为 0xdead；回退版本保持完全相同的错误退出。栈改变后仍正常返回。
改变输入的参考来自原始 LLVM 的同输入执行，没有宣称新的 Unicorn 真值，
没有把改变输入的 Exit 强制成名义 ROOT_EXIT。

此组新增 12 次 native 执行，其中 4 次是改变输入的普通 LLVM 参考；
4 次回退输出比较后另外执行 12 个 RAX / Guest / Exit 比较器故障对照，全部检出。
没有重跑或重计名义 64 次。所有源、输入、动态库和工具 pin 前后不变，
跳过 / 排除 / 数据归一化均为零，外层守卫运行 21.382076 秒。

ROCK 根：`/home/ubuntu/vmc-runs/loop-real-native-four-controls-20261115-v1/`；
`run/RESULTS.json` SHA256：
`8daaa66b2ac7c511f138818987ae93bed8fac415ed0124cbddf98ce25334eb21`。

保存结果的独立名义审计再次检查 64 个完整输出、32 个路径计数和 192 个比较器
故障记录，报告 SHA256
`17ab56274fa43e821abc46162e12bac8eaae9b457d009c2786604983003c96b9`。
首版审计器最后比较 Python tuple 和已保存 JSON array 时拒绝，原始失败保留；
新版只让重算的完整记录经过原有 JSON 序列化后再比较，并核实差异仅为容器存储类型。
没有改变 Guest 数据、比较字段、顺序或数值，没有重跑 Guest。

## 证据封存

上述机械适配、名义执行、四类控制、保存审计和原始失败已完整封存：

- 包 `loop-real-native-after-evidence-20261115-v1.tar.gz`，17606477 字节，
  SHA256 `dd241d5a105c4a825f09703b366e137a9ba0050848ea3ff37e9f4d27e92f9efa`。
- 941 个 payload / 39822744 原字节，含 manifest 共 942 个 regular 成员；
  没有软链接或硬链接。两端分别验证完整文件集、每成员大小和 SHA、gzip CRC / EOF。
- 最终双端证书 SHA256
  `0d6a19107da97c083af6b6645b5278755804a698bf23629b1e063366d0ea348c`。
- Mac 根 `/Users/c/Desktop/deob/validation-artifacts/loop-real-native-after-evidence-20261115-v1/`；
  ROCK 包 `/home/ubuntu/vmc-runs/loop-real-native-after-evidence-20261115-v1.tar.gz`。

实际执行的工具及历史失败源位于 `scripts/validation/loop_real_native/`，
完整六个产品与 25 个编译组件源另见
[SDK 封存报告](loop-image-final-sdk-archive-20261115.md)，不重复打包或重计执行次数。

## 固定 LLVM 清理：静态检查已经通过

只在 B 视图内按现有顺序各执行一次 SCCP、DFAJumpThreading、JumpThreading、
SimplifyCFG、InstCombine、DCE。每步执行 whole-module verifier，并严格检查所有
普通函数全文、Function / Argument 对象身份及删除所选函数体后的模块骨架。
六步均通过，没有增加新 intrinsic 的例外，没有修改生产源码、默认管线或预算。

| 结构 | 清理前 | 清理后 |
| --- | ---: | ---: |
| 视图基本块 | 733 | 448 |
| 视图 IR 指令 | 16814 | 10900 |
| 视图 switch | 1 | 0 |
| 循环内 switch | 0 | 0 |

清理后完整 IR 为 1280465 字节，SHA256
`8418fd314005fa8b771473ea56e907a352416b459246e4f0e39d796c55ee1d2e`；
报告 SHA256 `6ed3489244c19e6c28b039b455810e06e19beda59aaf33dfbf31d3f2321c6c31`。
ROCK 根为 `/home/ubuntu/vmc-runs/accepted-view-cleanup-real-B-20261115-v1/`。

结构审计完整跟踪原视图全部 419 条 simple i64 LOAD，清理后剩余 345 条；
这些包含寄存器、入口检查及业务读取，不能声称是 419 个跳转目标读取，
也不能把 74 条删除等同于 74 个分发读取的删除。之前的纯常量地址分类只找到
69 条候选与 80 条入口检查，严格 73 门因此拒绝，原 v1 / v2 源和失败保留。
新库存改为按 LLVM 物理指令完整枚举，不按名称或地址推断，也不提供任何数值事实。
原库存上限 512 和清理程序保持不变，覆盖扩大到完整原始读取超集。

清理程序实际运行 40.154 秒，六步静态验证全部通过。

## 清理后 IR：独立执行已经通过

只复用已经验证的唯一 wrapper callee 改向逻辑，生成单独 native 适配副本；
不要求清理后的 CFG 仍具有原入口 switch 或 guard block 布局，不改动生产 IR。
同一 binding-only 客户端的 9 个机械检查通过，包含有效 IR 的重复调用 / 错误 tag
拒绝及拒绝时整模块不变。实际适配只改一个 callee，普通函数、所选视图及全局
保持原样，whole-module verify 通过。

再次使用原 16 份输入、原编译链和原参考帧，O0 / O2 共 32 次执行全部匹配
66136 个原始字节、76 个寄存器字段、ROOT_EXIT 和预算；96 个指定比较器故障
全部检出。没有重计原 76 次执行，没有新增 C / Unicorn，跳过 / 排除 / 数据
归一化均为零。源、输入、工具和动态库 pin 前后完全相同，守卫运行 17.470695 秒。

本次没有新增清理后 accepted / fallback 计数器证据。原始 B 的 32 次实际 1/0
和守卫失败的 4 次 0/1 是前面单独记录的测量；官方六步清理的严格隔离检查、
单 callee 绑定检查及清理后完整执行对拍是本次新证据。

ROCK 根 `/home/ubuntu/vmc-runs/loop-clean-native-20261115-v1/`；
`native/RESULTS.json` SHA256：
`25f39675fe2acda33b153b8141c10fdbd9da4de33326c36ed5d177a687313ba6`。
生产 IR 仍为 `8418fd31…`；只供验证使用的绑定副本为 `a7870686…`。

保存结果的独立审计重新检查 32 份原始输入 / 输出、96 个完整故障记录及绑定的
单 callee 差异，报告 SHA256
`6272adb21a420a4697be6c43de4785adf56a292c087eb41c2d2695f720fafd0d`。
清理后的新执行证据包为 `loop-clean-native-evidence-20261115-v1.tar.gz`，
7012255 字节，SHA256
`a8e436b0afffa169c0339c61ffbd6b00c2c2516dc0d56d6e2fa14ff6fddda77a`；
304 个 payload / 20436013 原字节，含 manifest 共 305 个 regular 成员。
两端独立核对完整文件集、每成员大小与 SHA、gzip CRC / EOF，最终证书 SHA256
`090cb7408c2da7cd8879c9283361a03b87958c1c986a86883da0921eba5bc00a`。
Mac 包位于 `/Users/c/Desktop/deob/validation-artifacts/loop-clean-native-evidence-20261115-v1/`。

机械自测、独立 emitter 构建及实际六步 IR / 日志另存
`accepted-view-cleanup-own-evidence-20261115-v1.tar.xz`，7482008 字节，SHA256
`63b3f054dc5eb06799e22da2d223b5f44d39076630efa3d10a2fb87194935f74`；
完整 81 成员双端逐成员验证和单 XZ EOF 检查通过。清单准备与历史分类失败另存
`loop-accepted-view-cleanup-input-evidence-20261115-v1/`，78 个 regular 成员，
证书 SHA256 `6ec20de2817c3c4a939eba467a2ca4f794f9c76c9e1b9994502913ad2186c215`。
这些是各自不同范围的证据，不重复计算 native 次数；没有删除任何证据。

详细机械验证见
[清理工具报告](../scripts/validation/loop_accepted_view_cleanup/mechanical-and-cleanup-validation.md)，
独立执行见 [清理后 native 报告](loop-clean-native-validation-20261115.md)。

## 其余范围

完整测试结果见代码检查点：main 1317/1317、no-Z3 1171/1171。
没有新增单指令、完整 69 输入语料库或通用 VM 验证；本工作输出分析 IR，
不改写原始 x86 可执行文件。
