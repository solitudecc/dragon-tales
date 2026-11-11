# 历史布尔 PHI 的候选身份

这是 `guarded-state-corridors` 枚举失败后的补充预测，不是布尔谓词证明，也不向 GuestSSA 或其他分析发布事实。旧有限枚举、旧相关布尔枚举和已成功的 STORE 预测先返回；只有它们失败且实际遇到历史 i1 PHI，才执行新增重试。

以前，历史 PHI 把所有入边值合成并集。SSAUpdater 为原执行与复制执行建立的桥接 PHI 因而可能在一个场景内得到 `{0,1}`，不能再作为 select 的单一条件。新重试要求该 PHI 已执行、支配选中的 seed terminator，将 `(该 PHI, historical=true)` 作为一个整体布尔键。不同 PHI、旧 PHI 与新执行的 LOAD 使用不同身份；同一旧 PHI 的所有使用共享赋值。最多三个键、八个完整场景、八个候选值。每个完整场景必须得到有限结果，不能丢弃未知整数臂。

在使用历史键前，有界遍历其 SSA 依赖形状，拒绝显式 undef/poison 和周期。简单 LOAD、函数参数以及 freeze 是既有执行边界；freeze 的结果可稳定，不能剥掉或复用它的新执行。检查与场景分配、节点访问都计入现有 work/node/depth 限制。原运算及 flags、全部 LOAD/STORE、真实目标检查和同次 LOAD 的完整回退保留。这里没有证明内存不变或消除 poison。

新独立回归文件为 `TestLLVMGuardedHistoricalBooleans.cpp`，覆盖整体键相关性、不同历史 PHI、历史/新 LOAD、未知臂、周期、undef/poison、四键拒绝、预算和既有成功预测。它使用已有公开 pipeline API，可在旧 I 运行库上编译；实际旧失败与新验证数字由集成任务记录。本文件没有把静态审查或 IR verify 计作原生语义验证。

这一增量不处理最后原边留下空 PHI 的问题；后者属于独立 core 事务扩展。原独立 history 三次复制门槛仍需完整重跑，不能因第二次复制恢复便报告全部通过。

集成结果：原严格三次复制与分轮字节相等门槛现已在两构建通过，完整独立状态对拍见[最终原生验证](guarded-history-singleton-native-validation-20261112.md)。真实四样本结构闭合仍为 2/4，不能把本组件验收当作完整循环去平坦化。
