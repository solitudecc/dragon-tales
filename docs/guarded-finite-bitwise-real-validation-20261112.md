# 有限位运算状态候选：真实验证

本阶段只更新 `guarded-state-corridors` 的有限候选求值实现。四个冻结 ELF、
原 16 输入、原完整 C/LLVM 比较器，以及 F 阶段 45 步配方和默认预算均保持。
新 backend SHA256 为 `7b76a0a8df46fb41077133dc9569b3690b0b41b76f51081b41050e144ce58c9b`，
shim 为 `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`。

正式运行 177.142 秒，C16/16、LLVM 完整状态16/16匹配；寄存器16、Guest16、
七个 Exit 字段112，共144/144故障控制检出。原16输入逐字节一致，四个 Graph 不变，
source/runtime/harness/fixtures/contracts/extras 前后绑定均一致；无新增排除、归一化或跳过。

正式预注册：`597ae378e14292bf044a7d7f7dd5b9d1ea2b430f29bcba03390660e9d94376a8`。
外层执行 adapter 仍是 F 已冻结源，SHA256
`b0e994e0d6981f309d410098c7b5bf45678d0bdd2c70f795f60835d6ec5a21af`。
配方为原 D37，加 F1、tiny5、constant-branches1、E1，共45步；F仍为
64 candidates / 32 rewrites / 128 blocks / 16384 instructions，没有为结果提高预算。
原三轮 Guest-memory 12次 incomplete /12 budget events，dispatch-specialize
12次 incomplete /16 events，如实保留。

## 实际结果

| 样本 | F pass 改写/尝试 | 新块/指令 | complete | budget events | 最终小集合数 | 严格闭合 |
|---|---:|---:|---:|---:|---:|---|
| f-cff O0 | 0/0 | 0/0 | 1 | 0 | 1 | 是 |
| f-cff O2 | 18/18 | 67/2350 | 1 | 0 | 19 | 否 |
| f-combo O0 | 1/2 | 7/64 | 0 | 1 | 1 | 否 |
| f-combo O2 | 22/34 | 108/2599 | 0 | 1 | 23 | 否 |

表中上下文数不是独立状态数，complete 是本次有界分析报告，不是去平坦化完成。
相对 F，cffO2 的 24/25 位运算候选新增一条通路；两个 combo 的有限移位/OR 表地址
得到处理，但仍不是完整循环去平坦化。

cffO2 新首残留为 `bb_66220 → memory.valid71 → bb_65712.bb_65718_crit_edge →
bb_65718 → memory.valid15`。独立诊断见实际未知 i1 经 zext 与同条件 select/OR
形成有限值，后续组件再处理。comboO0 首入口状态50边成功，仍有状态0边及后续
`bb_89568 → bb_68277 → memory.valid131/133 → bb_68301 → memory.valid135/137`；
整体 budget 报告不能代替每条边的拒绝原因。comboO2 新首路径经过 CALL 后的寄存器
reload、加一与 select，再到 `bb_66032 → bb_66038 → memory.valid49`，不能凭结果
把未知调用效果当常量。逐边诊断与后续方案不算本阶段已实现收益。

## 分发器归类和实际编译绑定

本轮独立审计增加“完整 case 值集合 + 实际 default Exit+16 源 PC”的 CFF family
识别。旧 default 名称签名会漏两个 combo 的 entry-prefix 改名 default；它们的
PC 仍分别是 68316/66038，而自然业务 switch 为80519/67571。cff O0/O2 的 CFF
PC为66264/65718，业务为67197/66375。新增分类将这些 prefix 全部计入，旧 F
闭合1/4结论没有改变。没有因为相同 decoded case 集合而合并自然业务 switch。

新分类不是生产语义假设。每个实际 fast set 仍须与同次真实 LOAD 的完整 fallback
共享 selector，所有 fast case 目标匹配当前完整 fallback；只有已验证失败边可从
快路投影排除。普通条件与业务 case 全部保留。遗漏 prefix 中央计数、错误 PC、
非 Exit 指针及原分类/16个结构故障控制都检查；未知 family 不能被静默忽略。

正式输出逐字节链路四份全部通过：追加 pass 的 `module.after.ll` 等于导出 raw，
原唯一首 kernel 调用绑定后等于 `after-bound.ll`，并等于原生实际使用的
`oracle/<case>/<level>/s0-product-O0/raw.ll`。同目录 `commands.jsonl` 的实际
`llc-21 -O0` 命令绑定该文件。旧首槽70032 guard/bounds→bb67010及bridge控制保持。
没有修改比较器，也没有把内存中的 D view 冒称已执行追加 pass。

四 IR 先行探针的唯一基础设施失败是 `copyfile` 未保留 executable bit，首次 launch
在目标执行前 PermissionError。记录及空输出保留；恢复 u+x 后同一字节二进制、
同配置四份 verify/pins 全过。该失败不算分析或语义失败。探针预注册为
`3c5cdc2fba1f025e884e6b3dc0a775e5cc3f2759fd10bed438acdca3846df9a1`。

## 证据和范围

正式完整包：`guarded-finite-bitwise-full-real-20261112/recipe-B.tar.gz`；
探针包：`guarded-finite-bitwise-real-20261112/probes.tar.gz`。
VM在 `~/vmc-runs/`，Mac在 `/Users/c/Desktop/deob/validation-artifacts/` 同名目录。
每包保留原源、命令、输入、完整状态流、全部中间IR、失败日志、精确 payload 集合及SHA；
双端 `*-VERIFIED.json` 提供压缩SHA与逐payload核验结果。

真实 loop observer 独立封包，不混入上述C16/LLVM16或144控制。已闭合cffO0先做4原输入
的实际命中验证，其余三个仍未闭合；不能用一个样本的观察声称四样本整体完成。
