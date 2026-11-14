# Checked dispatch 三组件接入与验证

状态：正式 C++ 接入、完整回归、原生与边界验证、双端封存校验全部完成。接口与合同见
[组件说明](llvm-checked-dispatch-components.md)。这是显式 custom-pass 能力，不改变默认
六步优化，也不把验证脚本中的自动候选枚举注册为默认 pass。

## 已冻结的原型结果

本阶段使用本地 `ida-review-20261114/manual-O2.elf`，SHA256
`f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe`。
它是手写普通 CFF 样本，不是另一个 Tigress Flatten 样本，也不是组合保护的完成证据。
原始 LLVM SHA256
`0619b5e6d0d22b90a4ef4351929cfa9f1b61b3590c2bd81cfa32118dba7c2eed`。

显式 recipe 创建带真实 SP/表字节检查的独立分析 view，再证明表内容在相关执行路径上
保持、把最近状态 STORE 提升为 SSA、穿过 PHI 分配 decoder，最后保序处理原始回流边。
ordinary root 和 guard miss 的原执行继续保留。目标不是生成独立去混淆二进制。

保序组件处理8/8原始候选边，拒绝0、预算耗尽0；48副本块、853复制指令、165历史
snapshots、182修复 PHI、145797修复 cells、work159112。核心调用紧后及最终实际业务
SCC 的循环分发 switch 都为0；最终循环 table read 也为0。19个 checkpoint 全部验证
合法性并检查源/ordinary function 不变、原 Function/Argument 对象身份保持。
前9个阶段的18份 canonical/bound IR 与旧 recipe 逐字节一致，无归一化。

真实 nominal 对拍先执行本次 raw 的 before LLVM32次，再执行处理后 LLVM32次：同16组
输入，各自 O0/O2，64份结果的全部66136字节均与原16份 Unicorn snapshots 一致。
比较器解读76个寄存器字段；完整原始快照同时覆盖512字节寄存器区、65536字节 Guest、
56字节 Exit（7个u64字段）、8字节 budget及24字节协议头/调用计数。192个比较器错误对照
全部检出。新 C/Unicorn 调用为0；旧结果只作冻结参考，不重复计算为新验证。
这组结果不覆盖 guard-miss、任意 GuestSize 或任意黑盒保护。

独立合成对拍覆盖历史回流、二值状态、STORE 后 alias/状态读取拒绝，以及错误路径：
64个 module-input、16份初始 arena；before C128+LLVM128、after LLVM128均通过，
另外64次实际错误变体全部检出，4992个快照比较器控制通过。6条完整保存流共448帧均
审计到 EOF。完整 arena 为66112字节，与真实样本的66136字节合同分开。

## 原始失败与限制

所有原型失败保持原始记录。v1 的历史 PHI 值证明、临时函数声明与元数据事务缺陷分别
记录；v1 原源码未成功编译，不能称其发生了 native wrong result。仅机械编译兼容的
v1 和保留旧值模型的合法事务 control 分别测到拒绝/错误证明，不能混为旧正式库缺陷。
重复边测试最初查询被 LLVM 清理的常量 PHI，诊断失败没有删除；最终测试保留常量返回
检查，并另加非恒定重复 successor slot 检查。宏测试配置14例/596断言通过；正式库不
导出强制提交失败接口，默认配置的13例另行验证。

本改动不涉及机器指令语义；没有重跑单指令 Unicorn、IGNIL 全语料或69输入历史 campaign。
已有 frozen reference 的桥接、原型 helper-object 和正式 shared-library 运行必须分别
统计。结构门槛不替代语义对拍，输入对拍也不等于任意程序的形式化证明。

## 正式库验证

接入复核发现 Image 原型的事务边界缺陷，随后单独修复，不能再把此次接入全部称为机械
include 迁移。旧核心 `066cb38f…` 的同一新增测试实跑1例/74断言，8失败：成功调用会
凭空留下空 `llvm.dbg.cu`，fastcc/musttail 输入还因回滚承载函数的默认 C calling
convention 在提交阶段被拒绝。这不是已证实的 target native 错误结果。

