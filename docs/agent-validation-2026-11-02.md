# Agent tasks 验证记录（2026-11-02）

开发起点：`e0224b9`。使用独立 VM 副本 `/home/cc/dt-agent-all`；未修改
`/home/cc/dragon-tales` 中已有的未提交工作。Unicorn 使用 `~/ucdev/bin/python3`。
原始日志保存在该副本的 `validation-results/` 中。

## 任务 7：C shim / Python 接口

提交：`db3a66f`，已合入并推送 main。

- 新增 opaque `dragon_analysis_hints_t`、复制的入口列表和同步外部内存读取回调。
- 新增调用约定 getter/setter 和 Python `CallingConvention`、`AnalysisHints`。
- Python 保持回调存活，安全传播回调异常；关闭 hints 后访问抛出 `ReferenceError`。
- 新 C++ 测试使用了新增 API，无法在旧代码上编译。Python 回归测试在旧代码上
  因缺少 `AnalysisHints` 而失败。递归入口夹具从 1 条指令增加为 3 条。
- 外部跳转表的 3 条 CFG 边和 Win64 参数签名均通过行为验证。
- Ubuntu AArch64 全套 `ctest`：403/403 通过。本项未修改指令 lifter。

## 任务 6：存储值转发

匹配地址和宽度的 STORE 向 LOAD 转发保存时的值；跳过可证明不相交的写入。
CALL、不透明指令、未知别名、重叠但宽度不同的写入均阻止转发。
跨 join 只合并各路径一致的常量表达式；循环或预算不足保留未知。

- 旧代码运行新增的两组 C++ 回归测试：2 组失败，4 个断言失败。
- 新代码：3 组、16 个断言通过；全套 `ctest`：407/407 通过。
- `memory_slice_diff.py`：448 次 Unicorn 执行，192 个唯一切片，0 错误。
- `slice_diff.py`：3000 次匹配。
- `loop_slice_diff.py`：128 次 Unicorn 执行，4 个唯一切片，0 错误。
- `guard_truth.py`：27 个 CFG，20736 次 Unicorn 执行，0 错误目标集合。
- 单独构建 `DRAGON_USE_Z3=OFF` 的库后再次运行 `guard_truth.py`：
  27 个 CFG，20736 次执行，0 错误目标集合。
- 两个语料库共 69 个样本，线性与递归策略的跳转表对照：

| 指标（两种策略合计） | 旧版 | 新版 |
|---|---:|---:|
| 已验证目标集合 | 1064 | 1064 |
| 错误目标集合 | 0 | 0 |
| 未解析 | 212 | 208 |
| 缺少独立可识别的界限 | 534 | 538 |
| 脚本无法完整读取推定表区间 | 30 | 30 |

减少的 4 个未解析集合没有足够的独立真值，未计入已验证集合。
`truth_rel.py` / `table_truth.py` 原先错误地使用附近无关寄存器的 `cmp`；
现按实际索引沿前驱追踪无符号界限，所有路径必须一致，否则报告未知。
无完整表数据时单独计入 unreadable。脚本还补上了 x86 ELF 的 Intel 语法。
6 个独立 oracle 回归用例通过。

69 个样本的 LLVM 模块均通过 verifier，无新增语料库告警。
第一组有 3 个 x86-64 Lua 的已知 `objdump_missing=2`。
第二组另外复现两处旧版同样存在的告警：

- `cxx_mingw32_O2.exe`：线性/递归缺 IGNIL 块 2/1，未解码边 14/13，
  线性 objdump 缺失 70；主要告警边指向 0。
- `sys_libcrypto.so`：线性缺 IGNIL 块 13，未解码边 1（`0x2b9a80 -> 0x2b83c0`），
  objdump 缺失 102；递归对应计数均为 0。

这些数量已用保存的旧版共享库复核，属于已有基线，不能称整个语料库完全干净。

AArch64 Lua 的最后一张表仍未解析：其基址保存后经过 CALL，按要求让转发失效。
不能为了提高覆盖率跨 CALL 保留栈槽值。本项未修改指令 lifter。
