# 同一函数去平坦化：逐边恢复与安全修补

2026-11-11，起点 `9020c70`。本轮聚焦同一机器函数中的 state 写入、分发和未知 CALL，
不扩展 VM、特殊堆保护或自动跨函数入口专门化。真实 Tigress 定位另见
[冻结样本记录](same-function-cff-validation-20261111.md)。

## 生产变化

1. 逐条验证 dispatcher 入边。未知值、未知 CALL 后的 state 或不支持的前驱继续经过原
   dispatcher；其它有完整证明的边可以独立直连。某个 dispatcher 因而可以同时计入
   `dispatchers_rewritten` 和 `unresolved_dispatchers`，不把部分恢复描述为全部完成。
2. 拒绝绕过带 live-out 业务 PHI 的 dispatcher；否则删入边时 LLVM 可能把业务值替换为
   另一条路径的值，模块仍通过 verify，但结果错误。
3. 保留 volatile/atomic selector LOAD，并拒绝改写 `indirectbr` 等 computed-address
   前驱。改目的列表不能同时更新实际跳转地址。
4. 移除通用 Pass 中实验性的跨调用 fallback。正确的调用写后摘要不是 callee 入口事实；
   不同嵌套指针和 callee 内的覆盖写也不能被忽略。兼容选项保留，显式 true 也不再启用
   原路径；三个 `cross_call_*` 计数为零。GuestMemorySSA 的正常返回后写效果消费保留。

普通 CALL 本身、CALL 次数、正常返回核验和未知边均保留。CALL 后实际重写 state 可建立
新的关系；CALL 前可能被修改的 state 不会被当成不变。未逃逸 host alloca 和 Guest 栈
不是同一类对象，不能因为源码里都是局部变量就采用相同的无别名结论。

## 失败控制和独立语义对照

| 检查 | 旧实现 | 修补后 |
|---|---|---|
| PHI live-out / observable LOAD / indirectbr 三项回归 | 3 cases，18 条断言失败 | 3 cases，32 条断言通过 |
| 已知与未知入边混合及 CALL 结构回归 | 4 cases，27 条断言失败 | 已知边直连；未知 CALL 后那条边仍进 dispatcher |
| 真实 pass 的业务 PHI 原生对照 | 40 次中，优化后 2 个错误返回 | 四夹具 O0/O2 前后 160 次对照，零差异 |
| 原生比较器单字段负对照 | — | 160 次控制比较，4/4 指定错误被检出 |
| 同函数 CALL 与 Guest 内存对照 | — | 7 夹具、28 模块、336 次全部匹配 |
| 故意错误 readonly 效果控制 | — | 24 次中 16 个错误检出、8 次预期匹配 |
| 跨调用 postcondition / 指针对象 / 内部覆盖写隔离 | 3 cases，18 条断言失败；24 次原生对照中 after 有 8 个错误 | 保留所有对应 dispatcher，不采用旧入口推断 |

最终主构建 CTest **867/867**、无 Z3 CTest **732/732**。两种构建的分发/调用聚焦组均为
**20 cases / 403 assertions**；主构建 GuestMemorySSA 为 **13 cases / 112 assertions**。
最终隔离版的三个跨调用契约原生对照共 **24/24** 匹配，原来 8 个错误均已消失。

CALL 对照逐次比较返回值、CALL 次数、512 字节寄存器和完整 64 KiB 内存。隔离对照的
摘要是真实合法的正常返回效果，不是故意注入的假摘要。所有原生检查保留输入、输出、IR、
编译命令和运行库哈希；没有新增排除、跳过或归一化。

永久入口：

- `tests/lifters/TestLLVMSameFunctionDeflattening.cpp`
- `tests/lifters/TestLLVMCallStateBarriers.cpp`
- `tests/lifters/TestLLVMCrossCallIsolation.cpp`
- `scripts/validation/same_function_deflat_diff.py`
- `scripts/validation/call_state_barriers_diff.py`
- `scripts/validation/cross_call_contract_native.md`

## 真实 CFF 的范围

四个冻结 ELF 的 `liftStaticRegion` 重放已得到 C 16/16、LLVM/Unicorn 完整根返回 16/16，
RAX 和内存单字段负对照分别检出 16/16。每次比较 76 个模型字段、1 MiB 内存和 7 个
Exit 字段。原 Graph、输入和运行库均通过前后检查。

每个 kernel 仍保留两个真实表目标 switch、实际表 LOAD、访问检查和未知目标 default。
被移除的 RIP entry switch 不算去平坦化。这些回放证明执行覆盖和受测状态匹配，不能据此
宣称任意 table[state]、条件状态更新、复杂循环或特殊混淆已自动消除。

本轮没有修改机器指令 lifter、Graph 分析或默认普通提升，也没有重跑 69 输入的全语料
活动和单指令活动。验证针对受影响的显式 LLVM Pass、未知 CALL 以及四个冻结真实内核；
全 CTest 与无 Z3 结果如上，不将历史语料统计计入本轮新覆盖。

最终测试日志、原生输入输出及库/源码哈希保存在本地
`/Users/c/Desktop/deob/validation-artifacts/same-function-dispatch-20261111/`。
真实 kernel 最终重放在相邻的 `cff-real-region-final-20261111/` 目录。最终主库 SHA256：
`a33dc8c9c1b79125a2a499836d5773ef98bc81eb2cab0afddb6dfcf4a30cf699`，C shim：
`4b4dc1fc6397fca9af3f50af9d39048b8d76d4544433135581eed55f5c3421b5`。
