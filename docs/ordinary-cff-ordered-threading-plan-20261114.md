# 普通 CFF：保序选边线程化的窄方案

状态（2026-11-14 更新）：私有 v6 聚焦14例／596断言已通过；独立 synthetic native 与
真实 manual O2 的静态结构门槛、限定16输入的完整状态复验均已通过。下面保留原设计和
失败阶段，不把历史“尚待执行”记录当作当前状态。三个低级 C++ 组件正在接入 installed
headers/CMake；正式库聚焦与完整测试仍待执行，默认流水线不变。

## 原型验证记录

原 v1（CPP `73dc2d99…`）已完整保留。静审发现其状态值证明把历史 PHI 展开为未来
incoming；事务还包含空 internal declaration 和 named metadata 回滚问题。
原 v1 没有成功编译或执行，不能声称它在 native 返回了错误的21。
其 LLVM21 `WeakTrackingVH` 的 `auto *` 推导还不能编译；另保存仅机械类型兼容修正的
版本，以及合法事务配合旧状态证明的有意错误 control，版本与证据分别记录。

当前核心 CPP `98be857a…` 区分逐 Use 的当前／历史执行，历史 numeric PHI 拒绝，
AND1/i1 的全域仍可使用。临时 carrier、完整 calling convention/attrs、named metadata
回滚、foreign BlockAddress 和无名函数也有对应保护及回归。

首次14例聚焦执行因测试查找已被 LLVM 清理的全常量 PHI 而 SIGABRT；保留全部证据。
随后只加 null-safe 诊断，保留原断言：真实14例中13通过／1失败，共574断言中573通过。
raw IR 明确原退化用例已变成 `ret i32 7`。原始常量用例继续作为返回结果兼容检查；
另加非恒定、非 default 的重复目标边，保留精确4 incoming 期待并增加值与实际 CFG
slot 检查（test `81ca58b8…`）。v6 随后实跑14例／596断言全部通过；先前失败完整保留。

独立 synthetic native 的 before C128与LLVM128均按完整66112字节校准；after128也全部
匹配，fault64含specified64，4992 comparator controls 全部检出，6 streams／448 frames
已审计（`~/vmc-runs/ordered-dispatch-corridor-native-20261114-v1/SUMMARY.json`，
SHA256 `437bacb6ca519c9603e9d9c89cd2ceeb72441b5d8bc1d7eb6f02029a5fe648e4`）。
这不是正式 installed library 的验证。

真实 manual O2 的新显式 recipe 在旧失败前9阶段的18个 canonical/bound 文件逐字节
门槛通过后，处理8／8原回流边；19阶段均 verify，核心紧后与最终循环分发 switch 都为0。
原16输入 before32＋after32的LLVM执行逐字节匹配原参考完整66136字节／76字段，含
Guest65536、Exit8、root RET64；192 comparator controls 全检出。新 C0／Unicorn0，
复用的旧参考只计一次（`~/vmc-runs/ordered-dispatch-corridor-real-native-20261114-v1/SUMMARY.json`，
SHA256 `6dc084ebd0a988d20a7f80051c2617f501098bb5b55dc60565a7fbc1e9d0d93d`）。
成功限定于 checked 分析视图和已测 nominal 域；真实 guard miss／public GuestSize 异常
尚未机器实测，不宣称普通 root 无条件净化或任意混淆通用完成。

## 已实测的边界

manual O2 的 checked-context、最近 state STORE SSA、纯 scalar decoder 已完成私有验证。
两个新 scoped recipe 分别在 all-use decoder 后加入 CVP/InstCombine/GVN/SimplifyCFG，
或标准 LLVM21 default<O2>，随后再跑现有 control/specialize/deflatten。
每个17阶段都通过 Module verify、ordinary/source exact 检查；均仍有1个循环 switch，
hard gate=false，0native、0full CTest。两 recipe 前8阶段共32个 canonical/bound IR 文件
与先前16阶段失败 recipe 字节完全一致。没有增加预算或 assume/noalias。

O2 改名/移动了中心 switch，使旧名称 observer 的 center=true；strict=false 仍使总门槛
失败，不能据此说分发器已经退出。真实 switch 在 `bb_4787`，selector PHI 在
`memory.valid107`，还在同一业务 SCC。

