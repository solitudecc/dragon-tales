# STORE 上下文与多级内存 hub 的独立验证

本轮基线是 `a0b92a3` 的 ec77 冻结运行库，实现里程碑是 `a5eb193`。独立验证只新增
`scripts/validation/store_context_*`、`guest_memory_hub_*`、`store_hub_*` 和
`guest_memory_saved_probe.cpp`；
没有修改此前 producer、字节模型或精度门槛。生产实现和单元测试由另外的工作分支完成。
本记录区分 native 语义、结构精度、真实样本重放与只读分析探针，重复运行和中间候选不累加。

## 独立模型和完整比较

`store_context_native.cpp` 在原历史值 fixture 上另建 STORE 变体：进入上下文先更新
Guest 中的执行次数，再按顺序执行 payload STORE、真实新 LOAD 和第二次 STORE。
旧 snapshot 仍参与历史 PHI，新 LOAD 必须看到本次 STORE。变体覆盖 Guest、寄存器、
Guest/regs 实际重叠、descriptor.size、i16/i128、历史值、外层条件、回入、效果屏障
和模块预算。重叠变体的 512 字节寄存器实际位于 Guest `[0x20,0x220)`，不是两份
事后拼接的独立数组。效果写到寄存器 offset96，避免被结果输出覆盖。

descriptor.size 变体会真实改写描述符，模型按新 size 检查随后 LOAD 的边界。
`descriptor.data` 始终与该次 invoke 的实际 Guest 分配地址严格相等；host 指针没有
被放入完整 Guest 范围，因而不需要掩码、归一化或忽略地址字节。正常矩阵覆盖
0、1、50、51、0x8000、0x80000000、最高位和全一，两个 size，以及有限回入。

`guest_memory_hub_native.cpp` 独立构造 H0→H1→J，分别从每层 PHI 选择实际入边。
常量 STORE 来源含 50、23、35，deep 变体强制走完三层；未知、部分写、其它写、
调用、volatile/atomic/fence、历史依赖仍保留原路径。loop 变体回到原 J 后读取新的
内存值，liveout 变体检查跨原 region 的 SSA 使用。精度门槛要求至少一个已证明
来源的实际 LOAD 消失，不要求一轮把所有来源一起复制；其它来源仍参加完整语义比较。

两模型都比较返回值、调用次数、完整 512 字节 regs、完整 64 KiB Guest、描述符
和七个 Exit 字段，生成前后 IR 各以 O0/O2 编译。普通 hub snapshot 为 66120 字节；
STORE snapshot 追加八字节 descriptor.size，为 66128 字节。无排除、跳过或归一化。
这些 native 矩阵使用 main 冻结库；不宣称另跑了 no-Z3 native 矩阵。

| 同源矩阵 | 输入数 | 正常 O0/O2 前后观察 | faulty 观察 | 预声明错误检出 | 比较器控制 |
|---|---:|---:|---:|---:|---:|
| STORE | 1040 | 4160 | 2176 | 1262 | 12483 |
| 多级 hub | 1408 | 5632 | 1472 | 734 | 15491 |

STORE 的九个 fault 是删 STORE、逆序、陈旧 LOAD、重复 STORE、未知状态强行已知、
绕过 bounds、错误 default、混淆历史与新值、陈旧 descriptor.size。hub 的七个
fault 是选错路径、删 STORE、未知路径、bounds、回入、历史和 liveout。每个控制的
目标输入集合在执行前由独立模型确定；正常与 faulty 流都按同一个完整字节格式比较。
单字段与长度错误控制证明比较器可检出对应错误。

## API 和旧版对照

旧默认 API 和显式 `allow_plain_stores=0` 维持原保守行为。新能力通过既有
`PassPipeline` 数字选项 `allow_plain_stores=1` 调用，不依赖旧库不存在的 C++ overload。
旧 ec77 对该选项明确报告 unavailable，IR 保持逐字相同，执行数为零；这不是旧语义
失败。旧默认 STORE 与 hub 基线都实际完成完整 native 矩阵。

最终旧默认 STORE 同源基线是 `store-old-default-v2-source`，旧 hub 是
`hub-old-default-attempt002`。它们分别达到 4160/1262 和 5632/734，未知和效果路径
按原规则保留。先前 STORE 模型加强前的 1266 控制和所有 v1/v2 中间运行均单独保留，
不混入这些同源数字。

最终 v3 同源结果如下，均为正常差异0、预声明错误全部检出：

| 最终组 | 正常观察 | 预声明错误检出 | 精度结果 |
|---|---:|---:|---|
| store-old-default-v2-source | 4160 | 1262/1262 | 旧默认 STORE 屏障 |
| hub-old-default-attempt002 | 5632 | 734/734 | 旧版不穿过新增 hub |
| store-candidate-v3 | 4160 | 1262/1262 | 八类实际 LOAD 收益及模块预算通过 |
| store-candidate-default-v3 | 4160 | 1262/1262 | 默认仍保持 STORE 屏障 |
| hub-candidate-v3 | 5632 | 734/734 | 六类实际 LOAD 转发收益通过 |

