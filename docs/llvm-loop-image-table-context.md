# Loop Image table joint proof（草稿）

新增低级 C++ opt-in 入口：

```cpp
ImageTableContextStats runLoopImageTableContext(
    llvm::Module &, const ImageTableContextRequest &);
```

只处理原始 guarded view，旧 `runImageTableCheckedContext`、`runClosedImageTableContext`
及其 Request/Stats 布局、默认流水线保持不变。没有新 C shim、Python pass 或默认优化步骤。
初版 `d525d70b` 已完成下述双后端 focused；尚未跑本组件的 full/native。
后续组合组件必须按各自新版本重新验证，不能借用初版次数。

新入口先在私有 module clone 中复用原 guard/normalize/table 发现，构造待证明的
hypothetical table CFG，并完成原 SP/index 控制证明。T 的内容仍只是待闭合假设。
从 PHI head 到 table block 只允许一条至多6 blocks 的前驱路径；一次冻结全部
outside-path 原始 successor slots，包含 initial entry 和重复 slots，然后调用既有
`threadDispatchCorridorBatch`。某个候选拒绝或 batch 不完整即拒绝全部新变换。

Batch 的 work 上限被夹到 Image 剩余 work，返回实际 work 计入同一总账，仍 hard1M。
复制空间夹到原4096 blocks/65536 instructions 的剩余量，最终再次检查。局部 range
每次根查询保留128 scalar nodes、128内存定义图节点、32 scalar depth；所有指令/边还
消耗同一共享 work，没有每个 cell 或每条边重置求解预算。内存历史采用迭代图，单前驱
且没有目标写入的连续路径压成一条图边，但逐块、逐指令和逐实际前驱 slot 的扫描及收费
不省略。嵌套 LOAD 的标量和定义图节点消耗仍计在同一个根查询中。

Batch 会替换 Function body，因此所有旧指令/块 handle 都失效。新入口只用自己刚创建的
constant-only metadata role 来恢复 Guest、accepted、guard chain 与真实 target LOAD；
不依赖 clone 的名字或 switch 仍存在。caller 的 reserved kind/name 碰撞一律拒绝。
恢复后逐项检查类型、Guest base、表几何、唯一 anchor、guard dominance 与每块读后
无新增 observable 操作；未知 table-shaped read 拒绝。成功前清除全部内部 role。
metadata 表示内部来源，不是 caller 证书或新定义性事实。

原 SP/index 结论经已验证的语义保持 Batch 继承到同一原 lookup 的所有克隆。没有给
新增 blocks 虚构 SP 常量或观察点。post-batch 从 accepted 的实际可达图反向到所有
lookup LOAD cutpoints，形成完整 preserving 集 P，包括首次 read 之前与每条 continuation。
每条 P write 都必须保护 T，必要时还须保护同一个常量 cell C。terminal 之外的别名/
partial/volatile/atomic/fence/unknown-call 限制保持；terminal 效果仍按实际 CFG 执行。
Batch 可删去在 hypothetical CFG 下已被证明不可能的边/块；在 T/C 联合闭合之前，
这些结果只属于私有证明图，不允许发布。

为证明动态 work-array 地址，新的只读局部 range resolver 在**那条 LOAD 的原执行点**
逆扫 block prefix，再覆盖全部 actual predecessor slots 的 block end。最近的 exact、
simple、同 Guest/offset/bytewidth STORE 形成定义锚，切断更早 effects。已证明不交的写
可跨过；部分覆盖、可能 alias、未知 base/call、无初始定义的路径拒绝。完整图中每一个
节点必须沿逆向依赖边到达真实定义锚，无锚子环不能被另一个入口的良好定义遮蔽。
这样可以覆盖已有初始化且循环中未改写的 cell；所有锚的原 SSA value 都在各自原 STORE
执行点求 range，并取其保守并集。LOAD-prefix 按实际指令标识，与同块 block-end 分开，
避免漏掉 LOAD 之后供下一轮使用的 STORE。结构 memo 不携带数值；全部定义、别名和锚值
证明成功后才缓存原 LOAD 观察，不能把历史值和 fresh reload 或同址的另一次读混为一谈。

非固定 Guest cell 的 simple 整数 LOAD 可仅取得类型的完整位范围。这不读取其内容，
不证明基址不交，也不关联另一条 LOAD；真实 LOAD、错误路径和所有 consume 保留。
例如任意 i32 的逻辑右移28位都落在0–15，而不是假定那个输入或内存为常量。

