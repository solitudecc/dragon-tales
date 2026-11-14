# Image 表证明消费入口寄存器快照

基线为 `281d4b2d16b87e1557560d92b38eb916489d01c8`，工作分支
`feat/tigress-dispatch-context`。这是显式 C++ ImageTableContext 的精度修复，
默认分析、优化流水线、C/Python ABI 和证明预算不变。

## 问题与修改

GuardedEntry 已能在入口检查完整寄存器 word，例如输出指针 RSI 等于候选地址。
Image 表保持证明原先只消费 SP 检查，其他已经执行的入口检查没有进入证明。
因此，即使后续写地址使用同一 RSI SSA 快照，仍会被判断为可能覆盖跳转表。

现在只收集实际成功检查链上的完整整数相等事实：必须是 raw entry 中的
simple LOAD，地址来自实际 register descriptor 的 byte GEP，宽度为整字节且
不超过 64 位。事实以原始 SSA 观测标识，并跟随私有 LLVM 规范化的等价替换。
在私有证明及完整闭合后的发布阶段，仅替换 accepted block 支配的该观测使用。
冲突值、partial/cast 检查、undef/poison、另一次 LOAD、Guest 重读均不获得事实。

这没有证明寄存器存储地址不变。Guest 可以与寄存器存储物理重叠：入口已经读到的
SSA 值仍受检查约束，后续再读同一个地址必须重新证明。未知调用和可能改表的写
仍然拒绝闭合；失败不改变模块。原 target LOAD、guard fallback 和错误路径保留。

独立 native 校准另外暴露了旧 SP 记录的精度缺口：`frame = initialSP - 64`
仅用于 `stateoff = frame + 16` 时，InstCombine 可以合并为 `initialSP - 48`，
删除不再使用的 `frame`。WeakTrackingVH 能跟随替换，但不保活一个没有真实用户的值，
因此旧实现报 `SP observation did not survive normalization`。该 fixture 满足
原入口及 SP 保持合同；没有通过更换 fixture 来避开拒绝。

现在仅在私有 normalize 的 mem2reg / InstCombine 期间，用 LLVM 21 的
`llvm.fake.use` 暂时保留各 SP 观测的实际使用。随后按本次创建的 call handles
删除这些使用，仅清理本次新增的空声明，不按函数名删除用户 call。进入表选择及
任何证明前，临时标记已全部移除；不再运行 DCE，后续显式常量折叠跟踪原观测。
musttail 块把观测和标记放在 tail call 前，保持 call / ret 相邻；私有 promotable
SP alloca 不会逃逸到 callee。所有额外处理计入原 shared work，不改变上限。

三个额外 SP 回归在两套修复前运行库 `435fcf7f...` / `51047237...` 上分别实际执行：
每套 3 cases / 52 assertions，2 cases / 4 assertions 失败，四个消息均是上述拒绝。
两库哈希执行前后相同。主库修复后 SP 3 / 78、全部 image 12 / 1359 已通过；
覆盖无外部 SP 使用、原有 unused intrinsic 声明身份和属性、原业务 call 保留。
最终测试 SHA 为 `ae0cbe324c17351bb1b6a1ff9d893d2380f24803b1b393a84f6b706b1f750a32`。

另外从已核验五产品包恢复 baseline `281d4b2` 的完整 `28ddd0d8...` SDK 到独立
namespace，用同一新测试程序执行 SP 三例：仍为 3 / 52、4 个失败断言。两个重载
均是 test executable 的 undefined API imports，实际 loader 只初始化恢复的旧 SDK，
没有覆盖活动产物。首次额外 admission 把两个重载误计为一个，在 target 启动前停止；
新 v2 明确核对两条原型均 UND 后完成这次旧基线执行，未改变测试或预期。

## 回归与主构建

最初三个寄存器用例在旧 SDK 上实际编译并执行，仅应用测试补丁：3 cases / 352 assertions，
2 cases / 8 assertions 失败，阳性被旧证明以 `STORE range may overwrite a protected
table byte` 拒绝。不是旧执行结果错误，也不是缺 API 导致无法编译。
该版测试 SHA 为 `096869dd5bed7f864f60ae341d243b056524a02d722593cc7fc2d299a0d3b9c6`。
首次较小测试的失败证据也保留，没有覆写。

