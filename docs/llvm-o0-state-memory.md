# 保留 STORE 的状态上下文与多级 Guest 内存汇合

本轮基线为 `a0b92a3`，实现里程碑为 `a5eb193`，旧运行库为 ec77 冻结版本。目标是修复上一轮明确定位的
两个 O0 阻塞：可复制状态检查内存在真实 STORE，以及已知 STORE 与 state LOAD
之间隔着多层 PHI 汇合。生产实现不依赖 Tigress 名称、函数名或样本地址。

## STORE 上下文的显式入口

`dispatch-specialize` 增加数字选项 `allow_plain_stores`，只接受整数 0 或 1，
默认值为 0。C++ 新增自由函数及 `LLVMLifter` overload，最后一个参数为
`bool allowPlainStores`；原 overload 仍传 false。没有给按引用传入的 options
结构追加字段，没有改变已有 C/Python ABI、默认优化流程或 Guest 入口契约。

显式启用后，可复制的 STORE 必须是 AS0、simple、i8/i16/i32/i64/i128，且地址和值
不是字面 undef/poison。复制保留原事件顺序和执行位置，后续 LOAD 仍重新执行。
该选项不凭 STORE 推断内存事实，不假定 Guest、寄存器或内存描述符互不别名。
pointer/FP/vector 值、其它整数宽度、volatile/atomic、CALL 和同步仍是复制屏障。
已有历史值快照、live-out 修复、外层守卫、未知状态和预算上限继续适用。

动态 `freeze` 和带 poison 语义的计算可以按原位置复制，但不能被当成已经证明的
常量。对应单元测试检查结构与保守性；没有为 undef 任意选一个数做原生正确性声明。

## 多级 PHI 汇合的逐边内存证明

GuestMemorySSA 的 checked-edge 路径现在可以从原单入口检查链向前扩展到多层
只读汇合块。每层 PHI 都选择该路径实际经过的前驱，再证明所选前驱上的最近
精确 STORE 与最终 LOAD 的字节地址、宽度和值一致。未选入边、越界出口、未知路径
及真实后续读取都保留；没有把整个汇合块的状态统一为某一条入边的常量。

每个原始 LOAD 在一次调用内最多生成一个路径副本。复制路径不包含回到自身或较早
成员的环；副本侧出口仍进入原程序，原程序重新执行时不会错误复用副本中的定义。
扩展路径修复跨原块使用及边上 PHI；不支持的异常 live-out 保持原路径。
原有直接路径继续保留其既定异常处理契约。

预算没有增加：路径最多 6 块/256 条指令，模块最多 64 个上下文、16384 条复制
指令、262144 个 SSA repair cells、2048 次边证明和 65536 个索引节点。
先处理所有函数原有的普通、字节及直接 checked-edge 机会，再以剩余预算搜索新增
汇合路径。待处理列表只保存已完成普通改写后的原始 LOAD，不重扫新副本、不重复
计候选；使用时重建当前地址和支配证明。

## 验证中发现并修正的候选回退

v2 的同输入真实 f-combo O0 转发数从旧版 4 降到 3。独立 probe 固定输入、公开
GuestSSA API、二进制和库路径，定位到 `bb_80464` 至 `memory.valid1435` 的原有
直接副本缺失。插桩实现生成的 IR 与未插桩 v2 逐字相同。

原因是把新汇合路径所需的跨原块修复范围也用于旧单入口链，多计了内部使用的
修复工作。v2 前三次耗费 224996 cells，加第四次后为 271812，超过 262144；旧
路径实际共需 248428。v3 对未扩展的单入口链恢复原修复范围，扩展路径继续使用
更强修复。预检计费和实际 `RewriteUse` 共用同一判定，没有提高预算。
原链除首块外均为 single-predecessor，内部只有前向边，外入边不能绕过原定义；
首块 PHI 上来自链外的使用仍然修复。

另一个独立压力夹具发现，新增汇合搜索可能先耗尽边证明预算，阻挡后续函数原有的
直接转发。它不是上述真实回退的原因。v3 的模块级“原直接路径优先、汇合搜索随后”
解决该问题；相同测试二进制在旧主线 1/26 通过、v2 有 3 条断言失败、v3 1/26
通过，用于防止候选引入精度回退。

## 回归与原生验证

新增 STORE 11 用例、多级汇合 13 用例和模块预算优先级 1 用例。STORE 测试用
旧头文件编译，通过既有 PassPipeline 调用新选项；旧库 11/83，10 cases、31 条
断言失败，原因是旧选项 unavailable，不能称为旧语义算错。原默认 API 和显式 0
仍实际执行并通过。

汇合测试同一二进制接旧库 13/476，7 cases、93 条断言失败；接 v3 为 13/636，
全部通过。加入原 checked-edge 组后为 29/1509。STORE 同一二进制接 v3 为
11/503，全部通过。主构建和无 Z3 相关聚焦组均为 129 cases/6720 assertions。

