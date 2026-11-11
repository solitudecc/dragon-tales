# Guarded entry 后首个 CFF 分发仍动态：只读诊断

日期：2026-11-11。范围：已完成 `sp` 配方的 f-cff O0 保存 IR。本记录没有修改实现、增加预算或执行新的测试；它说明剩余精度缺口，不是新的语义失败报告。

## 证据身份

本地证据目录：`/tmp/dragon-guarded-entry-real-evidence/sp/guarded-views/f-cff/O0/`。对应真实验证 namespace 为 `~/vmc-runs/guarded-entry-real-20261111/candidate-v1-sp`；展开目录可能在完整归档验证后退役，长期恢复位置以本阶段根验证报告为准。

| 文件/来源 | SHA-256 |
|---|---|
| `after-raw.ll`（2,283,387 字节，本记录所有 IR 行号） | `0b5eb62a2f3075cff7827546be98e1ee0cd202f9e55204af1a763c5378349724` |
| `before-raw.ll` | `829a4b155248296e92fa2084ecabe4ae5ed0d9855d4d39f1f72b82c3947af08e` |
| `manifest.json` | `2e298aad1895828728f786302a20741571cfa549ca5c738ad239bddf3a6d344f` |
| manifest 记录的源模块 bitcode 摘要 | `94fdea5761c2cd82f7e353196808a7c52981babd75c6e378b195e48983335d06` |
| manifest provenance 记录的原 ELF | `20948e657826406c23c9945ce1b7e74605a5b033349daf3c495273a24aa537fe` |
| 诊断时 `lib/StateGuardEdges.cpp` | `ba49162cdb226d5ad6765055b00d519eb8a515736652c630f3507a3aca869a1b` |
| 诊断时 `lib/LLVMGuardedEntry.cpp` | `71c81a51659b1947fa0631d8b0cfdff7141181ffa555909b7eb1623fad981114` |

后两项是本地代码读取的内容摘要；bitcode/ELF 摘要取自已保存 manifest，不能冒充本轮重新生成或重新执行的证明。

分析函数是 `dragon_static_entry_x86_64_102a0.guarded.real_sp`，普通 root 仍保留。上下文要求寄存器字节 offset 32 为 `f8c7000000000000`（RSP = 51192 = 0xc7f8），并运行时核验 Guest `[69632,70040)` 的 408 字节。最小 regs/Guest 大小分别为 40/70040。这些是成功边已检查的事实，不是全函数内存不变性。

## 首个分发的实际阻塞

入口行 40–41 检查 PC = 66208（0x102a0），不匹配走原 `entry.unsupported`。`guarded.bounds` 行 8427–8439 检查描述器范围、Guest base 非空和 RSP = 51192。最后一项表检查位于行 8745–8749：

```llvm
%2582 = getelementptr i8, ptr %2, i64 70032
%2583 = load i64, ptr %2582, align 1
%2584 = icmp eq i64 %2583, 67010
br i1 %2584, label %bb_66208, label %guarded.fallback
```

成功边进入 `bb_66208` 后，首次沿合法检查边到实际分发的路径恰为以下九个块（不把前面的 `guarded.next50` 算入九块）：

```text
guarded.next50
  -> bb_66208
  -> memory.valid
  -> memory.valid1
  -> memory.valid3
  -> bb_66234.guest.edge
  -> memory.valid7.guest.edge
  -> bb_66252.state.context
  -> memory.valid11.state.context
  -> memory.valid13.state.context
```

该前缀没有 `call` 或 `invoke`。各检查失败边仍转往原 `memory.invalid*`。末块行 8888–8889 保留：

```llvm
%2649 = load i64, ptr %2582, align 1
switch i64 %2649, label %jmp.unresolved [ ... ]
```

这是真实 selector LOAD，与最后 guard 使用**同一个指针 SSA `%2582`**；state 50 和表下标 70032 已知，但 LOAD 的内容没有传播为 67010。不能把第一个分发的阻塞归因于外部调用。

中间的 STORE 如下。第三列是当前 IR；第四列仅在 `guarded.next50 -> bb_66208` 此次进入上下文中成立。

| 行号/块 | STORE | 当前 Guest index | 入口上下文中的 index/字节范围 |
|---|---|---|---|
| 1657 `memory.valid` | i64 `%RBP.0` | `%35 = %RSP.0 - 8` | 51184，`[51184,51192)` |
| 1726 `memory.valid1` | i32 `%EDI.0` | `%372 = %RSP.0 - 12` | 51180，`[51180,51184)` |
| 1768 `memory.valid3` | i64 `%RSI.0` | `%385 = %RSP.0 - 24` | 51168，`[51168,51176)` |
| 8753 `bb_66234.guest.edge` | i64 50 | `%398 = %RSP.0 - 128` | 51064，`[51064,51072)` |
| 8875 `bb_66252.state.context` | i64 50 | `%2591 = %RSP.0 - 136` | 51056，`[51056,51064)` |

当前 `%RSP.0` 不是全块常量。上述动态 Guest 写入不能与表 slot `[70032,70040)` 证明不相交，因此复用早先 `%2583` 会不安全。在限定的首次入口上下文里，这五个范围确实均与表分离；保留该上下文才有机会让现有 LLVM 内存优化证明它。

## 二十条入口 PHI 入边

`bb_66208` 行 43 的 predecessor 列表有 20 项；行 56 的 `%RSP.0` 有以下 20 个 incoming。只有 guarded 成功边直接携带 51192，不能把它 RAUW 到整个原块。