新显式0、旧 free-function/LLVMLifter overload 和其它拒绝值另由根任务单元测试覆盖；
这里的默认 native 组没有冒称额外执行了显式0矩阵。根任务 main/no-Z3 聚焦组各
129 cases/6720 assertions，完整 CTest 分别1000/1000、865/865。

## 已保存流的二次核验

`store_hub_review_saved.py` 不执行目标或 Dragon 分析。它加载每份 report 冻结的
模型源，对 20 条正常 snapshot 流及所有 faulty 流重新逐字节核对，检查 artifact SHA、
输入键类型、重复/额外/缺失行、预声明错误集合、字段列表、总覆盖和 EOF。
最终审查严格使用上表 old 两组与 v3 new 三组，文件为
`final-native-saved-audit-v3.json`。这使报告摘要之外的完整状态也可独立复查；二次
核验的 new native/analysis 都是0，不使用 v2 中间结果替代最终候选。

## 真实样本的固定契约

`store_hub_real.py` 复用原 `guest_memory_checked_edges_real.py` 的四个冻结 ELF、
线性反汇编、StaticAnalysis/FunctionStart、四个边界整数和 product-O0。
只增加显式选项和只读保存 pass 前后 IR。每个 raw/post 组保持 C16/LLVM16 的完整根
比较，16 个 regs、16 个 Guest 和 112 个 Exit 字段错误控制，以及 Graph 全字段和
runtime/input hash 检查。raw 与 post 是不同流水线，不能互换前后输入。

新增精度必须由正确 CFF sourcePC 的 switch 实际消费一个 CFG 可达的 constant-index
Guest LOAD。脚本自身有 1 个正例、7 个假收益反例和 2 个 switch 保留反例：断开的
LOAD、错误 Guest base/sourcePC/入口、不可达/恒假路径、错误 index 和被删除或改写的
原 switch 都不能通过门槛。原 LOAD 支持的 switch cases/default 必须保留。

v2 的 raw 语义比较 C16/LLVM16 通过，但严格 O0 index70032 门槛失败，wrapper 返回1。
保存的 raw `memory.valid9` 没有可选的 state PHI，不能把这一结果改称精度通过。
v2 post 则实际出现 index70032 的新 CFF LOAD；原 66 cases/default/LOAD 和程序原
switch 保留。CFF switch 1→2 是上下文复制，不能称为消除了平坦化。

最终 v3 原样重跑这两个真实流水线，结果没有掩盖 raw 的精度缺口：

| 流水线 | C/LLVM 完整根匹配 | f-cff O0 实际 index70032 | GuestSSA 首轮 / post 额外转发 | dispatch branch 改写 |
|---|---|---|---|---|
| raw | C16/LLVM16 | 无；严格 wrapper 返回1 | 0/0/1/0 / 无 | 0/16/0/3 |
| post-GVN | C16/LLVM16 | 有；严格 wrapper 返回0 | 0/0/1/0 / 1/0/4/0 | 1/16/0/3 |

四模块顺序是 f-cff O0/O2、f-combo O0/O2。两组各自16个 regs、16个 Guest、
112个 Exit 错误控制全部检出，Graph 全字段和冻结 runtime/input hash 保持。
dispatch 仍四模块 incomplete，GuestSSA raw 两模块、post 四模块 incomplete；
switches_rewritten 均为0。f-cff O2 原有13个实际索引保持，f-combo 没有新增常量
CFF target LOAD。真实新增收益只有 post 的 O0 已知状态上下文，不是 CFF 消除。

post v2 对 f-combo O0 的 GuestSSA 转发从旧4降为3。对完全相同、未 tag 的
`before-memory-stage-1.ll`，同一个仅用既有 public GuestSSA API 的 probe 分别加载
冻结 ec77/v2：旧 forwarded4、blocks3731、phis231；新 forwarded3、blocks3729、
phis194；两者 loads225、stores279、complete0、budget1。ldd 和前后 hash 确认
实际使用不同冻结库且输入不变。这是必须解决或说明正确性原因的精度回退；v2 全部
正常语义结果作为中间证据保留，不据此宣称最终精度保持。

定位结果是 v2 将新 hub 所需的较强 SSA 修复 scope 也用于 legacy direct path，
前三条路径的 repair cost 已到224996，加上第四条46816超过原262144上限；旧四条
合计248428。v3 对 direct 恢复原 scope，新 hub 保留完整跨原 region 修复，预算不变。
另外的合成模块用例发现了新搜索可能先消耗后来 legacy path 的模块预算，v3 因此
采用全模块 legacy-first。这个 priority 问题与真实4→3的 repair-scope 根因不同。

