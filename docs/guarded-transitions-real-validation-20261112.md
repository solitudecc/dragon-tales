# 自动 guarded transitions：真实验证（2026-11-12）

实现里程碑 `4c66276`。本组件从已有常量表地址识别候选，实际执行 LOAD 后比较候选，成功直达原 case，失败使用同次 LOAD 值进入完整 switch。manifest 的表字节仅提示候选，不是 readonly/noalias 或循环不变量。**这是局部分发快路，尚未闭合全部业务循环。**

候选 runtime：`~/vmc-runs/guarded-loop-transitions-20261112/runtime-main-v2`，backend `e96ec64725e521e88e8104c4e77bc1103c39dc5593642afe4ce9d7ccce5971c8`，shim `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`。所有执行前后核对绑定，不复制第三方运行库进子包。

## 顺序诊断，保留反向结果

输入固定为前轮 cache B 的四份最终 guarded IR。以下都是独立 IR 诊断，原生执行计数为零。

1. **v2：controller → control-flow,dce**。controller 一轮一次，显式预算64候选/32改写/128累计块/16384累计指令；cleanup 一轮2 pass。中间实际改写15/24/1/3，全部 complete，原 eligible switches 与同值 fallback 各保留相同数量。但 cff O2 的24个比较随后被 control-flow 合回 switch；最终可识别 guard 仅15/0/1/3。不能把其中间24次改写当最终收益。
2. **v3：mem2reg,instcombine,gvn,control-flow,dce → controller**。相同输入、相同 controller 预算，前置cleanup 一轮5 pass；controller 后不再 cleanup。末轮遗留的状态 LOAD 经标准优化成为常量后，最终实际 guard 为24/24/4/3。原 v2 结果与失败均保留，不替换。

| 顺序 | cff O0 | cff O2 | combo O0 | combo O2 |
|---|---:|---:|---:|---:|
| v2 controller 中间改写 | 15 | 24 | 1 | 3 |
| v2 cleanup 后 guard | 15 | 0 | 1 | 3 |
| v3 最终 guard | 24 | 24 | 4 | 3 |
| v3 不同实际表地址 | 19 | 17 | 4 | 3 |

v2、v3 均逐项核对 fresh LOAD、eq、原合法 case、成功目标、完整 fallback case/default、原 eligible switch 保留；各8个故意破坏 candidate/fallback 清单的结构控制被拒绝。这些不是额外原生 faulty-IR 执行。

v2 预注册 SHA `f49ffc958a68c8a88e96191ad0c94d0f567343cddc4a29099a1a2a051ff50660`；v3 SHA `d0fad0bb4226829da6e698379d05903952a29c665f93cb08ee4a41547c6c4f06`。v2 独立完整包 SHA `badb8022ceb45e6b82936b896cce8c8d37df5c45e28d11aac24b9c0dbfca6ff9`，100文件/99 payload共68799816字节，双端准确集合和逐文件长度/SHA核验。

最初 v1 driver 使用了与既有成功配方不同的额外共享 LLVM 链接及 llvm-config 编译 flags。旧 b6 打印 unknown builtin 后 allocator abort，不能当正常旧结果；新四组当时未执行。失败源码、编译命令、ldd及日志保留。改回既有 backend-only g++ 配方后，新旧 empty-module 生命周期 smoke 均正常，旧 b6 同一真实 cff 输入正常拒绝未知 builtin、exit9。此为接口不可用对照，不是旧语义错误；未将 allocator 失败归因为已定位的生产库语义缺陷。补充 reviewer 首版 KeyError 字段拼写故障也保留；修复字段访问未改变任何语义比较器或精度门槛。

## 完整 B 原生重放

预注册 SHA `5c7a8d5078f793fb59a9a20d63e32bdfcda04d17eea269af1166a99a0dfc3f9b`。新外层 adapter 导入旧 `entry_prefix_continuation_real.py`（SHA `d8b45cebf2497fc3d04251fda3185483b0f6df27b56605d8400305fa66cba447`），不修改输入、模型或 comparator。

