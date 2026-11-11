# 首次入口前缀的直接出口值传递

基线 `756d64b`，实现里程碑 `aac31b6`。此前 `entry-prefix` 对普通 non-PHI 跨区域使用保守拒绝。
真实 combo O2 的三个值，以及显式高预算 combo O0 的一个值，都只在定义块
的直接出口中使用；这个出口原来只有该定义块一个来源，可以精确合并新旧执行值。

## 实现边界

在独立候选事务里，收集定义的所有普通外部使用。仅当它们全在同一个直接出口，
且出口唯一原前驱块就是定义块时，才在出口入口插入 PHI：原路径传原定义，
复制路径传本次复制值。根 PHI 的复制值仍来自真实 seed 入边，fresh LOAD/freeze
不会被换成上次循环执行值。重复 successor 按实际边数各保留一份 incoming。

调用、volatile 访问和其他原出口行为仍在原块执行。修复不跨调用证明内存值，
不移动或删除副作用，不推断原函数签名。任意合流、多级使用和无法修复的历史
PHI 仍保守拒绝，候选失败时原 IR 不变。

每个候选最多修复64个值、512个普通使用、512个 PHI incoming。新增 PHI
同时计入原来的指令预算，超额在改写前拒绝。`cloned_instructions` 现在表示
复制原区域指令加新出口 PHI 的预算成本，仍不等于 IR 净增长。默认32块/2048
指令、模块累计预算、原事务身份及 Stats/C/Python 字段布局均保持不变。

## 已定位的真实边界

| 样本与配方 | 旧阻塞 | 精确修复位置 |
|---|---|---|
| combo O2，32/2048 | 3个普通跨出口值；同时触及前缀预算 | `bb_65869` 到 `memory.valid19` 的栈地址；`memory.valid33` 到 `memory.valid35` 的地址和存储值 |
| combo O0，128/16384 | 预算充足后仍拒绝1个普通跨出口值 | `bb_68024` 的栈地址到 `memory.valid69` 的 Guest GEP / RSP STORE，保留该块分配函数调用 |

默认 combo O0 前缀中，15个错误返回叶占1230/2007条指令。四个旧版预算探针
显示：32/2048、32/4096、64/2048输出逐字节相同；128/16384又遇到上述值传递
障碍。探针仅诊断 IR，不算原生语义验证。明确的大函数配方与默认配方分开验收，
不能把提高预算隐含进原默认结果。

旧循环分发仍从 Guest 状态槽读值，经历状态范围和表地址检查后读目标表。
本修复不承诺完整循环去平坦化。旧12次 dispatch-specialize 均在第257个条件
触及256上限；GuestSSA的合并预算计数不能唯一定位具体私有限制，不臆测其原因。

## 回归与当前验证

最终10个定向用例通过旧 PassPipeline API 编译，旧 main/no-Z3 运行库实际各为
**10 cases / 227 assertions，5 cases / 20 assertions 失败**；新候选各为
**10 cases / 262 assertions 全部通过**。其中新5用例单独在旧版为5/109、
4 cases / 17 assertions失败。不存在新API缺失而无法编译的例外。

原直接调用出口负例保留原输入，升级为严格检查 PHI 新旧值及调用位置的正例；
另外增加越过直接出口的普通使用负例，继续要求拒绝且 IR 逐字节不变。
这项预期升级不删除失败输入，也不放宽语义比较。

主/no-Z3 完整 CTest **1056/1056、921/921** 均通过。独立原生两个构建的六种形态
各得到1536次完整状态匹配，896次错误变体观察中476个指定错误全部检出；
未知调用次数、volatile 效果、寄存器、Guest和全部Exit均比较。
每组38条保存流、2432帧完整审计通过；原13个成功产物在两个新构建下逐字节相同，
不重跑或重计旧1824次执行。详见[独立原生报告](entry-prefix-liveout-independent-validation-20261112.md)。

真实原预算配方A通过 **C16/LLVM16** 完整状态对照及144个错误控制；原cffO0
首次selector直连门槛保留。comboO2从拒绝变成22块/2045成本的partial前缀，
三个出口PHI及其原/新入边、全部普通使用均核对，五种错误结构控制均检出。
预算耗尽仍保留；comboO0仍为32块/2007成本的partial结果。独立高预算配方B
也通过C16/LLVM16及144个错误控制，comboO0完整隔离110块/7892成本的前缀，
comboO2完整隔离40块/3071成本的前缀。新PHI正确传递原/复制执行值，分配调用
及出口其余指令仍保留。两套配方每个模块各25次实际pass run，原三个优化轮次和
其它pass预算不变；B仅显式提高prefix预算。
详见[真实验证](entry-prefix-continuation-real-validation-20261112.md)。

## 仍未消除的循环分发：具体证据

