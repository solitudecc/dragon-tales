# Dragon 当前阻塞与设计评审交接

日期：2026-11-15。**代码基线 `4c3c597b4263f4259876bd20312a6fa1882724b7`。**
本交接只归纳已测事实、待决策问题与验收条件，不把候选方向当成已验证方案。

## 1. 最终目标与范围

用户要求：**去混淆后的 IR/伪代码，应能清楚呈现混淆前函数的业务逻辑**，包括计算、
真实条件与循环、可观察内存读写和调用关系，行为保持等价。原变量名或源码逐字复原
不作为要求。CFG恢复、IR合法、测试通过、分发器减少只是中间成果。

主线是通用组件处理 MBA、控制流平坦化和不透明谓词。分析过程静态进行，Unicorn/
原生执行只作独立验证。VM、任意全程序堆分析、独立替换二进制不在本轮范围。
特殊保护以后接自定义 Pass。罕见的子函数写分发状态不作为当前主线。

目前业务观察契约、业务分析视图和可读业务伪代码仍未交付。曾经7150行的输出来自
完整执行状态模型，不能当成已恢复清晰业务逻辑。

## 2. 当前真实输入及两条不同配方

当前输入是自建复杂函数经 Tigress 2.1/revision2128 处理的 x86-64 ELF：

- 种子2026111307，InitEntropy/InitOpaque(array size16) → AddOpaque(true,count6)
  → EncodeArithmetic → Flatten(switch,随机块顺序、拆分基本块)。无VM或堆链表。
- 有嵌套循环、输入相关次数、动态索引、旋转、真实业务分支、字节覆盖和四字输出。
- `kernel=0x1010`；O2中央间接分发在0x11d6，表在0x2000，92槽/86个不同目标。
- `combined-O2.elf` SHA256：
  `72f3e6321c5ba5c40dbe18601ca2959fb92828c6a31231948cb0987176aae860`。
- Mac原样本：`/Users/c/Desktop/deob/ida-review-20261115-large-combo/combined-O2.elf`。

请区分两条路线：

1. **普通十步优化配方**已有 Guest-memory/MBA/predicates/dispatch 等组件。
   历史 `9404326` 产品上的O2测量是 MBA化简1处、条件折叠14处、块432→408、
   switch2→1、dispatch-deflatten改边0。条件折叠含提升器检查，不能当成消掉14个
   源级不透明谓词。这不是当前最新代码的重新测量，也不是下面CLI的配方。
2. **当前重点CLI**：raw静态提升 → 显式入口字节检查的独立guarded view →
   `loop-image-context` → 仅闭合成功后运行 `function-control-flow` 清理。
   它已有统一调用接口，但尚未把MBA/谓词/内存等优化事实充分接入同一证明上下文。

当前配方明确提供表目标、额外解码入口及入口候选；不是已完成任意黑盒的自动目标发现。
入口候选有实际检查与原逻辑回退，不是把镜像数据直接当永久不可变内存。

## 3. P0：最新卡点的确切事实

两套冻结正式SDK都报告：

```text
message = loop address projection exhausted: phi projection shared node or depth cap
available=1  complete=0  closed=0  budget_exhausted=1  work=889951
```

独立诊断将模糊的“节点或深度”消息区分为：

```text
LOW_CALL 3
LOW_PAIR large=dispatch.live2648 small=dispatch.live3741 nodes=0
LOW_PAIR large=dispatch.live2648 small=dispatch.live3571 nodes=64
LOW_PAIR large=dispatch.live2648 small=dispatch.live3403 nodes=147
LOW_PAIR large=dispatch.live2648 small=dispatch.live2900 nodes=236
LOW_CAP nodes=256 depth=1 work=36565
LOW_RELATION a= %dispatch.live2658 = phi i64 [ %dispatch.old779, %memory.valid51.dispatch.edge800 ]
b= %6034 = call i32 @llvm.fshl.i32(i32 %6011, i32 %6011, i32 %6032)
```

解释边界：

- 发生在第三个地址低位投影查询；该调用各pair共享256节点，节点同时包括关系对和
  已定义性节点。每个pair有新的私有Proof，但已花节点/work不回滚。
