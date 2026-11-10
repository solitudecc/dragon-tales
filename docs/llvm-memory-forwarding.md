# S2 Guest 内存 SSA 第一阶段

本阶段先交付一个显式、保守的 LLVM 组件：`LLVMLifter::runGuestMemorySSA()`。
它先按选项运行 mem2reg，再识别 Static/V2 函数中的 Guest memory base，沿成功的
线性内存检查块传递事实，处理精确宽度的：

- `STORE slot, value` 后的同址 `LOAD slot`；
- 没有中间写入的同址 `LOAD slot` 复用；
- 已知常量地址的明确不相交写入保留旧事实。

调用、未知别名、易失/原子访问、部分重叠、CFG 汇合点、循环回边和普通基本块边界
都会停止事实传播。所有真实 STORE、边界检查、失败出口和 CALL 都保留。预算耗尽时
不会伪造收敛报告；统计中的 `complete=false` 表示本阶段覆盖未完成。

API 在 C++、C shim 和 Python 中可用；默认 `optimize()` 顺序不变，不会隐式运行本阶段。
统计包括 Guest LOAD/STORE 数、转发/复用数、调用屏障、别名屏障、部分重叠和预算退出。

## 验证

S2 第一阶段新增 4 个回归用例，覆盖精确 STORE 转发、无写 LOAD 复用、CALL 屏障和部分
重叠写。旧代码对新 API 的测试编译失败，证据保存在
`validation-artifacts/llvm-memory-forwarding-20261110/s2-first/old-tests-result.json`。

新代码的聚焦用例为 **4/4、34 条断言**；主 CTest **831/831**，无 Z3 CTest **696/696**。
Python shim 对同一机器码样本返回 `loads_forwarded=2`，说明结果可从 C/Python 入口取得。

这还是同一基本块/成功检查链的第一阶段，不包含跨块 PHI、循环不动点、字节级重组、
未知外部调用摘要或一般堆分析；这些属于后续 S2/S3 工作。