另外记录了两个不同崩溃：新增测试首版在旧实现意外发布后继续解引用已过期句柄，是测试
自身错误；修正为先严格 REQUIRE 拒绝再检查句柄后，独立 foreign BlockAddress section
仍在旧实现 `BlockAddress::get → Mapper::mapBlockAddress → CloneModule` 崩溃，已有单
section GDB 记录。两份原始失败都保留，不互相替代。

新核心 `b52d25ff…` 在 clone 前拒绝本地 metadata 身份与嵌套 foreign BlockAddress，
保留 named-metadata 对象/operands，临时承载继承原 calling convention 和 attributes，
在删除原 body 前完成全部验证。表内容证明和业务改写不变。独立宏测试7例/939断言
通过，含原4例、同 API 新2例和两阶段强制提交回滚1例。正式库不定义故障注入宏，默认
Image6、Decoder9、Corridor13，共28例须在新 shared library 上另跑。

正式源码已冻结并通过 GCC 原 RelWithDebInfo 参数的6次编译、2次串行链接；428个实际输入
pins 前后相同。保留原 Cell/C4/PassPipeline/predicate 对象和94个 LLVM static archives，
各库只追加3个新实现对象，不加载另一个 shared LLVM。构建 PLAN `267a3b66…`，BUILD
`b32a98ce…`。实际库分别为：

| backend | libdragon.so SHA256 |
|---|---|
| main | `2645b01d50cd9336a25ff4aadea87abe6ccc64ab615b2dca2063136bc7e610f8` |
| no-Z3 | `80dc8fe64694ecbd836cf28fdaf05ad82770423f9e347055e22a9ca469c36b6c` |

正式客户端每库28例/1701断言全部通过，SDK正链和运行也通过。客户端仅3个 test.o 或
smoke.o，没有实现对象；6个正常 API 都为 executable UND → 实际库 T，故障注入 hook
不导出，loader 记录锁定对应新库。旧实际库806d负链精确缺少这6个新 API；这是新增
接口不可用，不是旧 target runtime wrong。focus SUMMARY `37b015d8…`。

提交前检查另发现 Corridor 测试文件末尾4个空行。只删除这些空行，测试 token 与断言
完全相同；独立重新编译该 test.o，复用另2个 test.o，两库再次各28/1701通过。
最终测试源 SHA `d0e83b3c…`，source-clean SUMMARY `aa835a20…`；原 BUILD 的14份源码
快照保留原样，9个实现/头与2份 CMake 均仍逐字节匹配，不能把测试的空白差异隐去。

历史缓存完整发现列表保持命令、断言、超时与测试数量；只替换44个 C shim 环境路径，
显式 LD 指向新库。main1163/1163通过（88.42秒）；no-Z31028/1028通过（76.49秒）。
另外补跑不在缓存发现列表中的现有客户端：predicate main12/962、no-Z3 1/5；C4各16/3493；
Cell各14/261，全部通过且 loader 绑定新库。该补充计数与历史 CTest、28新用例分开。

正式 native/real producer 均仅旧 producer.o 加新库重链接，正常 API 为 UND → 实际库 T。
两个 backend 的合成4次 API 调用都与冻结原型的全部 parsed-before/output IR 字节、统计
和固定报告字段一致。各自 after128完整66112字节对拍、fault64/64指定检测、1664比较器
控制通过；合计新normal256、fault128、controls3328，8条保存流共384帧审计通过。
旧 before C/LLVM 校准只引用。backend/优化级别重复不扩大64 module-input/16 arena域。

## 正式真实样本：分构建校准

