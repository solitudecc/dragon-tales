# Loop image：有界变量移位同客户端回归（2026-11-15）

本轮用同一 public API `detail::runLoopImageTableContext` 和同一个已编译客户端，验证 LoopRanges 对有界非单值移位计数的精度补齐。旧固定 SDK a5d 的 7 个新增阳性失败，新固定 SDK 777 的 15 个场景全部通过。阴性、IR 输入、阈值和后置检查在两次执行之间完全相同。这是 C++ 分析精度及保守拒绝回归；没有新增目标函数原生执行、Unicorn 或完整 CTest。

## 固定输入与测试结构

测试源为 `tests/lifters/TestLLVMLoopBoundedVariableShifts.cpp`，最终 16,229 字节，SHA256 `0643d0d75018aad063cf51e8f98d9a46799c1abd126f8fa1011dcf9d9a1b8a9a`；过滤 tag 为 `[image-loop-variable-shift]`。两条 TEST_CASE 分别包含 8 个阳性与 7 个阴性场景。

Fixture 的 entry 检查 SP、输出寄存器及两个 Guest 表项，普通 fallback 仍调用 `ordinary`。受检查的 C cell 位于 Guest4096，表位于 Guest8192，移位初始化器写入独立 E cell Guest1024。B 的真实工作 STORE 地址为 `3928 + 4*E`。计数掩码来自实际 C LOAD 与常量的 xor；只有在已证明的 C 上下文内才成为常量，防止普通前端提前折叠掉待测移位。

每个场景先在另一个 raw Module clone 上独立运行官方 `PromotePass` 和 `InstCombinePass`，验证 `%shift.value` 仍是实际的 LShr/Shl/AShr，计数仍非 ConstantInt，`%e.b` 仍为真实 LOAD，且原始 Module 字节不变。undef 阴性还要求此控制版本中有可见、非 poison 的 UndefValue；本次控制实际通过，未把优化掉的 undef 计作覆盖。

阳性检查闭合、实际改写、未耗尽预算、Function/Argument 身份、caller named metadata 身份、原 Guest STORE 数量、实际 E LOAD 与移位的存活 uses、保留的 target LOAD 以及普通 fallback，并检查没有新增 assume、unreachable 或 private tag。阴性检查不闭合、不改写、整个 Module 文本以及全部 Function/Argument/Block/Instruction 身份不变，Guest STORE 与 metadata 不变，且 verify 成功。原有 1,000,000 work 上限不变。

## 范围和边界

本轮针对的数学传递是：合法计数区间 `L..H` 必须满足 `H < width`。LShr 对 `[lo,hi]` 给出 `[lo >> H, hi >> L]`；Shl 仅在 `hi <= unsigned_max >> H` 时给出 `[lo << L, hi << H]`，否则保留 Full；合法 AShr 可以保留 Full。区间不等同非 poison 证明，也未新增 noundef 或 freeze 承诺。

阳性包含 i32 计数域 0..28 与 0..31、i64 的 0..63、`31 >> count(0..3)` 不靠末端 AND 的界、`3 << count(0..3)` 不靠末端 AND 的界、AShr 后有限掩码、可能无符号回绕的 Shl 后有限掩码，以及原单值计数 2 控制。

阴性保持可真实写中受保护字节的端点：`84 >> {1,2}` 包含 E42，可碰 C4096；`2132 >> {0,1}` 包含 E1066，可碰表8192；`21 << {0,1}` 包含 E42。另有 i32 计数域包含 32、i64 包含 64、i32 超界域 0..63，以及可见 undef 计数。两个右移端点阴性同时防止上、下界错误取计数端点；左移阴性防止错误只选最小计数。

## 实际执行结果

| 固定运行时 | TEST_CASE | 断言 | 失败 | 场景结果 |
|---|---:|---:|---:|---|
| old a5d | 2 | 553 | 1 case / 7 assertions | 新阳性 modes0–6 在 `result.closed` 失败；原单值控制和全部7阴性通过 |
| new 777 | 2 | 763 | 0 | 全8阳性、7阴性通过 |

两次 JUnit 均 errors0、skipped0。旧失败消息均为 `loop STORE may overwrite a protected table or cell byte`。旧结果说明缺少证明能力，没有执行原目标程序，不能据此声称旧运行语义错误。新通过说明本 TU 的精度、effects/identity 与保守拒绝门通过，也不意味着真实大样本已完整闭合。