每模块原 B：一次 prefix128/16384，加原3轮×8步骤。随后明确追加前述5个cleanup pass，再一次controller；总31 pass。旧 activation 的25个继承步骤保持原记录，额外6个在 `guarded-transition-tail-activation.json` 分开列出。绝无 controller 后 cleanup，也未提高原 GuestSSA/dispatch 预算。

实际新执行结果：

- C16/16、LLVM完整状态16/16匹配；寄存器控制16、Guest控制16、七类Exit控制112，共144/144故障检出。
- 四个 source/Graph 未变，16份原输入逐字节相同；源码/运行库/fixture/合同/harness pins 全部不变。无新增排除、跳过或归一化。耗时107.6575秒。
- 原 continuation 严格审核原样通过：cff O0 首次 selector 直接到 `bb_67010`、guard LOAD70032和错误出口保留；combo O0 `%83` 原/复制值 bridge 保持。原四项首 selector、五项 bridge 结构故障控制仍通过，单独计数。
- 后续继承阶段仍有12次GuestSSA incomplete/12个budget events，12次dispatch incomplete/16个events；不能因原生匹配称全分析 complete。
- 独立只读 LLVM parser 对真正送原生执行的 prepared/final IR 验证并枚举结构，未再次优化：最终实际 guard24/24/4/3、不同表地址19/17/4/3；同值完整fallback及原eligible switches全部保留。8项新增结构错误控制检出，不混入144原生控制。

外层 adapter SHA `30b10047458d52dc5c98391b5d68cfd104ee4f7a8e392763f7ba53547b09f460`；独立审核脚本 SHA `fa5cca22e50db1f9eb82c1b2926543a45a19094ff6ccc5c92622eb9110c32f6c`；只读 LLVM inventory 源 SHA `c2fc641bb19f9ab7563d74393cd06c790297a1809c84f2a18d905a6b9cb07dbe`。

## 尚未闭合的真实边

cff O0 原动态 CFF `memory.valid13` 与自然业务 switch `memory.valid261` 仍存在；O2 对应 `memory.valid15`/`memory.valid85`。combo O0/O2 还保留动态 entry-prefix 表路径。业务 switch 不算 CFF，fallback switch 数增加也不代表混淆增加或消除。

具体 cff O0：table69976 的候选66450（state43）成功后，仍有结构路径经 `bb_66450 → memory.valid73 → memory.valid75`（STORE41）`→ bb_67382 → bb_66234 → memory.valid7 → memory.valid9 → bb_66252 → memory.valid11 → memory.valid13`，回到原动态分发器。路径清单包括实际 critical edge；这里简写核心块。只读图遍历采用已识别 guard 成功边、其他条件两边，停在原分发器/调用/退出；它证明的是尚存 CFG 结构，不证明该路径对所有输入可行。

下一步需要隔离业务 STORE 到下一次真实 selector 的有界路径，处理历史值/当前执行值、写内存及 SSA liveout，同时保留当前表值变化后的 fallback。24个上下文并非24个不同状态，更不是整个快速循环已闭合。

## 归档

IR探针完整包：Mac `/Users/c/Desktop/deob/validation-artifacts/guarded-transitions-real-20261112.tar.gz`，VM `~/vmc-runs/guarded-transitions-real-20261112.tar.gz`，包括最初失败和v2/v3全部版本。完整 B：Mac `/Users/c/Desktop/deob/validation-artifacts/guarded-transitions-full-real-20261112/recipe-B.tar.gz`，VM `~/vmc-runs/guarded-transitions-full-real-20261112/recipe-B.tar.gz`。最终外部校验收据绑定包SHA、准确文件集合和每个payload长度/SHA；不把包自身SHA写入包内造成自引用。

本阶段没有新增单指令/IGNIL/69样本 corpus；仅四个冻结函数的原生完整状态重放和上述独立结构诊断。展开证据和冻结运行库继续保留。

最终完整IR探针包SHA `cca1e8bb8a80eb962ff22b5812dc85684ad42a0c78e16a11a8710a83a20c47d8`
（227 payload）；完整原生B包SHA `0e8e85ab9a9886c68586dbca4d74f976c3541de32c96cc9aee33ae04e9dacd14`
（413 payload）。root已另行重算准确成员集合与所有payload长度/SHA。