完整 CTest 主构建 **1000/1000**（236.33 秒），无 Z3 **865/865**（200.54 秒）。
新库 STORE 显式开启和默认两组各 4160 次完整状态匹配、1262/1262 个预声明错误
检出；多级汇合 5632 次匹配、734/734 个错误检出。三组共 13952 次正常观察，
无差异、排除、跳过或归一化。旧库同源 STORE 默认和汇合基线也实际执行，各为
4160/1262、5632/734。旧库不支持新 STORE 选项的运行执行数为零，单列 unavailable。
独立复核最终五组的 20 条正常字节流及全部 faulty 流，输入、长度、字段及错误集合
均完整。中间 v1/v2、严格精度门槛失败及编译失败分别保留，不累加到最终数字。

旧头客户端另在 options 末尾紧接不可访问页，验证原对象大小及旧 overload 无越界
读取；最终四种模式和两个 layout probe 保持原输出与符号。该 ABI 检查不执行 Guest。

独立模型、完整字节比较和故意错误控制详见
[STORE/汇合独立验证](store-hub-independent-validation-20261111.md)。本轮没有新增
机器指令语义，没有另跑单指令/IGNIL 差分或 69 输入全语料；其结果不能由本轮
CTest 或原生夹具替代。

## 真实收益与边界

真实样本必须区分 raw 与 post 流水线。post 先进行已有 GVN/内存处理后，状态
PHI 才能供上下文证明使用；单独开启 STORE 复制不保证 raw 中出现同样的 PHI。
精度门槛要求实际 CFF sourcePC 的 switch 消费 CFG 可达的 constant-index Guest
LOAD，保留原 LOAD、所有 cases 和 default。RIP 入口路由或孤立常量不算收益。

最终 raw/post 各 **C16/LLVM16** 完整根匹配；各 16 个寄存器、16 个 Guest 和
112 个 Exit 字段错误控制全检出，Graph 及冻结输入不变。raw 的严格 O0 index70032
门槛仍失败，wrapper 返回 1 原样保留；post 门槛通过，f-cff O0 的真实 CFF source
`0x102d8` 消费新增 index70032 LOAD。原 66 cases、default 和 LOAD 保留，CFF
switch 1→2 是复制。业务 switch 仍 1→1，所有 `switches_rewritten` 为零。

四模块 GuestSSA raw 转发数为 `[0,0,1,0]`，post 额外阶段为 `[1,0,4,0]`；
f-combo O0 恢复旧版 4 次。相同原始输入的完整 after.ll 与旧版逐字节相同，SHA-256
为 `493665eca6d4c4f120ff1dbe10bd1f2931b251851b5ff2f128d1c7bae1665f4c`。
状态分支改写 raw `[0,16,0,3]`、post `[1,16,0,3]`；四个 dispatch 模块均 incomplete，
GuestSSA raw 两个、post 四个 incomplete。O2 的既有 13 个实际状态未增加。

这是有界逐边证明，不是一般循环内存不动点，也不保证单次调用解开全部多层汇合。
中间 v2 的六轮 GuestSSA/六轮 specialization 分析探针虽恢复更多索引，但没有对
新增输出另做 native 验证，不能累加到上述完整根匹配。

即使已知状态能确定表索引，表内容仍是任意合法 Guest 内存的一部分。保留并复制
实际 target LOAD/switch 是路径恢复，不是消除了原分发器。若没有实际初始化写入
或独立入口契约证明，仍不能从 ELF 表字节直接推断目标恒定。

## 冻结与恢复

最终 main 运行库为 VM `~/vmc-runs/store-hub-root-20261111/final-runtime/`，
backend/shim SHA-256：

```text
b2449677c1cfaa247d90286dd4ed8dbefbf944c0f2ee1e30488c3c1eac4e747f
e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec
```

根证据包 `~/vmc-runs/store-hub-root-20261111-artifacts.tar.gz` 包含 160 个数据文件、
161 个 regular files，绑定 385 个语义源码和 31 个验证依赖文件；完整 index 与
本地复核回执位于其旁及 Mac
`/Users/c/Desktop/deob/validation-artifacts/store-hub-root-20261111/`。
包 SHA-256 为：

```text
bd0e59318ea991d019c87fd47b0b5e4eb1c6821b736bdd591c07a2d695fd8105
```

独立模型与真实重放的两个完整归档、1142 个最终文件、2663 个历史及最终文件的
恢复顺序见独立验证文档。所有目标进程结束后，VM 再次按完整集合/大小/SHA 核对
1142 个最终文件，才退役 reference 展开目录；相邻
`store-hub-reference-20261111-ARCHIVED.txt` 保存两步恢复说明。失败与中间结果保留
在双端归档，不再将已归档原路径当作仍可直接读取。

根包也通过双端完整校验后，展开目录仅退役六个压缩运行库副本及三个独立测试
二进制，final V3 pair、源码快照和日志保留。精确九文件清单及按文件恢复命令位于
相邻 `store-hub-root-20261111-PARTIAL-ARCHIVED.txt` 和 partial-retirement JSON。
本轮归档清理后 VM 可用约 649 MiB；未动原始脏 checkout 或冻结输入。