证据：`~/vmc-runs/dispatch-decoder-standard-recipes-20261114-v1/`，AUDIT SHA256
`b063f8ccf9c237b5d28b2d5f8ae46f4a57d6e5bd8b47d11ea568a77c6703f755`。
完整逐指令/逐 liveout 清单在本地
`validation-artifacts/dispatch-decoder-standard-recipes-20261114-v1-diagnosis/`。

## 当前 8 条原始候选边

依据实际 O2 IR 的完整 selector PHI 图，不依据名字、预测 metadata 或样本输出。
下面各边的原 successor slot 都是0。state6 会沿保留的 decoded>5 路径退出，不能直跳业务块。

| source | corridor root | 该原边的完整状态域 |
|---|---|---|
| bb_4794 | bb_4752.thread | 6 |
| bb_5002 | bb_4752.thread | 5 |
| memory.valid145 | bb_4752.thread | 2 / 3 |
| memory.valid115 | bb_4752.thread | 4 |
| memory.valid111 | bb_4752.thread | 4 |
| memory.valid127 | bb_4752.thread | 1 |
| memory.valid103 | bb_4752 | 1 |
| memory.valid125 | bb_4752 | 0 |

每个 root 到最终 switch 都是5块：
`root → memory.valid105 → bb_4765 → memory.valid107 → bb_4787`。
thread 组80条原指令；plain 组81条。当前全部8条若逐边采用独立副本，原指令复制上界
642条/40块，尚未计 snapshots 和 SSA repair；这不是已执行改写计数。
初始 state0 的原 switch 可以保留，但所有业务回流边均处理后它应不再处于业务循环。
任何一个候选拒绝都不能宣称完整去平坦化。

## 每条原路径都要保留的顺序和出口

1. plain root 的实际 Guest-size 检查；失败到 `memory.invalid106`。thread root 无此分支。
2. `memory.valid105` 的真实 `store i32 %ECX.089, ptr %896, align 1`，然后原 counter
   decrement/check；终止到 `bb_5420`。
3. `bb_4765` 的真实 GuestSize `load i64, ptr %2323, align 1` 和 bounds check；失败到
   `memory.invalid108.loopexit`。LOAD 每次仍在相同执行位置，不能复用旧一次读取或当循环常量。
4. `memory.valid107` 的 decoded>5；退出到 `memory.valid147`。
5. 仅到原 switch 位置才消费**原 selector**，改成已证明的直连目标或一个二选一条件。

两组走廊内都只有上面一个 STORE 和一个 LOAD，无 call、volatile、atomic、fence。
flags、snapshot、输出、错误处理在原位置保留；不移动到路径前方，不补造定义。
末尾原 switch/default/error 也属于计划：无完备值证明时保留原 switch，不把未知改成退出。

## SSA 修复与有限调度

5块整体的 outside liveout：thread 组41值；plain 组55值。因为旧 header 值可代表
**前一执行**，不能只修“区域外”uses。按 expanded corridor 的跨块修复范围计，分别为
71值/429uses、70值/440uses。完整记录见 `thread-candidates.json`，SHA256
`bb988901f2128d0434ae2dbf721536e89a27ffd539bcbee4598b6a6b13e37da6`。

优先复用/抽取 `lib/GuestMemoryEdges.cpp` 的机制：

- `preflightRepair`（约565行）预付 liveout/use/repair-cell 成本。
- `cloneEdge`（约674行）中的历史 EntryInputs snapshot、CloneBasicBlock/RemapInstruction、
  每实际 successor slot 的出口 PHI，以及约761行的 SSAUpdater。
- 原头部 PHI 的选中 incoming 必须先绑定为该次执行的历史输入。不能把旧
  `memory.valid107` 的寄存器 PHI 通过 remap 误接到稍后才执行的新 header 定义。
- SSAUpdater 同时修复旧值/副本值的全部普通 use 和 PHI incoming edge；不能只用
  verifier 当“没有 stale value”的证明。无完整到达定义、EH/blockaddress/特殊身份则拒绝。

这不是直接放宽原 Guest-memory pass：其 `safeCorridorBlock` 拒 STORE，`cloneEdge`
绑定 selected LOAD→STORE forwarding；应由独立的 scalar-state proof provider 使用
抽取的保序复制/SSA 修复内核，原调用者合同不变。Entry-prefix 的 direct-unique-exit
bridge 修复（`LLVMEntryPrefix.cpp:223-268,347-368`）不足以覆盖这里的多块339+ uses。