实际 mask 可独立界定结果，所以 `(old + 1) & 7` 的 STORE value 不必假设 old 有界；
仍完整检查 operands 中可见的 undef/poison/非法 shift，且不为 old 发布任何 range。
Select 的两 value arms 合并，不需要猜 condition 真值；不增加 signed flag/range/
definedness 或 RBP/EBP 同名寄存器等式。

常量 cell 的 seed 必须在任何 hypothesis 之前已是独立常量，支配所有 cutpoints 和
拟取事实的原 LOAD。每个候选都有独立 overlay 和局部 range cache。第一次完整 P
检查只读，不在 hypothesis 下做 CFG prune、DSE、memory GVN 或 post-batch InstCombine；
因此中间 clobber→reload→restore 仍必须经过逐条写检查。全部通过后才把已证明的 C
LOAD uses 改为常量，真实 LOAD 留存，再用空的新 range cache 重验原 P。晚失败丢弃
整个私有 proof。新入口只发布完整 proof body，不投影旧 facts 回另一个未 threading 的图。

普通 guarded miss 仍走原 ordinary root；业务/bounds/error 的可执行效果由完整证明和
Batch 语义保持合同约束，不以 copies 或候选命中数当完整去平坦化。accepted 内原本只
在不可能 index/default 上可达的块可随 Batch 变为不可达；原来的两个 Image 入口仍保留
各自既有结构合同。本入口保留每条存活 lookup 的实际 target LOAD。

## 验收准备

新独立 TU：`tests/lifters/TestLLVMLoopImageTableContext.cpp`，tag `[image-loop-context]`。
包含多 cutpoint/local mask、初始输入、重复 slots、prefix/end、失败 cell hypothesis
不泄漏、partial/缺定义/灰环、过宽 mask、中间 clobber/restore、unknown call/其它 base、
SP/initial index、metadata collision/cleanup、tiny budget/完整回滚与旧 API 拒绝控制。
新的 named API 在旧 SDK 缺符号；该 negative link 是新 API 缺失，不能称旧执行错误。

初版编译、旧 SDK link gate 与 focused/旧 Image 兼容结果如下；full 与独立 native 尚未执行。此 tree 的 Batch
公开 header2f7只是已验证组件的编译依赖镜像，不是新的 Batch 实现提交；实际链接必须
先合并其经过验证的 implementation。RBP64/EBP32 两套 PHI 的逐边低位关系是已知剩余，
本组件不按寄存器名、样本地址或 source case 推断，实际大 Flatten 闭合仍须实测。

## 初版 focused 与真实拒绝（2026-11-15）

源码 `d525d70bc7c2adbc844469f0164be0a9b52e4abccb47850ce2650e9b9f77303c`；
新 TU `a51438b9da963dd1b1b05149aee75b0513bc757d1b4d8ffef79eec0e357deb56`。
main/no-Z3 各新5 cases/204 assertions、旧 Image14/2273 全过。
原 Batch SDK 的小客户端真实链接因缺少 `runLoopImageTableContext` 符号失败；这是
新 API 不存在，不是旧执行错误。新客户端符号为 UND，定义来自实际新 SDK；
main 实际载入记录的 Z3.so.4.13，no-Z3 无 Z3 映射。

首轮 fixture 的 PHI 直连 self 入边被 LLVM InstCombine 合法改为 poison，同时 bounds
变成 true，原 index 控制证明因此拒绝。v1 的5 cases/164 assertions 中有2 cases/4
assertions 失败，源码、日志和纯 LLVM 诊断全部保留。v3 仅将该 fixture 自边改为真实
`state + 1` incoming，保留真实 bounds，原断言未降低；核心逻辑不变。

同一 canonical raw `4315ddcd…`、ELF `bb98e3f3…` 与冻结 input0 上，真实 A/B 各
调用新 API 一次，均 verified/refused，整模块 byte-exact。两次 work 为792275/792366，
`budgetExhausted=true`，最终外层消息为 STORE 可能覆盖表；这不是1M总账耗尽，
局部 range cap 消息被外层通用消息覆盖。没有发布 hypothetical CFG，也没有实际 native
或完整去平坦化收益。后续范围组件是否能消除该阻挡须另行实测。

