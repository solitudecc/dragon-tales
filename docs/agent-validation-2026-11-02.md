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

## 任务 5：去符号二进制的函数入口

入口种子来自 ELF FDE / EH frame header、初始化与终止数组、REL/RELA 代码重定位，
以及 PE x64 exception directory、导出和 TLS 回调。元数据种子独立于符号表存放，
不创建虚假的符号。只接受落在实际可执行文件字节中的地址。

`fstarts.py STRIPPED UNSTRIPPED` 使用对应未剥离文件的函数符号作真值。
PE 符号本身不含长度时，使用其 exception directory 的范围判断函数体内误报。
以下为递归策略；误报一栏只统计落在已有函数体内的入口，其他未知来源单独计数。

| 样本 | 入口召回：旧 → 新 | 解码指令：旧 → 新 | 函数体内误报：旧 → 新 | 分析秒数：旧 → 新 |
|---|---|---|---|---|
| sqlite AArch64 O2 stripped | 4/1874 → 1874/1874 | 48 → 236352 | 0 → 38 | 0.005 → 2.492 |
| sqlite x64 PE O2 stripped | 1170/2334 → 2084/2334 | 190139 → 247605 | 0 → 0 | 3.372 → 7.809 |
| Lua gcc x64 ELF O2 stripped | 4/700 → 699/700 | 41 → 46606 | 0 → 84 | 0.031 → 0.876 |

线性策略的入口召回分别为 1647→1874、1648→2084、685→699，指令总数保持不变。
其函数体内误报分别为 48→48、52→52、134→135。
函数体内误报仍包含原有启发式入口识别结果，不能把所有恢复入口都当成已验证函数。
Lua 尚缺 `register_tm_clones`（无函数体长度和对应 FDE）；PE 叶函数可没有 unwind 记录。

验证与硬化：

- 新入口与溢出测试在旧代码上失败。最终元数据测试：4 组、31 个断言；
  全套 `ctest`：412/412 通过（包括后述 A64 对齐恢复测试）。
- `Section::contains` / `readVirtual` 改用差值检查，拒绝地址或长度溢出。
- 新路径的 fuzz 曾触发 LLVM `RelocationRef::getType()` 的 fatal reader；
  已改为 `ELFFile` 的 checked `Expected<ArrayRef<...>>` 接口，并加入畸形重定位表测试。
  两份实际触发文件重新运行均为 OK。
- 修正 fuzz 脚本，只在加载调用处把文档规定的 `ValueError` 作为正常拒绝；
  后续分析中的异常仍属于失败。
- 修复后 200 次损坏输入：134 OK、66 LOADFAIL、0 signal、0 timeout、0 意外异常。
- 全库检查发现 FDE 指向的 `libcrypto` 入口 `0xebe00` 被原有逐字节恢复吞掉：
  解码器跨无效 A64 字吞入下一条合法指令。现按 4 字节恢复，保留真实指令边界。
  `ffffffff; nop; ret` 在旧代码中误解码为 `+1` 的 `fnmsub`，新增回归测试
  旧版 2 个断言失败，修复后通过。随后 100 次完整损坏输入：65 OK、35 LOADFAIL，
  0 崩溃、0 超时、0 意外异常。

格式核对参考：[LSB EH frame header](https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic.html)、
[Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)。

最终完整检查覆盖 34+35 个样本的线性与递归策略，所有 LLVM 模块通过 verifier，
无新增语料库告警。libcrypto 线性缺 IGNIL 块从 13 降到 4，未解码边从 1 降到 0，
objdump 缺失从 102 降到 70；递归仍无这些问题。其余上述已知告警保持不变。

## 任务 1 / 3 的浮点基础

新增独立 FP32 运算、精度转换、无符号转换、平方根及定向取整，三端保持
IEEE 原生精度。所有枚举追加到末尾，并同步 C/Python 值与静态断言。
LLVM 临时量保存原始整数位，使浮点结果能安全参与 V128 的拼接；访存及外部
寄存器快照显式使用字节对齐。SSE `cvtss2sd` / `cvtsd2ss` 支持寄存器和内存源，
正确保留未覆盖的 XMM 位。

- SSE 转换、LLVM 对齐测试在旧版失败；新增 FP32 API 测试不能在旧版编译。
- 20 个边界位模式由 Unicorn 独立求得，符号执行、切片 Z3、LLVM 常量优化均一致，
  包含 ties-even、次正规数、负零和整数边界；两组浮点测试共 182 个断言通过。