main 对旧未修元数据的 baseline 在01阶段按预期严格失败。root 独立读取整份 old/new
Module：唯一差异是删除19字节 `!llvm.dbg.cu = !{}` 行。第一版派生 expected fixture
预计后续阶段均无此行，又在01c被严格门槛拒绝；该阶段 actual 与旧原文件反而逐字节
相同。验证 probe 的 `importViewBody` 会再次跨 Module 克隆并生成该空行，已对应源码
定位。这两次真实拒绝与一次启动前 metadata wrapper 错误都保留，没有改为通过。

最终显式 v3 expected fixture 只对01-checked-context和01b-cell-ssa的4份 canonical/bound
文件删除各自唯一空行；其余30份 old 文件保留原字节/hash。它是有明确补丁和来源的
expected fixture，不是新的 before 执行，也不在比较时归一化。原 c227 probe 与18项
完整字节门槛不变。main19阶段实跑通过，所有实际阶段除上述4文件外仍与旧相同。
Image work127508→127510，增加2个 metadata 事务检查计费；48守卫字节、55保留 STORE、
126常量事实、闭合结果和原硬预算均不变。

no-Z3 首次误用 main 谓词结果的基准，在谓词阶段被严格门槛拒绝，失败记录保留。
其正常 API 真实返回 `available=false`，visited/folded/queries等均0；这是构建能力差异。
另用冻结旧3 helper 对象、同 probe 和实际旧689e库执行独立17阶段 private no-Z3校准，
它完整验证、保留 ordinary/source/身份，表证明闭合但分发仍有循环，退出4表示结构未完成。
未执行新 C/Unicorn/native。以此独立基准按相同4文件有限补丁建立 no-Z3 专属 fixture，
实际80dc再跑19阶段，18项严格字节门槛及核心紧后/最终业务 SCC 均通过，循环分发为0。
没有把 unavailable 谓词当已证明，也没有为了匹配 main 改预算或删未知路径。

两套结果都是条件分析 view。main final bound SHA
`394faefad43ac21708d823cc09b0c8cec237048c45340ef4b0f8365b6c87872d`；no-Z3为
`46545f4ae28b64a6c454c30852c2eaa9449e63e6bae038fc35e220fad498d07b`。
真实新库 nominal 完整状态复验随后通过：main/no-Z3各32份原始66136字节均精确匹配原16份
Unicorn期望，各96个比较器错误对照通过，保存结果再次审计通过。新 C/Unicorn/before
执行均0。正式本阶段合计 normal320、实际fault128/128、比较器controls3520；其中合成
与真实域分别仍为64 module-input/16 arenas及16真实输入，不把 backend/hostopt重复
增加为新样本。

## 入口回退与 GuestSize 边界

另外冻结4个场景/3份物理初始frame：改变SP并迁移root sentinel；把6个表项各翻转最高位；
逻辑GuestSize=0；逻辑GuestSize=0xc808使业务完成而root RET读取越界。物理 backing始终
65536字节，输入/完整66136字节输出协议不变。只修改独立测试driver的显式逻辑size参数。

先运行原始LLVM8次，以4对O0/O2完整raw结果交叉校准，再执行两实际库各8次；24份正常
结果全部通过。SP miss实际root RET、RSPc820、业务结果一致；table miss实际产生
`[2,1,4787,0x80000000000012ba,1,0,0,0]`（7个Exit字段加budget），GuestPC为该具体目标；
size0实际因内存检查停止且Guest完全未写；late RET在0x100f停止，RSPc808并保留已完成的
业务输出。后两者为ABI的UNSUPPORTED(5)/MEMORY(3)，未声称执行了操作系统异常处理。

两种明确wrong IR各在两库/O0/O2运行：强制SP守卫通过，要求RSP与Guest均不同；在早退后
额外翻转Guest[0x8000]与Exit首字节，要求仅这2字节xor1。8次观察全部指定检出，另72个
比较器控制通过；artifact6314395字节，小于原12MiB预算。RESULTS `d206f45a…`，原始
LLVM交叉校准 `af89a845…`。没有新C/Unicorn或生产pass调用、skip、排除或归一化。

