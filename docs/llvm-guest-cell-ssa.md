# Guest cell 的最近 STORE SSA 转发

这是供 C++ custom pass 显式调用的低级组件。installed header 为
`<dragon/lifters/llvm/GuestCellSSA.hpp>`；为保持原型的类型布局和符号名称，接口继续位于
`dragon::llvmlifter::detail`。它只处理调用者选定的一条原始 LOAD，不扫描模块，也不改变
默认 `guest-memory` 或六步优化流水线。没有新增 Python 高层 pass、PassPipeline builtin、
C shim 或 LLVMLifter 成员方法。

```cpp
#include <dragon/lifters/llvm/GuestCellSSA.hpp>

// function 是有效 IR；guestBase 是原 Guest 指针 SSA；originalLoad 是调用者
// 在首次改写前保存的原始 LOAD，且当前仍属于 function。
auto stats = dragon::llvmlifter::detail::forwardGuestCellLoad(
    function, guestBase, originalLoad);
if (stats.changed) {
  // originalLoad 已被替换并删除，不再访问其引用/指针。
}
```

调用者负责候选的原始身份、存活检查和外层改写次数预算。新生成或克隆的 LOAD 不作为
候选。一次调用最多消除这一条 LOAD；本组件不提高其他 pass 的 `max_rewrites`。

## 证明边界

支持 simple 整数 `i8/i16/i32/i64/i128` LOAD/STORE，允许 alignment 1。地址必须由指定的
同一 Guest base SSA 经常量 GEP 链得到；DataLayout 的 pointer/index 都必须为 64 位，
且指针必须是 integral pointer。偏移按 modular i64 计算，最多枚举 16 个字节判断重叠，
不把主机对齐、另一 base SSA、指针 PHI 或动态偏移当作已知地址。

从 LOAD 前的位置逆扫本块：遇到同一 base、同一常量偏移、同一字节宽度的最近 simple
STORE，就保留并使用其原 SSA value，停止这条路径。该 STORE 之前的未知写和调用不再
影响这个最近定义。已知不重叠的整数 STORE 可以跨过；可能 alias、partial overlap、
未知地址/宽度写、volatile/atomic 操作、fence、EH pad 和任何调用均为 barrier。
特别是 STORE 后的调用即使没有摘要，也不能假定它保持该 cell 不变。
直接 undef/poison 的 STORE value 保守拒绝。

到块头后递归覆盖全部实际 predecessor successor slots，包含重复边和不可达前驱。
入口无定义或在遇到 STORE 前回访灰色节点，整次拒绝，不猜初值、不生成 partial PHI。
允许普通 CFG 有循环：只要每条回溯路径的最近 STORE 切断内存定义图即可。LOAD-prefix
与同块 block-end 是不同节点，不能把本轮 LOAD 和回边之前的 STORE 混成同一个位置。

STORE value 可以是常量、原 PHI，或依赖旧 LOAD 的原算术表达式。组件不递归计算这些
整数值，不证明范围、definedness、table 内容或完整状态集合。成功替换后，原来的
`next = load + 1` 等依赖可自然形成合法的 loop-carried PHI；原算术与 flags、调用、
STORE、业务分支和其他 LOAD 都保留。不添加 noalias、immutable-memory 或 assume。

先完成纯计划图、每条 incoming edge 的 dominance 检查，以及全部 PHI/提交工作预付，
然后按函数 IR 顺序创建 PHI 并替换 LOAD。任何拒绝，包括晚到的前驱、dominance 或预算
失败，都不修改 Module；Function/Argument identity 和 CFG 保持不变。

## 预算与统计

`GuestCellSSAOptions` 的默认值也是硬上限；调用者只能下调，超出上限的请求会截到上限。

| 字段 | 默认值／硬上限 | 含义 |
|---|---:|---|
| `maxBlocks` | 128 | 实际访问的定义图 blocks |
| `maxNodes` | 256 | 含独立 LOAD-prefix/block-end 的节点 |
| `maxPhis` | 128 | 计划所需 PHI，提交前全部预留 |
| `maxWork` | 16384 | CFG slots、逆扫、GEP/重叠、节点/边和提交预留的记账 |

