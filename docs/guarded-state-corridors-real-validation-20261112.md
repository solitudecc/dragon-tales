# SSA 状态通路：四个冻结真实样本验证

本阶段验证实现里程碑 `500ba7f`（E 基线 `313df60`）的显式
`guarded-state-corridors`。原四 ELF、16 个输入及完整 C/LLVM 比较器保持不变。
正式运行 170.097 秒，C 16/16、LLVM 完整状态 16/16 匹配；寄存器 16、
Guest 内存 16、七个 Exit 字段 112，共 144/144 故障控制检出。没有新增跳过、
排除或归一化。四个源 Graph 不变，16 个输入逐字节保持，六类运行前后绑定均一致。

## 固定配方和边界

预注册 SHA256：
`3cdb40bcfd1c1cee6c080eb30729e8b7b3496028c0ff689cd2ae9642206d2d4f`。

保持此前 B 配方的前缀 128/16384、原三轮八步、业务通路 cover512 和 D 尾处理，
共 37 步；追加 F 一次、`mem2reg,instcombine,gvn,instcombine,dce` 五步、
`constant-branches` 一次、E `guarded-target-sets` 一次，总计 **45 步**。
F 默认 64 candidates / 32 rewrites / 128 blocks / 16384 instructions；
没有增加原 GuestSSA、dispatch 或 F 预算。自然业务六值 ELF 元数据仍只是预测，
没有写入 entry-memory 契约，也没有假定运行时表不可变。

运行库 backend SHA256：
`5d1247db22c263a32114134c7b31187d54de273088159d124175db48121049f0`；
shim：`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`。
运行前要求至少 1 GiB 可用，实际 1,130,684,416 字节；原 64 MiB 停止阈值保持。
运行完成，没有触发停止。库路径后续可由 root 退役，恢复位置以 root 运行库归档收据为准。

## 实际精度和残留

| 样本 | F 改写 | F 新块/指令 | complete | budget events | 最终实际小集合 switch | 严格快路闭合 |
|---|---:|---:|---:|---:|---:|---|
| f-cff O0 | 0 | 0/0 | 1 | 0 | 1 | 是 |
| f-cff O2 | 17 | 63/2206 | 1 | 0 | 18 | 否 |
| f-combo O0 | 0 | 0/0 | 0 | 1 | 1 | 否 |
| f-combo O2 | 23 | 116/2842 | 0 | 9 | 20 | 否 |

F-combo O2 实际尝试 31 条边，8 条拒绝。集合数量是复制后的上下文，不是独立状态数。
原三轮 Guest-memory 12 次全部 incomplete，累计 12 个 budget events；
dispatch-specialize 12 次全部 incomplete，累计 16 个 budget events，未伪装为 complete。

f-cff O2 的 state34 自然业务 case 与 state37 入边得到覆盖，但剩余
`bb_66244 → bb_65712.bb_65718_crit_edge → bb_65718 → memory.valid15`。
实际状态表达式为 `and unknown,1` 后 `or disjoint ...,24`，有限集合为 24/25，
本阶段 F 尚不支持该位运算候选形状。

两个 combo 首路径存在有限索引移位后与不重叠表基址作 OR 的地址表达式，
仍需下一组件处理。combo O0 本轮明确报告 expression/search budget incomplete，
不能据静态形状就把报告改成成功。combo O2 残留路径经过
`memory.valid49.transition2 → transition.fallback5 → bb_66038 → memory.valid49`；
前两者已变无条件边，因此不是遗漏的动态 guard-failure 分类，不得擅自排除。

严格闭合只排除已经验证的同次实际 LOAD 的单值/小集合失败边，保留所有普通条件
和实际业务 case。F 可以合法重定向原 case 边，所以审计要求最终 fast case 与其
完整 fallback 的实际目标一致，同时保留原 case 值集合/default 族；不要求所有目标
仍是改写前名称。此前 f-cff O0 首表槽 70032 的真实 guard LOAD、边界检查和直达
`bb_67010` 仍通过独立审计，错误出口、原重入 switch 仍保留。

## 输出到实际原生执行的绑定

外层 adapter 保留原内存中 D view，独立 C++ public API driver 解析 D 最终 IR，
执行八个追加 pass 并 verify；仅将该最终文本交给原 native 编译器/比较器。
这不是宣称原 D 对象内部已经执行 F。

四模块均验证：

1. `state-corridor-tail/.../module.after.ll` 与 `guarded-views/.../after-raw.ll` 字节一致；
2. 原 `bind_wrapper` 只替换第一次 kernel 入口调用，结果与 `after-bound.ll` 一致；
3. `after-bound.ll` 与实际 `oracle/.../s0-product-O0/raw.ll` 字节一致；
4. 同目录 `commands.jsonl` 中真实 `llc-21 -O0` 命令使用该 `raw.ll`。

审计另检出 16/16 结构故障（错误 case、目标、fallback case 和 selector），原分类控制、
首 selector 四控制和 `%83` bridge 五控制保持。首轮审计启动遗漏 Python 搜索路径，
产生 `ModuleNotFoundError`；原日志保留，修正调用环境后成功，未修改比较器或输入。
F driver 本轮仅执行 normal；其继承的 missing/invalid 模式没有运行或计入控制，
E 原封存负例仍是独立验证，不将整个 F 流水线错误要求为 unchanged。

## 观察与归档

这一阶段没有新增真实 loop observer 执行，闭合 1/4 不等于完整四样本去平坦化。
独立 LLVM-only observer v2 已完成自检：96 个完整状态比较、24 个正常计数观测、
24 个 double-count 和 12 个 omit-central 故障检出；四种坏 PLAN、四种限制性属性、
重复插桩均拒绝，输入/工具依赖保持。其结果属于工具自检，不计入上述真实 C16/LLVM16。
最终真实观察等待后续 G 输出，并需验证同一闭合 SCC guard 在一次入口调用中重复命中、
完整 fallback/中央计数覆盖；不能把不同 guard 总次数当循环证明。

正式证据：
`~/vmc-runs/guarded-state-corridors-full-real-20261112/recipe-B.tar.gz`；
Mac 同名副本位于
`/Users/c/Desktop/deob/validation-artifacts/guarded-state-corridors-full-real-20261112/`。
四 IR 探针另在 `guarded-state-corridors-real-20261112/probes.tar.gz`。
每包 `ARCHIVE-MANIFEST.json` 绑定精确文件集合、长度和每个 payload SHA256；
双端 `*-VERIFIED.json` 绑定压缩包 SHA。失败日志、所有中间 IR、命令、源快照、
原始原生流和控制均保留。独立保存流审计不计作额外原生执行。