- 完整 `ctest`：416/416 通过。
- SSE 单指令对拍：162 模板 × 60 次，0 差异、0 未 lift；50 个 NaN 结果保持未知。
- Lua x64 O0 的 XMM 真实块：40 块、102 次有效符号执行对拍，0 差异；
  LLVM 编译后原生运行 40 块，0 差异（另跳过 12 次 Unicorn fault 和 53 个高地址块）。
- 69 样本的覆盖率基线已保存：A64 未 lift 186562、x64 2625、i386 12288。
  后续 SIMD、串操作和 x87 提交将与同一元数据恢复后的基线比较。

## 任务 1：AArch64 浮点与 SIMD

支持标量 FP32/FP64 运算、比较、选择、平方根、定向取整、整数/精度转换，
以及 V 寄存器的 B/H/S/D/Q 访存、配对访存、FMOV、MOVI/MVNI、DUP、INS、UMOV、
常用向量逻辑运算和 CLZ/RBIT。标量写入清零 V 的其余高位；高半 FMOV 和插入
保持其他位。FCVTZS/ZU 实现饱和和 NaN→0，并在转换前防止 LLVM poison。

- 新增 4 组 C++ 测试在旧代码上失败。追加的高半 FMOV 使先前实现的两组测试
  各失败一个断言，修复后通过。最终全套 `ctest`：420/420。
- 全量首轮加修复后的定向复测共覆盖 835 个单指令模板、每模板 40 次输入：
  832 个一致，3 个为已记录的 CALL/RET 抽象差异，0 未 lift、0 harness 错误。
  输入包括 ±0、±inf、NaN、次正规数、舍入和饱和边界，比较全部 32 个 V 寄存器。
- 全量对拍发现 10 个 unscaled 形式少匹配 LLVM 名字末尾的 `i`；修正后
  14 个 LDUR 和 9 个 STUR 模板全部一致。高半搬移的 6 个定向模板也全部一致。
- Lua A64 O2 真实块：100 块、273 次有效输入，优化前后各 0 差异；另 27 次
  Unicorn fault 跳过。LLVM 原生对拍 100 块、0 差异，另跳过高地址 93 块、
  未 lift 39 块、Unicorn fault 19 次。两个脚本现都初始化并比较 V 寄存器。
- 69 样本两种策略的完整语料库回归：所有 LLVM 模块通过 verifier，无新增告警。
  此后追加的两个高半 FMOV 形式通过定向对拍及全套 ctest；总验收仍将复跑全库。
- 31 个 A64 样本未 lift 指令总数：186562 → 111217，减少 75345（40.4%）。

| 样本 | 未 lift：旧 → 新 |
|---|---:|
| Lua A64 O2 | 912 → 90 |
| SQLite A64 O2 / stripped（各自） | 3036 → 517 |
| libcrypto | 64879 → 51034 |
| Python3 | 37232 → 23720 |

剩余包含 PAC/AUT、AES/PMULL、向量算术和结构化访存、部分整数/系统指令；
相应形式继续显式报告 UNDEF/unlifted。覆盖率提升不代表支持整个 A64 ISA。

## 任务 3：SSE / SSSE3 / SSE4.1 与 FP32

补齐任务列出的打包/饱和、乘法、平均/极值/SAD、字节重排/对齐、符号/绝对值、
提取/插入、扩展、PTEST、blend，以及 FP32 标量/打包和 FP64 打包运算、平方根、
极值、比较与转换。MIN/MAX 的 NaN 和 ±0 分支保留第二个操作数的原始位；
CVTTSS/SD2SI 将越界或 NaN 转为整数 indefinite，转换前先防止 LLVM poison。
缩窄内存源只读取架构规定的字节数。

- 两组新增 C++ 测试包含 129 个整数与 35 个浮点 Unicorn 参考状态，旧版均失败；
  新版通过 2648 个断言。全套 `ctest`：422/422。
- `difftest_sse.py`：393 模板 × 60 输入，0 差异、0 未 lift；700 个活动浮点
  NaN 结果保留未知，其他 lane 与保留位照常检查。
- SQLite x64 O2：100 个真实块、252 次有效输入，优化前后各 0 差异；48 次
  Unicorn fault 跳过。按原脚本，高地址内存 32 项、已知区域外写入 38 项不比对。
- LLVM 编译后原生运行 100 个块，0 差异；另跳过高地址 46 块、Unicorn fault 23 次。
- 28 个 x64 样本的未 lift 总数：2625 → 2508（包含先前 FP32 基础提交减少的 49 条，
  本次再减少 68 条）。SQLite PE O2：295 → 285；Lua PE O0：194 → 192。
- 最终 69 样本语料库回归将在全部任务的总验收中再次运行。

指令边界行为参考 [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)，
预期位模式以 Unicorn 独立执行结果为准。