小证据双端证书 `6b4411aa…`，ROCK 根 `~/vmc-runs/loop-image-focused-20261115-v3/`；
完整六产物与源码/失败/真实诊断包为
`~/vmc-runs/loop-image-baseline-products-20261115-v1/runtime-source-focused-probe.tar.gz`，
314121758B，SHA `112088d5be9f9c02237f5a4688621d3dc292220b7a9d1d6e387319af10ed81ac`。
113 regular members 在 ROCK/Mac 两端逐成员 size/SHA 与单 gzip CRC/EOF 验证通过，
证书 `f71910ff…`。包明确仅4 DSO、2 focused clients 与列明源码/证据，不是完整 OS、
构建或依赖环境；恢复应使用新 versioned namespace，不能覆盖 active build。

## 私有 post-Batch 诊断更正

不能用 raw `B.before.ll` 的同名 block 或 numeric SSA 编号解释 transaction 后的
cap 点；此前把 `memory.valid167` 推成 SP+88 的说法撤回。独立 renamed source-inclusion
prototype 使用固定43f SDK helper、2次 private Image 调用，拒绝及 work885329/940135
与 installed43f 完全相等后才采用其内部快照。它不是 installed Image、T/C证书或
可发布去平坦化产物，native0。

实际 owned trial 的 root STORE 是 `store i32 %2628, ptr %2629`，地址为
`Guest + (dispatch.live2863 + 12)`。B 的 candidate C 是 Guest51072，overlay1803项中
已经包含 `%4479 = load i64, ptr(Guest+51072)`→32768。cap 的 PHI 互环有该 LOAD 叶和
字面量32768；strict 图按合同拒绝非字面量 LOAD，原 least-fixed-point overlay 也不能
补全此互环。这是 RSI/output pointer 依赖，不能再写成 SP 证明缺口。
完整 owned module、overlay、root STORE 和 cap→leaf 链保存在
`~/vmc-runs/loop-image-private-dump-20261115-v1/dumps/`，RESULT SHA
`96287f0afba9919135bd58357cd5b3e37313bd81ee4666d9b24c016d0785f264`。
条件 LOAD 锚的限定见 `llvm-phi-constant-graph.md`；H 本身仍须在原全部 P writes 上
完成联合归纳，不能因诊断看到了同址 LOAD 就当作已证事实。

## 后续实现与尚未闭合的部分

局部范围现在区分 scalar depth 与控制流历史距离。冻结同一24层夹具、同一个实际客户端，
旧 b8 SDK 在 scalar depth33 拒绝，新 b243 SDK 完成8 cases/1193 assertions；
详见 `loop-range-depth-accounting-validation-20261115.md`。完整内存定义图的入口覆盖与
不支持的值/别名递归边界见 `loop-memory-definition-review-20261115.md`。

固定字面 Guest STORE 地址可在已经检查的完整字节区间上直接判断是否不交，无需重复
展开该字面量为 scalar 依赖。128 STORE 压力夹具用同一实际客户端在旧 c535 的
scalar nodes131 拒绝，在新0da完成2 cases/4058 assertions；所有原 STORE、LOAD、
错误/fallback 均保留，partial/table/wrap/call 阴性原样拒绝。详见
`loop-fixed-store-span-regression-20261115.md`。

普通 `OpaqueIntegerAdmission::get` 仍限制128个物理依赖节点。新的显式
`getPhiRegions` 完整校验每条物理依赖、每个实际 PHI incoming slot 和所有不安全叶，
再把直接 PHI→PHI 的连通复制区域作为一个 shape 节点。非 PHI 各自保留节点。
新模式限制128个**完整压缩图**节点，另有65536物理节点和同一共享1M work 限制；
缓存仍须按完整压缩 footprint 检查当前剩余额度。它只允许 ordinary Full 位集合，
不提供 PHI 等值、稳定观察或定义性事实。任何原操作、flags 和 consume 均不改。

b348 SDK 的该模式5 cases/294 assertions、旧 Opaque5/274、其余聚焦回归与
128 STORE 回归通过。此次数只对应冻结 c69b 测试；新增的1000 PHI late-unsafe
测试尚待下一轮执行。真实大样本 B 仍未闭合，最终 fallback 的 scalar depth33
不能当作候选 C 的首拒原因，须查看 private 分阶段诊断。仍未发布 after IR，native0，
本组件的最终 full/no-Z3 也未完成。