编译仅一次，32.107 秒；链接一次，3.213 秒；两次 focus 都为 0.404 秒。客户端 SHA256 `fcd67ba33e83e36ec9f6856b21f0a731a85704cd0c0edf9fbed19d0e1137ccdf`，object `373a1102b04a382c18321d3f4111dcdb82e141996051e7989c9bc2215c5639e4`。after 没有重新编译或链接，仅显式 LD_LIBRARY_PATH 改为新固定库。

只链接测试 object、Catch2Main、Catch2 和固定 SDK，没有实现 object、TEST 宏或共享 LLVM。`nm` 记录客户端对应 API 为 UND，SDK 为 T。`LD_DEBUG=libs,files` 记录实际初始化固定 SDK 与同一个 Z3.so.4.13，没有共享 LLVM；本小测试没有捕获 `/proc/self/maps`，不能把 loader init 证据称作 proc maps。

- old SDK：`/home/ubuntu/vmc-runs/loop-image-mask-sdk-20261115-v1/lib/libdragon.so`，SHA256 `a5d90833dbeb137c74e783e63e28c5efdf7898e7a37825fd685000079b800a6c`。
- new SDK：`/home/ubuntu/vmc-runs/loop-image-variable-shift-sdk-20261115-v1/lib/libdragon.so`，SHA256 `777a373f959f3708bd0f8257de9c0181fea2949f32ad157e74f345b228521c38`。
- actual solver：bootstrap venv 下的 `z3/lib/libz3.so.4.13`，SHA256 `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`。

CPU0–3 单编译任务，单命令180秒、启动可用空间512MiB、运行64MiB守门均保留。其它代理可用独立核做非 Z3 小探针；这不是正式完整 suite/MBA 的独占计时结果。

## 保留的首轮夹具编译错误

最初 v1 源 16,225 字节，SHA256 `9e623664e696b1f743502b0a032ab7d8f6d842f65ad8994b69d72e6d9c22800d`，因两条 Catch2 REQUIRE/CHECK 的裸逻辑 OR 编译失败，28.293秒返回1。没有生成或执行 unit 客户端：0 TEST_CASE、0 API、0 Guest-native。v2 仅给这两个表达式各外加整体括号，共增加4字节；完整补丁、v1 源和编译 stderr 保留。IR builder、15模式、断言逻辑及阈值均未改变。不能将此编译失败计作旧 SDK 回归失败。

## 完整证据与限制

ROCK 的三个闭合目录为 `/home/ubuntu/vmc-runs/` 下：

| 目录 | payload 数 / 字节 | FILESET SHA256 |
|---|---|---|
| `loop-variable-shift-before-20261115-v1` | 9 / 35,408 | `e7bd55bc4edde08e73c006c3857cabe47662eb0e2834bb7079e2bdce2b2570b2` |
| `loop-variable-shift-before-20261115-v2` | 31 / 26,231,455 | `6cd890b4d45b6e4ea5846369a7ca135a6067cc32a20a54c52c6083708b582264` |
| `loop-variable-shift-after-20261115-v2` | 10 / 34,460 | `4ab36569daa344fa43ec038272214470f3f07f61815802649d931bc3648269f4` |

Mac 对应完整镜像为 `/Users/c/Desktop/deob/validation-artifacts/<同名目录>`，包含源、输入 pin、计划、全部编译/链接/loader/JUnit 原始输出、object、客户端及失败原件；外部工具和运行库用完整路径与 SHA 绑定，没有重复复制成测试 payload。全部成员名集合、大小与 SHA256 已双端完整核验，三目录 frozen/no_more_writes。双端证书在 `loop-variable-shift-campaign-20261115-v2/CERTIFICATE.json`，SHA256 `fa4aeb9d5410ad775724b83231b82a0bc034fa278231110b980b8aa1f574e90b`。

本报告不重计其它客户端/旧 Mask 或 observation-range 的测试数。新增 Guest-native0、Unicorn0、No-Z3运行0、完整CTest0，未新增排除或归一化；正式双配置、完整 suite 及真实大样本闭合/原生验证由后续独立记录报告。