这些 work 单位不是 LLVM 内部 CPU 时间上界。`GuestCellSSAStats` 报告
`changed/refused/budgetExhausted`、`blocks/nodes/phis/work` 和 `message`。
`phis` 是预留的计划数，晚期拒绝时可非零，但此时没有创建 IR。
没有一般内存固定点，也不会因为失败自动放宽 barrier 或预算。

## 已有验证与限制

原型的 14 个 C++ 用例已在实际 main32c backend 的独立 client 上通过，共 261 assertions。
覆盖多块/多回边循环、不同最近 SSA 值、重复与不可达前驱、STORE-free 灰环、最近定义
截断、i128/未对齐/mod64 偏移、alias/partial/call/atomic/volatile/fence 拒绝、晚期失败与
预算耗尽的完整 IR 字节不变。此次正式接入只改测试 include；14 例正文和断言逐字节保留。
旧库没有这个低级 API，不能编译相同测试；这不构成旧库返回错误执行结果的证据。
正式集成后的两实际库也各通过同一 14 例／261 assertions，客户端不链接 helper 对象。
历史缓存 CTest 分别通过 1163/1163 和 1028/1028，新 14 例另计。
正式库 after-only native 新增 384 次完整 arena 匹配、128 次指定实际故障检出；
原有 384 次 before 校准仅通过精确绑定引用，不重复计数。
构建、补充回归、输入域和证据封存状态见
[正式接入报告](guest-cell-ssa-integration-20261114.md)。

新的独立 native 域为 6 个 source fixtures：word32/128 各有普通循环、调用位于最近 STORE
之前、调用位于 STORE 之后并实际 clobber cell 三种形态。每个 fixture 固定 16 个边界输入，
共有 96 个 fixture-input、32 份不同初始 arena；每宽 16 个不同 cell word。O0/O2 是重复
验证这同一输入域，不增加业务样本数。每个循环至少三个 blocks、两条回边，实际 SSA
分别执行 `old+1` 和 `old-(-1)`，调用/counter/STORE 都真实执行。

先完成两档 before 校准：192 次独立 C/Python 完整匹配和 192 次原始 LLVM/Python 完整
匹配，才执行 after；after 再有 192 次 LLVM/Python 完整匹配。合计 576 次均逐字节比较
完整 66112-byte arena，其中 positive fixture 384 次、refusal fixture 192 次；后者的
完整 before/after IR 字节相同。7488 个快照错误对照单独计数。

另有 64 次实际 native semantic-fault 观测：故意把一个新 PHI 的回边接成初始值，64 次
全部被模型检出，cell/fresh 输出各有 64 次差异，原 historical SSA 输出有 48 次差异。
这些是额外故障观测，不是 normal matches 或快照 comparator flips。六份保存流的审计
均核对完整域、EOF、源 hash 和全 arena；没有 skip、排除或 normalization。
这是独立数学/C/LLVM synthetic 验证，不重计旧 predicate campaign，也不代表 machine
lifter、no-Z3 native、全 corpus 或全 CFF 去平坦化验证。

实际 C++ caller 已在真实 manual O2 的一个所选状态 LOAD 上得到 PHI 替换。该 caller 的
候选 CFG 闭合证明属于独立步骤；本 helper 不证明 dispatcher 的目标全集，不负责
decode-through-PHI 或删除剩余业务 switch。这一个 cell 的 LOAD→PHI 不是 full CFF。

native 证据包：`guest-cell-ssa-native-20261114-v1.tar.xz`，SHA256
`d380f02809a1d93916be05dd7ff89f957caabd712a29c1bef0b3971a4feecf3b`。
VM 位于 `/home/cc/vmc-runs/`，Mac 位于
`/Users/c/Desktop/deob/validation-artifacts/guest-cell-ssa-native-20261114/`；
两端逐成员集合、大小和 hash 已核对。包内保留全部源、输入/IR、实际 target、完整保存流、
命令、loader maps、source/runtime pins 与审计结果；main32c backend 只通过 hash 引用。