另外保留了 `TestLLVMLoopMemoryDefinitions.cpp` 的六个强阳性失败：普通最近定义图
尚不能用待证 LOAD 的范围反过来证明循环中动态 STORE 不别名；这需要独立的观察范围
归纳。其15个阴性与3个 tiny-work 场景继续通过。不会删除这些失败或把它们计作已完成
的循环精度；新归纳必须保持完整初始化 cutset、原 STORE 执行点与条件缓存回滚。

后续 `2a3f4b0c…` 已实现该观察范围归纳，并在2c9 SDK 中使六个原始阳性都闭合。
每个 exact LOAD 独立建立完整初始化图和锚值范围；锚阶段禁用所有临时假设，别名阶段
才允许引用该 exact LOAD 的候选范围。嵌套成功仍依赖父证明，父失败恢复完整数值缓存。
原 LOAD、STORE、错误和默认路径不改，不声称 cell 在所有时刻都不变。独立审查见
`loop-observation-range-closure-review-20261115.md`。

v1 的四个原始 LOAD 数量断言暴露了夹具校准问题：既有前端可合法转发同块 STORE 后的
index.a LOAD。v2 保持原 IR builder、全部阴性及预算输入不变，在独立 clone 上仅运行
LLVM 官方 mem2reg+InstCombine，要求 index.b 仍存在，并要求生产 proof 保留控制组的
每个存活 index LOAD。原 v1 所有失败保留。最终同一 v2 源码、同一实际客户端在旧 c535
六个闭合门失败，在新2c9完成3 cases/844 assertions；没有把 raw 数量2直接降为1。

Gray 原夹具现走完整阳性门。原 TwoCandidates 的首个500 seed cell 也能真实闭合，
因此另加动态地址1024或1536写501的失败候选，继续要求原 first.read 保持非常量使用，
检验失败假设不泄漏。最终聚焦 Loop6/221、Depth9/1210、内存图3/844、Regions6/314
及全部其余聚焦组通过。完整双构建回归正在进行；真实 B 尚未闭合，after-native 仍为0。

上述观察范围组件随后完成主构建1305/1305、无 Z3 构建1159/1159，提交为
`60d3365`。准确的源码、运行库、首次失败及重跑环境见
`loop-image-implementation-validation-20261115.md`。该提交仍在功能分支，未合入 main。

后续掩码精化只针对原 STORE 的完整写入范围：先作粗查询，只有有效但仍可能重叠的
结果遇到低位被清除的已知掩码时，才在同一预算内作一次内容精化。两个操作数的风险
检查仍保留；独立锚值阶段禁止该精化。每个嵌套 STORE 独立管理查询模式和恢复。
单值 AND 的上界取两个操作数上界的较小值，低位全1掩码仍可截断无关的内存值依赖。
同一客户端在旧2c9的五个新阳性失败，在新a5d完成2 cases/692 assertions；
详见 `loop-mask-memory-refinement-validation-20261115.md`。

接着允许已证明合法范围内的非单值移位次数。无符号右移采用
`[a.low >> b.high, a.high >> b.low]`；仅在整个左移范围不回绕时采用
`[a.low << b.low, a.high << b.high]`，否则保留 Full。任何可能越过位宽的次数仍拒绝。
原 shift、flags 和所有使用不变，不输出额外定义性事实。源码b6b18、运行库777的原11组
聚焦检查通过；该轮未跑新的完整双构建或 after-native。真实 B 越过原移位拒点后仍
未闭合，最终报告节点预算不足。首次拒绝位置仍须按完整阶段诊断，不能用最终消息
替代；预算、源模块回滚和真实样本验收要求不变。

之后已完成本轮实现验证：Loop 显式 BranchConditions 路径遍历省掉重复的纯计数扫描，
保留旧构造符号及默认计费。真实 B 在主/无 Z3 均以985771 work闭合，68 STORE、73
实际查表切点保留；选中 view 只剩入口 switch，cyclic switch为0，两个 ordinary 函数
原样。最终主1317/1317、无 Z3 1171/1171通过。首次发布遇到旧producer精确函数集合
断言，原因是既有提交逻辑允许新增 canonical llvm.fshl.i32声明；原失败保留，新typed
声明核验及14个错误对照均通过。见 `loop-image-branch-milestone-20261115.md`。
这仍是分支上的实现检查点；after-native、真实接受/回退观察与后续可读性清理待完成，
不能把旧原始IR的32次对照计为本次改写的执行验证。