- 前三次尝试分别花64、83、89节点但没有导出成功证明；第四次余20节点时触顶。
  **未证明成功不等于已证明语义不等价。尚未证明存在或不存在一个可接受候选。**
- 本次深度仅1；外层work889951也未达到总1000000。256是软件证明节点计数，
  不是基本块数或机器内存容量。
- `fshl`是LLVM intrinsic，不是未知系统API。当前截断恰好停在它附近，不足以证明
  “只补fshl支持就能解决”或“必须做跨调用摘要”。定义检查目前对这类Call也很保守。
- `dispatch.live*`/`dispatch.old*`是本次冻结SSA的名字，只供定位，不是生产匹配规则。
- 整个调用在私有试验中工作；本次失败后公开guarded输入/输出IR逐字节相同。
  中央分发循环仍在。`preserved_stores=0`是还没走到后续写集合证明，不代表这些写
  已被判定无法证明；`constant_values=0`也不是本次MBA有效性测量。

### 3.1 实际调用链

```text
scripts/deobfuscate.py::workflow
  guarded_entry_view
  PassPipeline: loop-image-context
    runLoopImageTableContext
      guard/normalize/findTable
      threadDispatchCorridorBatch      # 现已通过这一步
      recoverLoop / loopRegion
      projectLoopAddresses
        对每个合格原始Trunc调用 replacePhiLowTruncs(f, {trunc}, limits)
          扫描全函数前驱/受保护效果、verify、构建DominatorTree
          枚举large同块同位宽且支配Trunc的small PHI
          每个pair新建Proof，但共享节点/work额度
          relation → definition(trunc) → definition(small) → realSeeds
          本次在relation中用尽节点
      loopPathRanges / loopCells / loopWrites / 最终复验与commit
        # 当前尚未到达这些后续阶段
  function-control-flow                # closed未成立，本次不执行
```

## 4. 需要方案解决的具体机制

### P0-A：候选试探费用与证明复用

`replacePhiLowTruncs`仅按同块、位宽和支配关系挑small PHI，按现有PHI次序逐个尝试。
`Proof::prove`先做关系，再验证两侧已定义性及SCC真实种子。尚未成功的pair不断消耗
共享额度；256触顶时，后续候选未必已被尝试。

请评估：更早的充分不匹配/不支持判定、候选排序/索引、义务顺序、已闭合事实复用、
关系与定义检查分离，哪一种能降低费用且保持健全性。若设计negative cache，必须
区分“不等价”“不支持/未知”“预算耗尽”，不能把bool false都缓存成不等价。

注意：转发/转换后两端可能汇合到同一SSA，不能只看当前opcode就随意剪掉候选。
同源但独立的freeze也不能合并。禁止把灰色/假设中的coinductive关系缓存成已证明。

### P0-B：每个Trunc重复构建函数结构

外层对多个Trunc逐次调用helper；每次helper都会重建前驱槽计数、效果快照、
DominatorTree和校验。源码已确认存在重复扫描，但未独立量出其全部成本。
36565是整个第三查询的work，不能全归为重复扫描。

请设计可复用的分析会话或分离的结构预检，并明确IR版本与失效规则：

- `threadDispatchCorridorBatch`体事务会使旧Instruction/BasicBlock句柄失效；
  Function对象身份保持并不意味着分析缓存有效。
- 投影本身不改CFG/效果节点，但会改纯SSA操作数。仅凭Value*、CFG不变或直接
  STORE指针对象未变，不能无条件复用所有依赖/数值事实。
- 已证明事实、待证假设、Current/Historical执行、不同guard上下文必须分清。
  所有实际扫描/查询/维护费用仍需入账，失败费用不能重置。

### P0-C：局部精度增强失败与最终闭合的策略

当前 `projectLoopAddresses` 遇普通Unknown可继续；一旦helper报告预算耗尽，就让
整个loop请求立即退出，后续表/写集合义务不再尝试。

请明确哪些投影是必要义务、哪些仅增强精度。是否可能在不导出失败事实、不隐藏
预算、不改变原逻辑且仍证明全部最终义务的前提下，把某些局部失败视为Unknown？
目前没有实现或证明这种策略，也不能承诺这样就能让样本闭合。
**直接清budget标志、丢掉未证明路径、继续把请求报complete，不是可接受方案。**
默认兼容性、可选策略、停止条件和整体状态语义都需在方案里明确。

