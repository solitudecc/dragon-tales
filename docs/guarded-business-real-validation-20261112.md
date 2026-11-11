# Business corridors：真实验证与尚未闭合的边（2026-11-12）

本轮基于 `0c0717f`，验证显式 `business_corridors=1`：原常量 terminal 机会优先，随后从原业务 STORE 边复制有界路径，最终以实际 target LOAD 与候选比较。候选提示不构成 STORE 地址相等、表不可变或路径可行性的证明。原条件、写效果、完整同值 fallback 和退出仍保留。

候选 main runtime：`~/vmc-runs/guarded-business-corridors-20261112/runtime-main-v2`，backend `a9efefec601995481c6c3acceab55c6cf9a571e32b743c2f708af1b117269282`，shim `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`。

## 三个预注册预算对照

固定输入为上一阶段四份 cache-B 原始 final IR，先一轮 mem2reg/instcombine/gvn/control-flow/dce，再一次 combined controller，之后不cleanup。预注册 SHA `da2a4c9c892207769828d2e27c0fa07cc8be697b1032b1bba9755008f2cac77a`，runtime activation SHA `1bb368001d82e0ce976c3f1907621de24a94ad6d2ecb58c5014ab57ec676fcc8`。12次独立IR执行全部返回0并verify；此组原生执行数为零。

三个配置的候选上限均64、指令上限均16384。default 为32改写/128块；wide256 为64改写/256块；cover512 为64改写/512块。GuestSSA、dispatch及旧3轮预算未变。增量预算根据原图静态成本预先登记，不是失败后逐次加额：24 terminal 最少48块，25业务路径中8条8块、17条9块，每条另有fallback，合计至少290块。最早预注册写了仍成立的弱下界273，activation补充精确下界290。

| cff O0 | actual rewrites | 新块 | 指令成本 | 新业务 STORE 边 | 未处理已发现边 | complete |
|---|---:|---:|---:|---:|---:|---|
| default | 32 | 120 | 1144 | 8/25 | 17 | 否，budget1 |
| wide256 | 45 | 250 | 2808 | 21/25 | 4 | 否，budget1 |
| cover512 | 49 | 290 | 3320 | 25/25 | 0 | 是 |

这里49=原24 terminal+新增25 STORE边；此前“13”是业务起点数，不是所有分支/STORE边数。25个原seed、successor slot、原路径和最终guard均逐项保存，包含state41的 `memory.valid75` slot0 → 实际LOAD==67105；69960只是预测时使用的表槽地址，不冒称动态LOAD已化为该地址。

其他模块三配置结果相同：cff O2 为24改写/48块/96指令；combo O2 为3/6/18；combo O0 为5/18/308，比此前新增一条业务边，但仍报告 `candidate budget exhausted`。其 unresolved_edges=0 只表示5个已发现候选都处理了，发现阶段被截断，不能据此称整体complete。

逐边清单对未改写边仅记录事实和全局budget/rejection计数；现接口无逐边拒绝原因，审核器不虚构每一边的具体错误。

## 严格快速CFG闭合检查：四模块均失败

起点由真实入口最后一个表项LOAD等值检查及其ordinary-root fallback确认。只排除实际LOAD==candidate、同次值完整switch确认的预测失败边；所有普通条件分支、自然业务switch的全部decoded cases/default均保留，calls继续沿CFG遍历并另列。

门槛同时要求：成功子图不再可达任何CFF selector switch，并含至少一个带实际fast guard的循环SCC。还单列原decoded CFF case覆盖和51个候选表地址覆盖，后者不声称这些状态都可达。三配置四模块均未达到门槛：中央仍分别为 `memory.valid13`、`memory.valid15`、`memory.valid137`、`memory.valid49`。`complete`统计只约束局部候选处理，不能代替闭合门槛。

审核器首版只按case值集合分类，发现cff O0的CFF与自然业务switch均有66个decoded cases，误将业务switch也归中央。原源码和结果保留；修正版按实际manifest覆盖的常量selector所对应的 `(case集合, 原default出口身份)` 分组，不依据名字含义。业务switch完整保留，门槛仍失败；新增同case集合/不同default的控制，合计5项合成图检查通过。这是独立审核器修正，没有修改优化输入或原生比较器。

## 当前生产组件完整B原生验证

新预注册 SHA `cc60b721ef7f8f450dde10bc348bdc0537fae9fe10a1dc47fc4ac453913ef782`。冻结旧B prefix128/16384及原3轮×8步骤；追加原5cleanup，最后一次cover512 business controller，共31 pass。没有额外尾精简。新外层adapter仅显式设置选项，旧四ELF、16输入、模型和比较器不变。

- 新执行 C16/16、LLVM完整状态16/16匹配；寄存器控制16、Guest控制16、七类Exit控制112，共144/144故障检出。
- 四个source/Graph未变，16份原输入逐字节一致；runtime、源码、fixture、合同及harness pins前后均一致。无新增跳过、排除或归一化，耗时97.7673秒。
- 原continuation严格first-selector/bridge审核原样通过；其四项selector、五项bridge结构控制另计。
- 真正送原生执行的final IR独立只读解析：实际guards49/24/5/3，cff O0全部25原STORE slot均重定向，290块；8项故意破坏candidate/fallback的结构控制检出，5项图算法控制通过。这些不混入144原生控制。
- 四个实际final模块的闭合门槛仍均为False，完整保留。继承阶段仍有12次GuestSSA incomplete/12个budget events、12次dispatch incomplete/16个events。

## 另一个尾精简诊断：不是正式配方

在四份cover512输出上单独一轮 `mem2reg,instcombine,gvn,instcombine,dce`，没有control-flow/SimplifyCFG、没有再次controller。预注册 SHA `2bfdcea6978d4606a52ebdf17650e526332a871c8f52f663550c319e67f47205`。四次verify成功，原生执行数为零。

actual target LOAD guards保持49/24/5/3，逐项候选与同值fallback完整。cff O0的25个复制state条件全部成为字面量false。state41精确例：

```llvm
memory.valid9.transition9:
  br i1 false,
     label %memory.valid9.transition9.bb_67382_crit_edge,
     label %memory.valid9.bb_66252_crit_edge.transition13
```

因此真实常量已证明，但raw CFG的无效successor尚未删除。不能在审核器里悄悄排除该普通边后宣称闭合。下一小组件可仅折叠字面量条件的terminator，避免一般SimplifyCFG将动态target guards合回switch；仍需另验自然业务switch的未知/改表case和所有其他中央重入。

## 证据

IR诊断完整根：`~/vmc-runs/guarded-business-real-20261112`（三profile、审核器首版错误及修正版、独立尾精简）。Mac对应完整包 `/Users/c/Desktop/deob/validation-artifacts/guarded-business-real-20261112.tar.gz`。正式native根：`~/vmc-runs/guarded-business-full-real-20261112/recipe-B`；Mac包 `/Users/c/Desktop/deob/validation-artifacts/guarded-business-full-real-20261112/recipe-B.tar.gz`。外部MAC/VM VERIFIED收据绑定包SHA、精确文件集合和每个payload长度/SHA，避免包内自引用。

本轮无新增单指令/IGNIL/69样本 corpus，不把四个函数的局部正确性或guard数量当作通用去平坦化完成。展开证据保留，未经额外授权不删除。
