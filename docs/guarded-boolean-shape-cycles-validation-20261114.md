# 历史 Boolean 依赖闭图与旧候选优先（2026-11-14）

本轮只扩展 opt-in `guarded-store-expressions`。它验证不可信预测所需的
SSA 依赖形状，不计算循环整数值，不证明 definedness、Guest 内容或表不可变。
基线为 `f431ba4`；前轮的 native / 真实样本数量不计入本轮。

## 两个实现版本与不可放松的精度门槛

首版 `f31631a` 的闭图实现通过了 focused、双构建 full 和独立 native，
但真实 flatten-switch O2 丢了三条原已解析上下文，因此没有单独交付。
修正版 `f68b1ee` 将完整旧搜索放在前面，闭图只补充剩余候选预算。失败版本、
全部原始输出及门槛失败记录都保留，不以执行语义正确代替精度回归验收。

| 源文件 | v1 | v2 |
|---|---|---|
| GuardedStateCorridors.cpp | `8126c88859130b56eb2c32369a5698e39d09cf1e1d541ff1ac2a97f98b2ccc03` | `c4faad8a734765303ad4501de55cfeef979fc6b5f91bb22d76d12f8116e07d11` |
| TestLLVMGuardedStoreExpressions.cpp | `a55f98d87856c645aa7f0b6443952f6396bb66ff52546131ed54cad949a9a740` | `d795763093a838156a28a3530c52b3b2da0d44da2a23c204593a8cf89f475b3e` |

独立 oracle 固定为 `c074c3a9731216d3510813108a3b88a74c98f31f23b0528928b645afd8ff8f92`。
`native-inputs-v2/EMITTED.json` 为
`133394fd7e05373df264cca21923789333895a386b1de19e7e9328a7ad1e308d`；
86 个文本 payload 共 462235 bytes。oracle 与输入在产品 v1/v2 之间未变。

## 合同与调度

旧阶段保留 f431 的递归 shape 检查、acyclic cache、收费、BFS / visited /
continue、selected key 和 loop 排序。只有旧检查命中灰环且预测失败，才保留
已经收费的原始 Search 帧；原 incoming 循环结束后移动帧，不新增一次 BFS。
旧候选列表完整冻结以后，才重建这些原始请求并扣同一份剩余 Work。
闭图结果只能追加到剩余 candidate 槽，不能替换任何旧 selected key。
新阶段耗尽只停止扩展、标记 Incomplete，不调用 scanFailure 丢掉旧冻结前缀。
原始 ID、最终边适用性检查和 rewrite / block / instruction 预算仍共同适用；
新 clone 不成为本次的新候选。

新闭图内灰边只表示 pending 依赖，仍检查全部其余 operand / PHI arm。
整图、root 角色和整笔 publication work 都通过后才批量缓存；每个节点保存
其自身可达循环状态。cache hit 也检查循环 root：必须是 copied path 外、
支配 seed 原 terminator 的原 instruction。任何晚到的 poison / undef、
不支持节点、调用边界或预算失败，都不发布本次局部 cache。

原 nodes128 / depth32 / work1000000 不增加；继承 caller depth 和共享节点账，
灰边 / cache 引用计 work，首展开才计 nodes / depth。整数 get() 的循环拒绝不变。
LOAD / freeze、fshl/fshr 的原 whitelist / dominance、copied path 不含调用、
完整有限地址算术、fresh/history 区分、旧 profiles 和默认配方都保持。
原 flags / 运算 / 副作用、实际 state/target LOAD、bounds/error 和
同一次实际目标读取的完整 switch/default fallback 保留。预测不变成值事实。

## 回归：先看到失败，再验证修复

| 测试与 backend | 实际结果 |
|---|---|
| v1 新三 case 对 f431 main / no-Z3 | 每套3 cases /245 assertions，3 cases /3 assertions fail |
| v1 actual main / no-Z3 | 每套15 cases /3385 assertions pass |
| v1 历史 full main / no-Z3 | 1163/1163、1028/1028 pass |
| 最终测试对象对 f431 main | 4 cases /353 assertions，3 fail；旧候选优先 case 已通过 |
| 最终调度 case 对 v1 actual main / no-Z3 | 每套1 case /108 assertions，6 assertions fail，exit6 |
| v2 source-linked prototype | 16 cases /3493 assertions pass |
| v2 actual main / no-Z3 | 每套16 cases /3493 assertions pass |
| v2 历史 full main / no-Z3 | 1163/1163、1028/1028 pass |

新调度回归分别限制 max_rewrites=1、max_blocks=4、max_candidates=1，
确认较晚的旧普通源优先于较早的新循环源。原15 cases 未改；最终16包含
最初12个、三项闭图测试和这一调度测试，不把 focused 数量加入历史 CTest。
自/互 PHI、重复 incoming、freeze、反转 arm、灰边后的非法 arm、
非支配 funnel、未知调用、volatile/atomic、整数循环拒绝、cache root 角色，
以及 depth32/33、nodes128/129 都有 public pipeline 回归。
没有新增巨大1M-work压力 fixture；新阶段耗尽保留旧前缀的控制流经过独立审查，
不能把该审查称为已经执行的1M边界压力测试。