寄存器修复 v1 ROCK 主构建：focused 3 / 402、相关 9 / 1281 全通过；完整 CTest
1236 / 1236，通过数包含三个新用例，failures / skipped / disabled 均为 0。
完整 CTest 用时 192.762 秒。测试涵盖旧快照及物理别名写后的旧快照阳性、16/32/64
位完整检查，以及缺失、部分、fresh guard/use、反向条件、冲突、unknown call 和
undef/poison 拒绝；full-fallback 与 closed 两种显式模式均检查。

主运行库由 `28ddd0d889f8ae6e28ad8a41abfbd69a811600e90f40db2e3e1ac4a12b1e48e6`
变为 `435fcf7fac5563cb9ddc1a02437f7c22f022e6420acd8c1ddb6a1ab8dc622b4d`。
实际 solver 为 `libz3.so.4.13`，不是仅凭链接输入 `libz3.so` 推断；两文件完整 SHA
均为 `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`。
旧五个完整产物已经双主机归档，随后才重建库。

最终包含 SP 修正的主构建完整 CTest 为 **1239 / 1239**（192.758 秒），无 Z3
完整 CTest 为 **1093 / 1093**（166.054 秒）。两边 failures / skipped / disabled
均为 0，SP focused 各 3 / 78、image 相关各 12 / 1359。No-Z3 是 main 的严格
注册子集，146 个 main-only、0 个 No-Z3-only；没有修改 CMake 或删除测试。
无 Z3 首次完整冷构建 1144.346 秒，早期 CPU0–3、后期迁移 CPU4–7；最终增量在
CPU0–3 用时 157.530 秒，随后测试使用 CPU4–7。不要把冷构建记为全程大核。
No-Z3 backend / shim / testbin 的 DT_NEEDED 无 Z3；独立 load-only 实际映射及
focused/related loader trace 也确认无 Z3。没有声称审计 full suite 的每个子进程映射。

最终主 backend 为 `a8690acf6be36264fd628aa1ebdb8075605f452327609aa91fae63e4f35b1de3`，
无 Z3 为 `b081c487ab301b8b8752c7fc6bab84f5fbdf6b07a80009a0713654f6216e8882`。
实现源码 SHA 为 `e0c7768d1098a5e2abc619c1b73be41dd5b64d1e580c244ce0933e16ee137455`，
两次修正均不增加公共 API 或改变 request / stats 布局。

独立 main native 先通过 C48 / LLVM48 完整 before 校准，再执行坏 IR 的 48 份
观测、16 / 16 指定错误检测；首个 v1 after 因 SP 记录消失拒绝，原始失败仍保留。
同一 fixture / model / input / comparator 下，最终 after 的 snapshot 阳性闭合，
fresh LOAD 与 unknown call 两例拒绝且 IR 逐字节不变。O0/O2 共新增 48 个完整
66,112 字节状态匹配，1200 个比较器坏值控制通过；加上 before 合计 144 个 normal
matches、3600 controls。所有 saved-only XZ/EOF/全帧审计通过，没有跳过、排除或归一化。
这是 main 的独立 native 验证，不声称新增 No-Z3 native 验证。
详见 [独立验证文档](tigress-register-context-native-20261115.md)。

## 较大 Flatten 样本的边界

当前样本为 `ida-review-20261114/flatten-switch-O2.elf`，SHA
`bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`。
它有 80 个表槽、75 个不同目标。原 ordinary Graph 没有解析中央间接跳转；已校准的
private decoded region 才完整覆盖这些候选。旧十步输出仍有一个中央 switch，位于
208-block 业务 SCC。此基线不是三个新组件实际运行后的失败结果。