主agent另行读取配方B最终 `guarded-views/f-combo/O0/after-bound.ll`，SHA
`81ea20f65fd1b0b997d5881f1f575e53cdf70b57a614076b65e22545129876f0`。
该IR已在B完整归档内，以下只读诊断不新增原生执行计数：

- `memory.valid317` 读取 `Guest[90592]`，生成 `%1646 = select(load != 0, 3, 1)`；
  `memory.valid319` 将它存入 `Guest[RBP-168]`，再回循环分发。这是两个条件状态，
  不能在没有内存证明时将未知LOAD直接折成恒真或恒假。
- 分发器仍有实际的 `Guest[90120 + state*8]` 目标读取。状态集合与表内容是两层
  不同的证明；恢复 `{1,3}` 并不证明目标字节恒定。
- 普通调用 `@dragon_static_entry_x86_64_15f20` 有lifted定义，且确有动态Guest写。
  当前缺少足以保留表事实的写范围/别名证明。不能称它为未解析外部黑盒，也不能
  仅因callee可见就推断它不会修改表。

真实隔离配方没有运行MBA、predicates或dispatch-deflatten；另行冻结两个B产物，
各运行一次完整配方及一次扩大条件预算的配方，共四次独立IR探针。全部verify通过，
无超时；combo各化简16处MBA，谓词折叠和去平坦化均为0。扩大条件预算后新增的
上下文只发生在ordinary fallback函数；guarded函数的真实CFF和业务表LOAD/switch
均保留。谓词/MBA还有预算跳过，不能把API成功解释为覆盖全部表达式。
这些探针没有新原生语义对拍，也没有修改A/B验收配方或默认设置。
见[独立配方诊断](residual-loop-recipe-diagnosis-20261112.md)。

现有deflatten已支持某些已证明的双目标select，不能把这一能力再列成完全未实现。
下一步需把循环中的状态/地址关系接到实际表读取，并证明调用写范围与目标表不
别名等前提；仅增加预算或启用现有pass并未解决这些样本。不得将实际内存条件猜成常量。

证据根为 `~/vmc-runs/entry-prefix-continuation-20261112/`。构建来自 VM 的
`756d64b` 加五个候选源码文件，逐文件 SHA 在 `candidate-v1-source.json`。
旧测试最初两次编译因 Catch2 源/生成头 include 路径错误中止，日志分别保留；
第三次成功编译后的实际旧运行失败才计入回归证据。

旧预算探针整包 `entry-prefix-budget-diagnosis-20261112.tar.gz` 的 SHA 为
`14b5ec8a6d908f5f6efbd233c3cfe7d3dc818f6fcdf85d2ad387782c0232a3e5`，
26个payload加manifest，代理双端校验后主agent独立逐项复核。
此前四份基线运行库硬链接在旧测试和预算探针完成后已退役，原构建库及双端gzip
仍保留，恢复路径见 `baseline-runtime-retired.json`，避免新构建额外保留旧库占满磁盘。

本轮没有新增单指令/Unicorn或全69输入语料库campaign。原生验证针对该LLVM变换，
结构/字节审计单独计数，未知调用后的内存事实和完整循环去平坦化不在成功声明内。

## 最终归档

下列包已双端校验，主agent另外独立验证包SHA、精确文件集合及每个payload的SHA
和记录大小。运行库四份gzip同时验证解压后的原始SHA；源码五项与`aac31b6`一致。
Mac根目录为`/Users/c/Desktop/deob/validation-artifacts/`，VM根为`~/vmc-runs/`。

| 包（相对证据根） | SHA256 |
|---|---|
| entry-prefix-continuation-20261112/oracle-main-v1-evidence.tar.gz | f8b774c2c181d3d47c9ea85ba1bc9992052ca05da41b0557bd46e372a0e8f9e6 |
| entry-prefix-continuation-20261112/oracle-noz3-v1-evidence.tar.gz | 396aecd4ab0a9f4c8b43da885b08f4cb092bfed3a5795986d5c942911382e9e8 |
| entry-prefix-continuation-20261112/metadata-final-v1-evidence.tar.gz | a5effd6d688e8720006d2a6684911d7172c6faf217caf8aae2d8eab0c1caed3d |
| entry-prefix-continuation-real-20261112/recipe-A.tar.gz | dd85cb3efbbbd51aaac8b9c28d8837bbf87f400584e5b488ac72c2a0ba4a39e5 |
| entry-prefix-continuation-real-20261112/recipe-B.tar.gz | 107a7879475e07c2129b1b102bb972d45ccb8d1964e35eef351bcac41ca608cf |
| entry-prefix-continuation-real-20261112/final-review.tar.gz | c543b620709680ff0f6649ad39e8d864ee941708f7ac42d990943c1a0148702f |
| residual-loop-recipes-20261112.tar.gz | 5cf9c5a529d362d53efdabc56cc4c56f7c8a2cdf3f8edcb9d6dce13a380deecb |