actual focused 客户端只链接 tests.o + Catch 和目标共享库，
没有 core / pipeline 对象或 shared LLVM interposition；LD_DEBUG 绑定实际库。
full 保持启动1GiB；有界准备 / native / saved audit 保持启动512MiB，
运行64MiB，每命令180秒。没有放宽门槛或超时。

## 三参考校准与固定域

独立 Python 用 unsigned32/64 递推和完整字节 arena 生成 expected。
原始 LLVM 与单独生成的 C 各自对它校准，transformed IR 不作为 expected。
O0 / O2 每级256 LLVM +256 C =512完整状态匹配、4608 comparator controls。
这是 before-only 校准，合计1024，不计为产品 after-native。

配置为2 widths ×2 shapes ×4 modes ×16 keys：ordinary、table-before、
table-after、wrong-hint。wrong-hint 的原 IR / C / model 与 ordinary 完全相同，
只有 manifest 不同。因此逻辑域是12个不同 source fixture pairs /192个
source-input组合，而非16个新业务函数 /256个不同业务输入。
每级实际32种初始 arena；不同进程的 host pointer 字节完整保留，不能将
跨运行的新 raw hash 当作扩大业务域。return +73728字节 arena 全部比较，
包含 registers、Guest、7个 Exit 字段、counter、history 和其余字节，无归一化。

校准 saved 终态的 seed-effect 分布每级为0次16、1次120、2次56、3次64。
final iteration/history 与 counter 校核一致；中间序列只是模型重建并对终态校核，
不是逐步 native trace。4次是循环上限，未声称实际覆盖4次。
8个显式 poison / undef PHI 文本只做 verify / refusal，不进入 native。

## 产品 native：v1 正确性与精度失败分别记录

v1 actual main / no-Z3 × host O0 / O2 四 run，每次512完整 before/after匹配、
4608 comparator controls、448 fault observations /316指定差异全部检出。
v1 合计2048匹配、18432 controls、1792观察 /1264指定检出；这四次重复同一域。
七类实际故障覆盖 initial/current、old/fresh、skip/double effects、iteration、
state 和 table-after fallback reLOAD。saved audit 重核全部 normal/fault tuple、
完整帧、EOF/CRC与逐fault计数，没有新增 native 执行。
这些结果属于 v1；不能据此跳过它已失败的真实精度门槛，也不能冒领为 v2 实际执行。

v2 actual main / no-Z3 × host O0 / O2 也分别重新生成机器库并实际执行。
每次512完整匹配、4608 controls、448故障观察 /316指定差异全部检出；
v2 合计2048 /18432 /1792 /1264，不挪用v1执行数量。这仍是同一192组合逻辑域，
重复 host / backend 不扩大输入域。

native-products-v2 的16正常+8结构输入每套均verify；正常32个产品全部产生保护边，
16个结构产品保持原样、不执行毒值 native。两套合计144份parsed-before/after/report
与v1逐字节相同；仍完成了上面的独立v2机器执行。最终v2 saved audit 已全部通过，四次c074审计与独立逐fault census
只读已有帧，重核全部256 normal /448 fault tuples的唯一域、EOF/CRC和逐fault计数。
不加载被测机器库，不增加 native 次数。29 payload /93306B小报告双端完全相同；
REPORT SHA256为`c98cea8cff0ce0490586302d7d41b13d8a2ed756c7f111ceaa2faa0fbc6db2b8`，
FILESET为`3f10047c9521fc74879df816bc06b0ea4a6c06cc2238be8a156506f643dd2713`。

实际 v2 main backend SHA256：
`0456b6dff29367bf6814b46e081688bcde54ddf32d935641248f316a26f70429`；
no-Z3：`fc1d4bfb637ba965c49853a69751c6f571972633fa78dd27d8670dda67284b09`。
FOCUS/FULL的原始source/config/runtime共409项hash及文件身份前后不变；
生产验证summary为`4f70b91d4b0efd289dc448f1067a3c8d51616c63da06f1841e55cea3f686b056`。

## 真实四图：保留旧收益，不宣称新增去平坦化

v1 四图各仅跑一次。前三图与 f431 baseline 完全相同；flatten O2 的
candidate196→199、unresolved171→174，其余报告保持25 rewrites、124 blocks、
4652 cost、loop0，却只保留旧25中的22条。旧丢失：
`bb_6057→5317`、`memory.valid65→6110`、`memory.valid69→5062`；
新较早源：`bb_5261→{4770,5693}`、`bb_5317→{4522,5185}`、
`bb_5588→{4858,5222}`。六组完整片段、原行号和文件 hash 均保留。
这是新候选与共享预算/顺序的精度回退，不声称已观测 native 错值。

