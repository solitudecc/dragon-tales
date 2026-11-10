# 同一函数 CFF 的未知调用屏障回归

基线 `9020c70`。这是当前行为的永久验证组，没有扩大外部函数、syscall 或跨调用证明范围。
它使用同一函数内的状态循环 `10 → CALL → 20 → 30 → return`，不读取 Guest 入口 PC/RIP，
不使用 callee-entry selector，也不依赖 `CallEffectSummary`。未知 writer 的函数体在优化时
不可见，只在原生对照时单独编译并链接。

回归覆盖以下边界：

| 状态位置与写入时机 | 期望 |
|---|---|
| CALL 后重新写 Guest 槽或 Guest 栈 state | 最后的真实 STORE 重新建立 state，3 条 dispatcher 入边可以直连；CALL 保留 |
| CALL 前写 Guest 槽、Guest 栈或寄存器快照 state | writer 可能修改 state，CALL 后的边保留原 dispatcher；独立的初始和第二块写入仍可直连 |
| 未逃逸 host alloca 保存 state | mem2reg 可以提升为 SSA，未知 CALL 不能凭空获得局部指针；可以去平坦化 |
| 把 host alloca 指针传入未知 CALL | state 已逃逸，CALL 后的边必须保留可能被改写后的读取与 dispatcher |

Guest 栈是 Guest 地址空间中的字节，并不因为源码声明为局部变量就成为 LLVM host alloca。
未知 CALL 会清空 Guest-memory SSA facts；只有 CALL 后实际重新赋值才能恢复对应关系。
寄存器 SSA 在真实调用边界需保留 snapshot 写回/重载；本组通过字节 16 的普通寄存器快照
检查旧值不会穿过 writer，故意避开 PC/RIP 的 128 字节偏移。

初始回归在 `9020c70` 上包含 3 cases/127 assertions，已经通过。后续同函数逐边去平坦化
把未知 CALL 情况的直连数从 0 提高到 2，并单独断言 CALL 后那条未知边仍经过 dispatcher。
这些断言与混合前驱回归合计在前一版有 27 条失败；未知调用本身的语义约束保持不变。为了证明对照能发现
问题，导出器额外产生一个故意错误的 readonly 模块，原生 writer 保持相同：它使 24 次
对照中的 16 次检测到 stale-state 差异，另外 8 次输入本来就写入相同 state，仍匹配。

独立原生脚本比较返回值、CALL 次数、全部 512 字节寄存器快照和全部 64 KiB Guest 内存。
7 类夹具 × before/after × O0/O2 × 12 个边界输入，共 336 次正常对照全部匹配；28 个模块
通过 LLVM verify。没有排除、跳过或归一化。这是 LLVM/CFF/调用别名边界验证，不代替
机器指令 lifter 的 Unicorn 对拍或任意程序的全函数证明。

运行：

```sh
DRAGON_CALL_STATE_FIXTURE_DIR=/tmp/call-state-fixtures \
  ./build/bin/dragon_tests '[call-state-barriers]'
python3 scripts/validation/call_state_barriers_diff.py \
  --fixtures /tmp/call-state-fixtures --out /tmp/call-state-native
python3 scripts/validation/call_state_barriers_diff.py \
  --fixtures /tmp/call-state-fixtures --out /tmp/call-state-control \
  --control false-readonly --expect-control
```

本轮验证只编译新回归文件并链接既有基线库，没有并行重建生产库，也没有修改 VM overlap
工作树。结果和完整 before/after IR、native/expected 快照保存在
`/home/cc/vmc-runs/call-state-barriers-20261111/`。