最终 v3 对完全同一输入重新核验：forwarded4、blocks3731、phis231 恢复，八个旧
复制块身份和内容均保留。特别检查 memory.valid1429 的实际 STORE27 进入
bb_80464.guest.edge，再到 memory.valid1435.guest.edge：copy 中 i32 LOAD 被
`sext i32 27 to i64` 取代，原 LOAD、两层 bounds 和 failure exits 保留。
**整个 v3 after.ll 与旧 ec77 after.ll 逐字节相同**，SHA-256 为
`493665eca6d4c4f120ff1dbe10bd1f2931b251851b5ff2f128d1c7bae1665f4c`。
这个 probe 只执行一次 GuestSSA 分析，目标 native=0，不冒充新增样本重放。

## 有界重复的分析探针

`store_hub_repeat_probe.cpp` 对已保存 f-cff O0 post 的 before/after STORE-context
IR 各重复三轮 GuestSSA 与显式 STORE specialization，不提高预算、不覆盖原 IR。
`store_hub_metric_fold.cpp` 只在另存度量副本中折叠操作数全为 ConstantInt 的整数
运算/cast/比较/select；不折叠 LOAD/PHI/freeze，不把这些副本反馈到下一分析轮。

v2 两个起点均为：第一轮实际 CFF LOAD indices `[69760,70032]`，第二轮增加
69736，第三轮增加69912。每轮 memory_forwarded1，所有轮仍报告 memory/state
budget exhausted。共六次 GuestSSA 和六次 specialization，仅是有界分析收益；这些
新路径没有额外 native 复验，目标执行数为零，不能累加到前面的 C16/LLVM16。

## 中间失败和证据位置

运行证据根目录为 VM `~/vmc-runs/store-hub-reference-20261111/`。最终 main 冻结库
为 `~/vmc-runs/store-hub-root-20261111/candidate-runtime-v3/`，backend/shim SHA-256：

```text
b2449677c1cfaa247d90286dd4ed8dbefbf944c0f2ee1e30488c3c1eac4e747f
e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec
```

v1/v2 的原 so 后续压缩保存，旧 ec77 来自已完整双端归档的
`future-region-root-20261111/final-runtime/`；其旁边恢复公告保留。final-summary.json
绑定最终五个 native 组、两个真实组、audit、source 和 runtime SHA。同输入回退证据在
`same-input-combo-probe/pinned/`；有完整前后 pins 的重复探针在
`repeat-probes-pinned/`，最初相同探针仍原样保留在 `repeat-probes/`。

为给最终 v3 腾空间，中间 v2 的1935个文件、404574409字节已逐文件在 Mac/VM
核对，并验证 tar 每个 member 后，部分展开目录退役。中间 tar 为
`~/vmc-runs/store-hub-reference-intermediate-v2-20261111.tar.gz`，131743398字节，
SHA-256 `decc2cdbf0192c94d9a73ca08043242fcf998deba23eeb0fe7877042dd2f6bc9`。
最终 old 三组、harness、同输入 probe、真实摘要和四个关键 post IR 保持原路径；
精确1521个退役文件/414个保留文件和完整恢复命令见相邻
`store-hub-reference-20261111-PARTIAL-ARCHIVED.txt` 及 retirement JSON。
原始重复探针现在通过该 tar 恢复；后加的 v3 文件不在这份中间 manifest 中，未删除。

最终归档是 `~/vmc-runs/store-hub-reference-final-v3-20261111.tar.gz`，112895641字节，
1142个 regular files、336478476字节原始内容。VM 当前集合及 tar 每个 member 均按
完整 path/size/SHA 核验，下载 Mac 后再次核对每个 member。最终 tar/index SHA-256：

```text
a795157ea1dfafad4690e9b55f470efafb152d0335fd9d603559c5a0447a04eb
d254a14cc0fe3f62ee06ced548d6b48df7fe3da6fff87e1f8780eae85a828d09
```

index 为相邻 `store-hub-reference-final-v3-20261111.index.json`。两个 tar 的414个
重叠文件逐字节相同；先恢复中间 tar，再覆盖最终 tar，可重建完整2663文件证据集。
最终 tar 内 `archive-records/` 保存中间 manifest、精确退役记录和两步恢复命令。
不能只保留最终 tar 而丢弃中间失败证据。两份 tar、index、最终 summary/audit 和
双端核验 JSON 都已下载至：

```text
/Users/c/Desktop/deob/validation-artifacts/store-hub-reference-20261111/
```

STORE attempt001 的 C++ fixture 引用误解引用错误和 hub attempt001 的 IRBuilder
构造错误均在目标执行前发生，日志保留、native=0。旧新 API 不兼容、编译失败、
结构精度失败和实际语义差异分别报告。没有本轮新指令/IGNIL/69 输入全语料验证声明。