v1 失败的77-payload /20556146-byte 包双端核验，archive SHA256：
`374fd40606c79606230cab343a1e0e0188eb02658eff3a183c4991a9f38d581c`。
没有重跑 v1 或放宽旧上下文保留门槛。

v2 prototype 四图与 f431 baseline 的 parsed-before / after / report 全部
12/12逐字节相同，旧丢失三条及全部25条恢复。改写数保持
manual O0/O2 =0/6，flatten O0/O2 =16/25；所有 loop_contexts=0。
四图 raw/hints 路径和64/32/128/16384配方未改，没有 normalize。
actual main/no-Z3 的24份parsed-before/after/report也全部逐字节相同，
LD_DEBUG仅加载对应新库；这些只是兼容性，不重算旧128次真实native，
也不表示本轮为这四个真实样本新增去平坦化收益。原条件48-block业务 SCC
来自前轮，outer dispatcher/125限制仍在。

## 独立审计与封存

根目录 VM：`/home/cc/vmc-runs/guarded-boolean-shape-cycles-20261114/`；
Mac：`/Users/c/Desktop/deob/validation-artifacts/guarded-boolean-shape-cycles-20261114/`。

- independent-calibration-audit-v1：20 payload /781113B，COVERAGE
  `92729d8600051f8d365e685ec738fa18458108b6d693f77a59f7a58ca8cf236c`。
- independent-native-v1-audit：29 payload /1909528B，COVERAGE
  `f786283bc7f88b12254becefe5bb2a040ce9d95406714b858cabe7964a040809`。
- real-v1-independent-audit：完整失败证据、六组片段及77-payload包，SHA如上。

real-v2-independent-audit 的独立checker重新读取并逐字节/hash比较原型12份、
actual main12份、actual no-Z3 12份，对应四组各12文件的完整set也核对。
raw/hints 8项固定pin match，61个读取输入pre/post hash不变。此审计没有产品、
native、C或Unicorn执行，也未复制大IR。小报告SHA256：
`b9f8c76dbe6d246a6ca90d6569d1773aba89d959eb8a2183073dc2117c9f464f`；
checker：`4ab292de4bcefe58619db14af5b432df720573f3e56a9f75c51b8b82fb13c7c6`。

完整封存文件为
`guarded-boolean-shape-cycles-evidence-20261114-v1.tar.xz`，SHA256：
`3c428cbb87be414fe081601b3174c03e7f9be7435e378bfe3831c124616302d4`。
包含1303 payload加manifest，187238648 compressed bytes、1126917679 logical bytes；
VM和Mac分别重新验证完整文件集、每项长度/SHA及archive SHA，无hardlink成员。
创建器还核对所有源文件的设备/inode/长度/mtime/ctime/hash前后不变。

包内同时保留v1失败和v2最终代码/runtime/prototype、两代full/focused、全部校准与
native流、三组独立saved audit、真实精度失败/恢复证据及清理操作记录。
resource symlink只记录不跟随；外部原raw/hints、source/object/cache依赖由原始pins
及00e/dc1/9c623等前轮完整归档绑定，不将不存在的旧runtime当作新live input。
`source-docs-pre-seal`是命名明确的封存前文档快照，本Git文档为最终seal。

包内source-git-f68b1ee.tar.gz绑定commit
`f68b1ee47054adc6e08f7b8ef072bc05471af834`，SHA256：
`911ef183132945a8bbc768a5cb9ad1dafe6bd23e52fbfca6955d7ade4264019d`。
源码包的最终cpp/test与c4fa/d795逐字节一致；manifest也核对两个最终actual backend。

本轮沿用已验证前轮archive，双进程扫描、文件身份/nlink/hash和保护文件核对后，
退休了上轮旧replay的460个非JSON expanded files，释放46112768B；
随后退休251个旧focused/native/重复IR输出，释放58073088B。
原raw/hints、f431比较基线、当前失败/最终证据均保留到本次封存；
历史重放必须按RESTORE说明恢复exact archive member，不能静默替换旧输入。
两组操作PRE/POST/脚本/恢复说明均在本包，测试门槛未降低。最终封存占用空间
与测试启动时快照是两个时点；下一轮仍须重新检查1GiB fresh门槛。

最终archive绝对路径：

- Mac：`/Users/c/Desktop/deob/validation-artifacts/guarded-boolean-shape-cycles-20261114/guarded-boolean-shape-cycles-evidence-20261114-v1.tar.xz`
- VM：`/home/cc/vmc-runs/guarded-boolean-shape-cycles-evidence-20261114-v1.tar.xz`

没有新的机器指令语义、IGNIL、单指令Unicorn或69输入全语料库 campaign；
本轮也没有新增真实 producer / disassembly / lifting 或真实 native campaign。