两个先前失败另保留：v1误调用不存在的saved-helper加载接口，在任何target前失败；v2
执行2个原LLVM场景后因把具体间接目标的出口误写为UnknownTarget(4)而拒绝。保存的raw
其实为TRANSFER(2)/BRANCH(1)，root按ABI及原lifter的jmp.unresolved路径独立确认。
一次未解析时的NeedTarget(3)推断也明确撤回。v3只改成精确的上述8word期待，不接受多种
reason、不改输入或比较范围。v2的2次尝试不充作成功校准，也没有产品错误结果被忽略。

当前正式成功统计可分为：基础normal320/fault128/controls3520，边界normal24（含8原LLVM
peer校准、16实际after）/fault8/controls72。合计344/136/3592时必须保留此分层，不能把
before-peer比较当独立机器oracle，或把重复backend/hostopt当新输入。完整逻辑size/错误
边界验证的是原LLVM与改写LLVM等价；没有新的Unicorn架构异常对拍。

## 最终封存

实现提交 `05c6ab2`、验证提交 `d347234`。源码包同时绑定实际编译的11份实现/头/CMake、
最终回归及4个实际执行的adapter；测试EOF例外保持独立证据。后置纯读取审计再次通过：
synthetic8条流/384帧、real64完整raw及192控制、boundary24/8/72、全部文件集/原始失败
与缓存事故恢复均复核，未再执行target。正式SUMMARY SHA
`dcd94605b02af2cba98f3239d6ea6602c60cca96e2948394f66966a16f42562d`；独立audit REPORT
`5cb69ce35b1124e6014bf678f6ae30c07aee5781b5b25596394fb8a06466bd91`。

三层完整档案在VM和Mac各执行一次unchanged-v5检查，逐成员集合、长度和SHA全部匹配，
无symlink/hardlink/special成员。源与4份旧完整档案、2份Git包的stat/ctime/hash在读取前后
保持一致。总计3775个payload、1727180227字节，压缩250668216字节；非target备份时间
单列，没有改变180秒target或64MiB运行门槛，没有归档后删除证据。

| 档案 | 压缩字节 | SHA256 |
|---|---:|---|
| historical-prototypes-and-campaigns | 108492232 | `0141795851ba5915bf68267a6fe092128356c2041e5bda341c7422506e7d927a` |
| actual-production-runtime | 99756720 | `c261f2c4a9344ddc6ebf023d088e75edd1e78d8b4e4ec01555c09e86c7d3ef7a` |
| final-production-validation-and-source | 42419264 | `ffefe433e229119e3e2fffbba91c739777763d22ee61d25ca594577162be7ae9` |

文件均为 `ordered-dispatch-<档案>-20261114-v1.tar.xz`。Mac目录：
`/Users/c/Desktop/deob/validation-artifacts/ordered-dispatch-components-20261114/`；VM目录：
`/home/cc/vmc-runs/`。总证书 `DUAL-HOST-COMPLETE-VERIFIED-v1.json` 的SHA为
`b973676a094513f2ece56d2a5a7cabff9479ec8d55a4a50dc619511d50c3447c`。这份证书记录封存时
的索引服务暂停状态；随后独立恢复记录 `TRACKER-SERVICE-RESTORED-v1.json`（SHA
`93bd6cc16222bb8a94fae59b43a388d85b9c19eb73114d4e038e2900435c17cf`）证明原static/active
状态已恢复，仅移除runtime mask，无persistent配置或gsettings修改。

索引缓存可重建，不是永久空间收益。恢复后实际可用空间约422MB，低于下一轮fresh build/
full所需1GiB；后续应先扩容或按既有双端验证/消费者闭包规则回收重复产物，不降低门槛。
本节是封存后记录，未声称被先前的Git源码包包含；实现及实测adapter源码保持不变。