### P1-A：优化组件尚未充分协同

统一Pass接口已经存在，缺口是受控证明视图中的事实与调度协同。当前CLI的loop证明
并不包含普通十步配方的完整MBA/谓词优化。在guard构建前先任意优化raw，会破坏
现有canonical raw入口前提；不能优化后仍假装来源/快照/观察点完全没变。

需设计如何承接内存、MBA、谓词和有限状态事实，如何在CFG/SSA变化后失效和复验。
这部分不是重新写一套反汇编器，也不是优先扩展罕见跨调用写状态。

### P1-B：执行模型IR到业务逻辑之间缺一层

当前完整状态IR携带寄存器快照、Guest内存访问、边界/错误出口、预算和回退逻辑。
把它直接交给反编译器会暴露大量执行模型细节。缺少显式业务观察契约与独立业务视图：

- 输入ABI、返回值、输出/可观察内存、调用及错误行为如何定义；未知原型怎样保留。
- 栈槽/变量/数组及真实循环如何恢复，哪些状态可被证明与观察无关。
- 完整状态等价与业务投影等价怎样分开验证；原回退及未证明副作用怎样保留/展示。
- 怎样交付可读IR或伪代码，并与混淆前源码/独立规格逐项对照；原源码仅作测试参考，
  不提供给生产Pass来生成答案。

### P1-C：状态与诊断可读性

本次保存的顶层pipeline仍有 `ok=true, converged=true, budget_exhausted=false`，
但具体run为status2/incomplete，内部counter `budget_exhausted=1, complete=0, closed=0`。
顶层字段描述调度/执行层，不能替代具体证明结论；CLI已检查具体run并正确退出3。
这不是已发现CLI误报成功，而是对外报告容易误读。生产消息还把node/depth合并，
本次需诊断DSO才能分辨。请评估诊断分类和接口清晰度，保持现有语义兼容。

## 5. 代码入口（4c3c597基线行号）

| 文件 | 重点 |
| --- | --- |
| `lib/LLVMPhiLowBits.hpp` | Options/Stats及原子性契约；256/32/65536限制 |
| `lib/LLVMPhiLowBits.cpp:27` | Work与node/depth计费 |
| `lib/LLVMPhiLowBits.cpp:104` | relation、转发尾部迭代、普通PHI递归 |
| `lib/LLVMPhiLowBits.cpp:141` | definition与原始依赖图 |
| `lib/LLVMPhiLowBits.cpp:185` | realSeeds，循环必须有真实PHI外部种子 |
| `lib/LLVMPhiLowBits.cpp:241` | prove义务顺序 |
| `lib/LLVMPhiLowBits.cpp:248` | helper整体；全函数扫描、pair枚举、延后提交/回滚 |
| `lib/LLVMImageTableContext.cpp:2139` | 每Trunc调用及局部预算向外升级 |
| `lib/LLVMImageTableContext.cpp:2726` | loop总流程及body事务后句柄恢复 |
| `lib/LLVMImageTableContext.cpp:2772` | 投影在后续写集合证明之前 |
| `scripts/deobfuscate.py:274` | 当前CLI：proof闭合门后才cleanup |
| `lib/PassPipeline.cpp` | 统一调用接口及状态层次 |
| `tests/lifters/TestLLVMPhiLowBits.cpp` | 现有正例/拒绝/原子性/转发回归 |

## 6. 已解决项与验证边界

不要再按旧问题设计：

- `5e13a05`已支持任意位置one-hot AND/最高位提取的完整二值观察，原case78/
  0x1e3d的 `{0,16}→{60,76}` 不再是当前首个拒绝。
- `8a339a7`已在同一未修改批次中复用完整原SSA使用检查，解开原batch预付阻塞。
- `24c72f8`已对关系/定义转发尾部使用迭代；当前不是原来的33/32深度拒绝。

最新 main/no-Z3完整CTest：1339/1339、1193/1193。迭代转发四个LLVM夹具得到
2048 raw+2048 after完整80字节匹配、12288比较器控制、1024指定错误检出。
主/no-Z3相同IR只执行一次；保存20流审计不重算执行。它们不是完整机器66112字节
协议或新的真实函数数量。先前真实组合的完整机器状态对拍属于旧版本/配方证据，
本轮未重跑。当前四次正式真实回放是静态API检查。