沿用既有硬上限而不增加：6块/256原指令、512原候选/512 incoming、64副本块、
16384复制指令、128liveout、8192uses、262144repair cells；所有候选共享各项预算。
snapshot、repair PHI、必要桥块也必须计费，不能只算642条原指令。
首轮冻结所有原 block/terminator/successor-slot/target 身份，按原 Function IR 顺序调度，
每条原边最多成功一次；副本不是候选，不靠无限重试获得不动点。每次仍需复验当前边和
原 SSA 证据，失败保留原边且报告 incomplete。

## Poison / undef：在原 switch 位置消费

AND1 只限定 defined 执行的数值域，不证明原 atom 非 poison/undef。实际 atom 来自普通
Guest LOAD 和带 nsw 的计数表达式；当前没有可直接使用的 noundef/freeze 证据。
因此不把动态 atom 提前变成分支。state-leaves→DFA 的设计先停止，未称可用。

保序方案的二值情形在**原 switch 位置**使用一次原 selector，如比较其是否为2，再去2/3
对应业务块。只有完整、同宽、保 flags 的有限域证明才允许此二分；不另读 Guest，不读取
早先 atom，不引入 freeze/assume。原 counter/fault/bounds 退出先执行，新的 poison 控制
使用不会越过这些原操作。单值无条件跳转也须是该边上真正已定义的常量；从可能 poison
的相等 select arms 得到“singleton”不够，否则保留原 selector 消费/原 switch。

回归必须包括：多块 liveout 与历史 header 回流、重复 successor slots、未知路径、
counter/GuestSize 错误路径、晚 STORE→LOAD alias、未证明 call、预算拒绝全不变；
poison/undef 结构控制不原生执行。独立 native 对照需比较完整寄存器/flags/Guest/Exit，
含错误路径和错场景检测，再以完整 SCC 门槛决定是否真的完成。

## 现有组件拒绝的实际原因

`LLVMDeflattening.cpp:412,416-419` 要求无 escaping definitions、state PHI 与 switch 同块且
单use。实际两个块分离且 bounds 同时使用 state，故默认组件报告 unresolved，budget0。
`LLVMLifter.cpp:1307-1316` 已显式跑 DFAJumpThreading，并非缺 pass。

LLVM21.1.8 的 MainSwitch 回溯在同 loop 内遇到非 PHI/select/constant 叶会触发默认
early-exit；当前唯一动态叶是 `xor (and ...,1),3`。这是源码与实际 IR 的对应，尚无独立
remarks 计数，不声称成本阈值实测失败；其默认20/2500/200/50阈值保持。
参见 [LLVM21 DFA 源](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.8/llvm/lib/Transforms/Scalar/DFAJumpThreading.cpp)。

## 历史受控版本（记录保留）

本地 `validation-artifacts/ordered-dispatch-corridor-prototype-20261114/` 保留每版源码和
SHA manifest。当前候选 `source-v3` CPP SHA256
`b1b4c3a601bf1d15bb2274f53ce4c510b69b3bca0f0580945354e95e3c02f3ae`；
test SHA256 `4fb004dac71a24e736e42e8a4dab744fbebc156a0a9ee2d388e349cb2f22cc6a`。
`DRAGON_DISPATCH_CORRIDOR_TESTING` 宏下14cases，含两阶段强制 commit 失败的 named metadata、
原 block/instruction/Function/Argument 身份不变检查。这里列的是当时尚未运行的 source-v3；
当前通过的是后续 core `98be857a…`／test `81ca58b8…`，不能混用版本标签。

`source-control-tx-v1-state-v3` 保留原 v1 StateValues 的精确类文本，只使用修正后的事务与
身份保护，故它是**有意保留错误值模型的验证对照**，不是旧产品或可发布实现。CPP SHA256
`b97eab8fd3e016fca877dde7ee8c6d7c6624c4eec70ed596c9def404c594817e`。
未来在明确窗口用同一 API/fixtures 证明历史10、未来20导致的错误预测可以被测试捕捉；
最终版本应对该历史 numeric 依赖完整拒绝，原 Module 字节不变。后续 controlled 版本
`14b51551…` 用同一最新 test.o 执行 history-proof2例／38断言，失败1例／6断言；
这是有意保留错误值模型的对照，不能称旧生产实现曾 native 返回错误21。