| incoming block | RSP incoming value |
|---|---|
| `memory.valid13` | `%state.edge.live546` |
| `memory.valid261` | `%state.edge.live544` |
| `memory.valid13.state.context` | `%350` |
| `memory.valid13.state.context1085` | `%1997` |
| `memory.valid13.state.context1094` | `%1728` |
| `memory.valid13.state.context1103` | `%1124` |
| `memory.valid13.state.context1112` | `%974` |
| `memory.valid13.state.context1121` | `%825` |
| `memory.valid13.state.context1130` | `%state.edge.live547` |
| `memory.valid13.state.context1139` | `%state.edge.live543` |
| `guarded.next50` | `51192` |
| `memory.valid13.state.context1244` | `%state.edge.live548` |
| `memory.valid13.state.context1452` | `%state.edge.live553` |
| `memory.valid13.state.context1462` | `%state.edge.live572` |
| `memory.valid13.state.context1472` | `%state.edge.live574` |
| `memory.valid13.state.context1482` | `%state.edge.live575` |
| `memory.valid13.state.context1492` | `%state.edge.live577` |
| `memory.valid13.state.context1502` | `%state.edge.live578` |
| `memory.valid13.state.context1512` | `%state.edge.live580` |
| `memory.valid13.state.context1522` | `%state.edge.live582` |

行 44 的 `%34` 同样合并 20 路 Guest size，guarded 入边为 `%2425`。首检查行 61–66 等价于 `size >= 8 && RSP-8 <= size-8`。已有 guard 只给 `%2425 >= 70040`，没有给确切常量。

后续行 1683、1728、1770、8754、8876 重新读取 Guest descriptor size。Guest 与描述器没有自动 noalias；某次 Guest 写入可能影响描述器，不能把这些 fresh LOAD 全换成入口值，或把后续 bounds 一并删掉。

## 为什么现有专门化没有自动处理

`StateGuardEdges.cpp` 的当前合同解释了这里的保守结果：

- `ConditionProof::evaluateInstruction` 的 ICmp 分支（约 208–230 行）需要两边都求出常量；没有使用支配边上的数值范围事实。根 bounds 中的 size 因此仍 unknown。
- `specializeStateGuardEdge`（约 624–635 行）要求整条分支条件已证明为 i1 常量且使用 incoming PHI，才开始复制；仅 RSP incoming 常量不足以启动。
- `contextPath` 可以保留后续未知条件，但首条件必须先证明。上限仍为六块/256 条指令；当前九块路径不能一次完整复制。遇到过长路径时，现实现回到原单块形态。
- `usefulContext`（约 407–435 行）只把下游 PHI 获得常量作为扩展收益，没有将 Guest STORE 地址变为可证明常量视为收益。
- 普通 control-flow 配方已经含 SCCP、DFAJumpThreading、JumpThreading、SimplifyCFG、InstCombine 和 DCE；“再加入 jump threading”不是针对这一缺口的方案。

这不是本轮动态量测得到的某个预算拒绝计数。九块与六块限制是代码/IR 静态对照；不得据此声称所有剩余分发都因预算失败。只添加范围证明也不能保证获得整个九块前缀的收益。

## 下一步最小通用组件建议

候选组件是**有界的选中入边值上下文专门化**，或语义等价的首次入口前缀展开。以已经存在的某条原 incoming edge 及其 PHI 值为证据，隔离一次进入的前缀；未知条件保留为真实运行时分支，不要求先把整个条件证明为常量。

首个可验收能力应是：在限定复制前缀中，RSP 使用该原边的 51192，至少一个实际 Guest STORE 地址变为常量且保持 STORE 本身；原机器入口及其余十九条入边保持动态。随后用已有 GVN/GuestSSA 的失效规则处理真实 table LOAD，只有跨过的全部写入均已证明不别名时，才能消费之前的 guard 事实。

必须独立设计并验证以下边界，而不是直接把当前条件判定放宽：

1. 只消费冻结原边/原块上下文；机器回入、内部递归和未知路径继续普通代码，不重播首次入口事实。不能把生成的克隆无限作为新种子。
2. 每次重新构造 dominance/PHI 映射；历史输入与 fresh LOAD/freeze 分开。保留 STORE 顺序、未知 bounds、错误出口及跨块 SSA/live-out。
3. 共享既有模块预算，保留旧 direct/hub/pointer 优先级。不增加外部三轮次数或六块上限；若做分段前缀，必须先证明有界身份与进度、处理截断处 PHI，不能先承诺九块一定完成。
4. 对未知调用、可能别名写入、描述器修改继续失效。整循环 SP 不变、整表 immutable、Guest 与寄存器/描述器不别名均不是可添加的前提。

预期回归应包括：首次边常量而循环回边不同；首 bounds 未知并可失败；STORE 会覆盖表的负例；Guest 写入修改 descriptor 的负例；fresh LOAD/freeze 和外部 live-out；截断/预算及新克隆不重复入队。原生验证应比较完整状态并有指定错误控制。真实精度门槛先明确区分“栈 STORE 地址具体化”和“真正 selector LOAD/switch 被消除”，不统计 guard 自身 LOAD 的消失，也不承诺所有剩余分发同时消除。

本记录没有运行上述新验证，也没有实现该建议。已完成 SP 真实矩阵的执行/故障控制统计由本阶段独立验证报告承担；此处只提供保存 IR 的可定位解释。