普通大Flatten样本保持闭合，raw/guarded/loop/clean四份IR在两端都与上一版逐字节
相同，work747565；**这不能代表当前三重组合已闭合**。六个源级不透明谓词尚未逐项
验收清除，业务伪代码也没有完成。

必须遵守 `docs/correctness.md`：旧代码失败的新回归、完整比较、边界输入、错误控制、
负例、真实样本收益、限制和失败证据分开报告。不得用提高默认阈值、删比较、归一化
差异、假定Guest不别名或镜像永久只读来过关。现有helper的全候选原子性、实际重复
边槽、支配关系、undef/poison/flags、独立freeze及SCC种子条件必须保留。

如果更换API/返回结构，需说明ABI与旧测试可编译性；C++按值返回的内部Stats也有
布局影响，不能让新旧SDK在不匹配布局下混用。缓存设计必须有失败假设泄漏反例。

## 7. 请评审AI交付什么

请先给方案，不实施代码，并明确已确认原因与待实验假设：

1. P0的首选算法及备选，具体修改文件/函数、数据结构、复杂度和预算账目；
   不以单纯提高256/32/1000000为主方案。
2. 缓存键、证明义务分离、pending/closed事实、IR版本、失效/回滚规则。
3. 局部Unknown/Unsupported/Budget与整体closed/complete的严格定义；是否要改变
   既定策略、默认行为或增加显式选项，充分条件是什么。
4. 最小回归：正确候选出现较晚/根本不存在、失败pair灰色缓存污染、独立freeze、
   真实/历史值、重复入边、未锚定循环、flags、未知读取、查询间操作数变化、总预算
   真耗尽；旧代码必须实际失败，不能只证明能编译。
5. 真实样本验收分两层：P0先推进该组合的实际闭合和真实分发结构；随后P1交付业务
   观察契约及可读IR/伪代码，对照真实计算/循环/读写/调用，不把更多统计当终点。
6. 若P0修复仍不足，列出下一项最小诊断实验及其能区分的假设，避免无依据扩大到
   全程序堆分析、VM或任意系统调用效果推断。

## 8. 本地评审材料与复现

仓库：`/Users/c/Desktop/deob/dragon-tales`。
最小评审材料：`/Users/c/Desktop/deob/ai-review-20261115-current-blockers-v1/`。
其中：

- `diagnostic/proof-3.ll`：第三次查询进入helper前的完整私有模块；
  `diagnostic/run.stderr`：上面的真实节点轨迹。仪器化源码单列，不能当生产版本。
- `production/`：正式run报告、入口manifest、原配置和guarded输入；
  `repo/`：上述基线的相关生产代码、测试与正确性文档；
  `input/combined-O2.elf`：SHA验证的原ELF。
- `REVIEW-MANIFEST.json`记录逐文件大小/SHA、基线和原归档来源；准备评审材料没有
  新增目标执行。配置中的原image路径是ROCK路径，移机复现只可另存修改路径的副本，
  保留原配方及其SHA，不悄悄修改事实或预算。

完整新证据包：
`/Users/c/Desktop/deob/validation-artifacts/phi-forward-sealing-20261115-v1/phi-forward-complete-evidence.tar.gz`，
SHA256 `d9620b5d7ffb66fb83da8cab4eb13d93decbf50296384f67ebf26283ada64cd7`。

构建/复现主机为 `ubuntu@192.168.10.241`，不能再依赖169。冻结SDK为
`/home/ubuntu/vmc-runs/phi-forward-depth-20261115-v3/production/SDK-main`，
no-Z3为同目录 `SDK-noz3`；配置为
`/home/ubuntu/vmc-runs/large-combo-loop-20261115-v1/config.json`。
实际CLI命令、库路径和SHA在生产报告及 `diagnostic/RUN-COMMAND.json` 中，诊断
Python/动态库环境见 `diagnostic/run.py`。
活动构建目录 `~/dragon-tales-rock5b` 仍是历史Git基线加受控覆盖，勿盲目reset或
把其HEAD误称为当前干净源码；优先使用冻结产品。版本、依赖与构建规则见AGENTS.md。