新适配器从实际 raw LOAD/GEP/switch 结构发现表、从真实调用与输入提出 SP/RSI 候选，
再生成可执行 guard；ELF 表内容和输入值都不是无条件内存事实。旧库 A（SP+表）与
B（再加 RSI）各实际调用一次 closed Image API，均以可能覆表 STORE 拒绝；没有
budget exhaustion，拒绝模块字节、普通函数和参数身份保持。独立旧 raw LLVM 的
O0/O2 共 32 个新完整快照、96 个字段坏值控制通过；旧 C/Unicorn 参考没有重新计数。

最终主库上同一适配器 A/B 再各调用一次，使用原编译客户端及相同 source/header/input/
guards/caps，实际绑定 a869 backend。两例仍以可能覆表 STORE 拒绝，保留 35 个前序
写证明；work 为 128584 / 128611，未耗尽预算、未发布常量、完整模块字节相同。
这轮只验证实际库的静态 admission，不新增真实样本 native/C/Unicorn 次数，也没有
调用 Decoder/Corridor。原统计无法确定唯一失败 STORE，不把这个消息直接等同于某一
特定重读的因果证明。最终真实 probe 报告 SHA 为
`8bee6cce43972bef3596bcff86679261a32b040e45f7d929d2b5dca5cae38829`。
完整两次新库 admission 的 41 payloads / 14,049,202 字节已双主机核验，见
[适配器报告](../scripts/validation/tigress_dispatch_context_probe.md)。

本修复不能把入口检查延伸到栈中的指针重读。实际样本会把初始 RSI 存入 Guest 栈，
循环内再从该槽读出并用于后续输出写。该重读需要单独的 seed STORE 与所有可能写
的保持证明；`Ranges(load)` 仍按未知处理。之后还需处理 bounds 自环的原边证明，
以及有限状态表达式中的 poison flags。不能忽略回边、扩大预算或把 context copies
称为完整循环去平坦化。

本轮没有新增单指令 Unicorn、完整 69 输入语料库或全程序内存分析；本改动不涉及
机器指令语义。native 验证覆盖的是明确列出的 LLVM 模块和完整物理状态协议。

## 证据与恢复

ROCK 根为 `/home/ubuntu/vmc-runs/`，Mac 根为
`/Users/c/Desktop/deob/validation-artifacts/`，以下同名目录已完成完整 payload
集合、大小和 SHA 的双端核对，失败没有被成功结果覆盖：

- `tigress-register-context-builds-20261115-v1`：五组原始构建/测试记录、源与补丁、
  旧 origin SDK 交叉检查；61 payloads / 2,330,741B，双端证书
  `5aa15bc0b96df6038ad9d79657eb77fd70526b658b04171ceed9619c6b76311b`。
- `tigress-register-context-native-20261115-v1` / `v2`：完整独立校准、坏 IR、首次
  after 失败与最终 after 成功，证书分别 `aa068ab5...` / `fa24ff87...`；完整值见 native 报告。
- `tigress-register-context-noz3-20261115-v2`：六个完整产物及完整无 Z3 验证；
  48 payloads / 428,251,359B，证书
  `24f7c06c271816f07d34913106f0511fad7108aabad8d18d4a559c4cc80fdc52`。
- `tigress-dispatch-context-final-20261115-v1`：两次真实新库 admission，证书
  `ad8fc897b03af83e4ea33720c7171ff8bf7e3c80d1441a5466c6d060cfd0fcea`。

最终四个完整 DSO 另在 `tigress-final-sdk-products-20261115-v1` 备份：main 与
No-Z3 的 backend/shim 共 438,861,760 原字节，压缩包 154,034,895B，SHA
`d76bc84e34a90341c45ce8fedf9a82ccb8c1f8a0722655d02d924828a5313e34`。
29 个明确 regular 成员包含四库、三源码和已完成的指定 metadata；两端通过全部
成员集合/size/SHA、单 gzip CRC 到 EOF / 无尾数据检查，源 stat/ctime/SHA 前后相同。
双端证书为 `b87e53f8a9ad01cdc51e70231ced9edd695598401b4d7ab16b81eb48dce7a33a`。
该包不含测试程序、OS、全部依赖或无 Z3 full 输出；后者在上述独立验证树。
只能恢复到独立版本目录，不能覆盖活动 build。封存不运行 target，也不删除原文件。
