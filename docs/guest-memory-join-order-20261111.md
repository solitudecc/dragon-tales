# Guest 汇合顺序与局部预算诊断

基线 `9f9e5f9`。本轮分别处理同输入的宿主布局敏感性和解释真实样本的
`INCOMPLETE`，没有调整预算，也没有将合理的局部搜索上限判为语义错误。

## 按 IR 顺序选择汇合候选

普通 GuestMemorySSA 的 `mergeIncoming` 以首条入边的事实作为候选，再检查所有
其他入边是否都有相同地址和宽度的事实。原先直接遍历指针键 `std::map`，首条
入边由 BasicBlock 的宿主地址决定。因此一个前驱有事实、另一个没有事实时，
`joinBarriers` 可能为 0 或 1；两个前驱都有不同值时，新 PHI 的 incoming 顺序
也会随分配变化。计数没有被改成恒零，也没有增加未经证明的事实。

修复为函数中的原始块建立 IR 顺序编号，在合并前据此排序入边；排除 backedge、
逐路径地址/宽度检查、值定义检查、PHI 开关和改写预算均保持原样。只保证相同
输入 IR 的这项顺序稳定，不保证任意等价但块顺序不同的 IR 具有相同统计。
本轮没有证明原问题会导致错误执行或实际预算精度下降。

两个新单元测试在分配后按实际指针顺序指定左右块的角色，再按相同的 IR 块顺序
插入函数。两种宿主布局具有逐字节相同的输入，避免依赖概率性 allocator padding。
测试分别检查 join 拒绝计数、保留的 LOAD，以及生成 PHI 的全部入边和值、输出 IR。
相同最终测试在旧 2e50 库运行 2 cases/44 assertions，其中 6 assertions 失败；
新相关组 main/no-Z3 各 84 cases/3560 assertions 通过。完整 CTest 为
main 1015/1015（85.17 秒）、no-Z3 880/880（73.72 秒）。

## 独立原生执行验证

`scripts/validation/guest_memory_join_native.cpp` 独立生成 checked Guest 写入及
汇合 LOAD：两个前驱分别写 10/20，汇合值参与返回值、寄存器和 Guest 输出。
`scripts/validation/guest_memory_join_diff.py` 使用 Python 字节模型推导预期完整状态。
输入包括 15 个地址（未对齐、输出区域重叠、末端合法/非法、符号位与全 1）、4 个
分支值和 8 个边界种子，共 480 个输入。越界路径有独立的完整 Exit 预期，没有排除。

主构建旧库和新库各自在两种强制宿主布局、优化前后 IR、原生 O0/O2 下完成
（本原生矩阵未另外运行无 Z3 库）：

- 3840 次完整状态比较，0 差异；比较返回值、512 字节寄存器、全部 64 KiB Guest、
  七个 Exit 字段和零调用计数，并检查描述符未被修改。
- 1920 次故障 IR 执行，1408 个预先指定的有效地址故障全部检出；删 STORE 和
  错误汇合值各有独立的故障模型，实际故障输出也与其完整模型逐字节相等。
- 5283 个字段/长度比较器控制通过；跳过、排除、归一化均为零。

旧版两布局的 before IR 相同、after IR 不同；新版 before/after 各自在两布局间相同。
两版冻结验证源码完全一致。另逐个复核保存的 16 个正常流和 8 个故障流，核对每条
记录的 SHA、完整长度、预期流及指定故障集合。旧版原生结果也正确，因此这些结果
支持“顺序确定性修复”，不能被描述为修好了已复现的原生错误值。

运行库 SHA-256：

```text
old 2e509ceb03bc5b47d2e95fdacfd579860e520f238ee239746c273b6034ba9bda
new f3cb7078bf88be92e376f38454edfc14adcec86ac75c403bd3434c3b62d57f12
```

既有固定真实 IR helper 再运行 8 个 padding，新版 join 全为 0、loop 全为 35，
其他报告字段也逐项相同。所有 before/after IR 同 SHA：
`fb4b28223e1502b18fabf21e10f6f829eca74c6744925fd3a0a85984b4a40cb0`。
这是 8 次分析、0 次目标原生执行；运行前后新库 SHA 一致。它没有重跑四个 ELF，
也没有把旧严格统计比较的失败改为通过。

## 为什么固定真实输入仍然 incomplete

对已冻结的 `counter-fixed-input.ll`，使用旧 2e50 库和现有 helper 的只读 GDB
诊断，完成三组各 8 次分析。输入文件 SHA 为
`5595f90f1b5baceebea0008929891dc154e46d84ee64e6dcde44fae207c820d1`。
这是代表性 f-cff O2 输入；不能把此诊断冒充原四模块全部 12 次调用的逐一定位。

每次最终 `CheckedMemoryEdgeBudget` 为：candidates=69/512、edgeProofs=35/2048、
indexNodes=0/65536、clones=0、clonedInstructions=0、repairCells=0、stopped=false、
reported=true。因此这里的 budget=1 是局部 `exhausted` 汇总成一次报告，并不是
全局共享预算被耗尽。

`findCorridor` 有三个实际 6-block 上限触发，累计指令数分别为 56、56、30。
它们位于 `dragon_static_entry_x86_64_10080` 的 `%1045@memory.valid102`、
`%1054@memory.valid104`、`%1063@memory.valid106`。例如第一条反向路径为：

```text
memory.valid102 → memory.valid100 → memory.valid98
→ memory.valid96 → memory.valid94 → memory.valid92
```

继续走到 `bb_66529` 才会超过六块，属于连续检查/LOAD 链的合理局部复制边界。
没有为获得 complete 提高限制，也没有依据聚合报告推断其他局部上限同时耗尽。

优化构建中 `GuestMemoryEdges.cpp:372` 的 GDB 行断点映射到了普通证明路径，
532 行断点实际停在 `prependHub` 的 537 行正常路径。不能将这些命中数当作
repairCells 或 hub 尺寸耗尽次数；初步按行号归类的判断已撤回，原日志与限制保留。
实际最终 repairCells=0、stopped=false 也排除了该输入的全局修复预算耗尽。

代码层面仍可考虑缓存未变 CFG 上的失败 direct 搜索，避免 hub 阶段重做；但必须
在 CFG/PHI 改写后失效，不能只按 LOAD 指针缓存。该输入仅用了 35 次 edge proof，
这个优化不会消除上述局部限制，本轮不实施。其他真实调用的具体耗尽原因仍需
分别定位，不能仅凭同样的 budget=1 推断相同根因。

## 证据保存与边界

VM 目录为 `~/vmc-runs/guest-join-reference-20261111/` 和
`~/vmc-runs/guest-budget-diagnosis-20261111/`。完整归档为
`guest-join-and-budget-reference-20261111.tar.gz`，同时保存在 VM `~/vmc-runs/`
与 Mac `/Users/c/Desktop/deob/validation-artifacts/`，包含 192 个 manifest 条目
以及两个 manifest 本身。VM/Mac 分别核对全部文件集合和内容 SHA，一致；归档
SHA-256 为 `040efd460084f792bc2d351ecef7cdee5ee30f320dee18dd9912c70c3fdf3aca`。
运行库本体由主验证的冻结运行库档案保存，本归档固定 SHA，不重复存放库。

未扩大普通循环不动点、表内容不可变性、跨调用证明或真实 CFF 去除范围；默认
流水线和公开 ABI 保持不变。byte walker 的事实合并及其他指针键容器没有在本轮
一并重写，不能因此宣称框架全部统计或全部优化输出都已获得确定性。
