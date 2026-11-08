# 入口初始状态的条件化有限值集分析

基线 `9993d04`。本组件给已有的有限值集分析增加显式入口寄存器、内存约束，
沿实际 IGNIL STORE、分支、循环与调用效果推导后续值。分析本身仍是静态的，
不会执行目标程序，也不会自动把文件映像或 `.rodata` 当成不变内存。

这一步提供初始化分析可以使用的查询接口；尚未自动推导 `megaInit` 的全局或堆效果，
也没有实现通用去平坦化。调用者必须另行证明所提供的入口事实适用于目标执行。

## 契约与用法

```python
state = dragon.FiniteEntryState(
    entry,
    registers=[(register_offset, 64, pointer_value)],
    memory=[(table_address, table_bytes)],
)
report = graph.finite_memory_values_with_initial_state(
    [(block_address, load_op_index, table_address, 64)],
    [state], entry_points=[entry],
)
```

每个内存查询的程序点必须是一个真实、同目标位宽的 IGNIL LOAD；查询地址是固定
Guest 单元，可以不同于该 LOAD 的动态地址。错误程序点或位宽返回 Unknown，
不会猜一个相近的 LOAD。寄存器/临时值的块末查询使用 C++
`finiteValuesWithInitialState`；Python 的 `finite_values_with_initial_state` 接受
`query_register=(block_address, register_offset, width_bits)`。

结果保留原有限值集字段，并增加：

- `conditional: true`：所有结论均以随结果保留的入口假设为条件。
- `assumptions_valid`：只表示结构与初始化预算校验通过，不表示假设已被独立证明为真。
- `entry_states`：本次使用的入口、寄存器约束和初始内存字节的拥有式副本。
  JSON 地址和值为十六进制字符串，内存为保留前导零的十六进制字节串；Python
  把地址和值转换成整数，内存字节串保持原样。

`complete` 只表示有限分析到达不动点；Unknown 仍可能存在。Finite 是完整的过近似集合，
可以包含不可行值，不能把集合大小当成真实运行路径数。

C 接口为 `dragon_graph_finite_values_with_initial_state` 和
`dragon_graph_finite_memory_values_with_initial_state`，输入数组只在同步调用期间借用，
返回拥有式 JSON 用 `dragon_cstr_free` 释放。C++、C、Python 使用同一个原生分析器。

本组件只读，不给 Graph 安装目标、不修改 IGNIL 或 LLVM、不注册任何隐含内存读取器。
因此不能把条件结果直接作为通用 Graph 跳转边、LLVM 无条件常量或原二进制补丁。
原来的有限值集接口、默认反汇编和可选分发 pass 没有接入这些入口假设。

## 初始约束不是机器写入

32 位 EAX/Wn 初值只约束所声明的四个字节，不清零 RAX/Xn 的上半部；实际执行的
32 位机器写入仍遵循原来的高位清零规则。部分 SP 约束不能保留整个未知 SP 的符号身份，
也不能用零补齐其余字节。完整具体 SP 可以参与普通地址和别名推导。

内存事实表示入口时的字节。后续确定 STORE 替换相应字节，未知写或可能别名的写按
现有保守规则使事实失效；初值不是后续 LOAD 的持续保证。循环回边与入口状态合并，
不会每轮重新安装初始字节。合并多条路径时，任何未知输入都会使相关值保持 Unknown。

事实只能约束原本已准入的有效根，不会新增入口。未声明的独立根和解码出的独立被调
函数仍接受任意状态。默认 CALL 保留完整屏障；显式启用 `prove_leaf_calls` 也只能采用
现有[有界调用效果证明](stack-call-effects.md)，没有新增 API 名称白名单或 `memset` 摘要。
unlifted 和 IGNIL 局部控制流仍是屏障。

## 校验、预算与兼容性

寄存器事实宽度必须是 8..64 的整字节位宽，值必须可表示，范围不可溢出。
同一入口内的寄存器事实和内存范围均不可重叠，即使重叠字节相同也拒绝；相邻范围允许。
内存不可为空或越过最后一个 64 位地址，入口不可重复。

入口数、描述符、寄存器字节和内存字节共同受 262144 的硬上限及
`max_facts` / `max_operations` 限制。校验后拥有的假设也计入分析驻留事实预算。
无效假设或初始化预算不足使整批分析 `complete=false`、所有结果 Unknown，
并且不保留未经有界校验的假设；后续分析预算不足同样不输出部分目标集。
C/Python 在分配前另做输入大小、空指针/计数和整数范围检查。

新接口使用新结果类型，没有向旧 `FiniteValueResult`、limits、AnalysisHints 或 Guest
状态追加字段。旧 JSON 不增加条件字段。此分析不需要 Z3。

同时修正既有有限分析的寄存器读写范围检查：查询偏移加位宽对应字节数若会回绕，直接返回
Unknown。反例是在高偏移 `0x1ffffff8` 保存常量、使 SP 未知，再查询
`Reg{0xfffffff9,64}`；旧检查会因无符号回绕而误命中另一处寄存器，输出其右移后的值。
合法高偏移仍可以查询。这是旧有限值集接口的边界缺陷，区别于新增入口状态能力。

写入端也先检查实际字节范围，再检查未配置父寄存器时的 32 位写入扩展范围。
不可表示的写入使寄存器事实保守失效，避免保留被覆盖的旧常量；零宽写作为无效 IR
同样处理。合法高地址写、已配置父寄存器的部分写，以及合法宽 UNDEF 的失效范围均保留。
这部分是寄存器模型的边界校验，不是新增机器指令语义。

## 验证边界

回归覆盖入口约束与真实写入的区别、部分 SP、相邻/跨片段内存、地址运算回绕、
STORE/CALL/别名屏障、独立入口、循环、所有入边汇合、无效查询和预算失败。
新 API 在旧 SDK 上无法编译，这一限制须与旧接口无约束分析的实测结果分别记录；
旧接口返回 Unknown 符合其契约，不能记为错误语义。

参考验证比较实际机器 LOAD 时的内存与查询集合，Finite 必须包含每个观测值，Unknown
单列。Graph/IGNIL/边的前后指纹验证只读性；它不等于证明 LLVM 完整函数执行或
默认 Tigress 分发已恢复。本轮不修改机器 lifter、IGNIL 优化器或 LLVM 降级，因此
不把以往全指令/原生 IR campaign 的数字算作本组件的新覆盖。

## 本阶段实测

主构建 CTest 715/715、无 Z3 583/583。每个构建的相关 C++ 回归均为 21 用例 /
896 条断言，其中 19 个入口状态用例、2 个旧接口范围回归；C API 另有 3 用例 /
66 条断言，Python 接口测试也在两个完整 CTest 中执行。

```sh
./build/bin/dragon_tests '[finite-initial-state],[finite-register-range],[finite-register-write-range]'
./build/bin/dragon_c_tests '[finite-initial-state]'
DRAGON_C_LIB="$PWD/build/c_shim/libdragon_c.so" python3 tests/python/test_finite_initial_state.py
```

新增 API 的最终 19 用例在旧 `9993` SDK 上因接口不存在而无法编译，新头覆盖下可编译。
原接口范围回归则使用匹配旧 SDK 和真实 `9993` 后端成功编译、实际执行：24 条断言中
15 通过、9 失败，四个越界查询及错误 JMP 结果被检测。修补后全部通过。
写入回归也在同一旧库真实执行：92 条断言中 57 通过、35 失败；其中 28 条属于四类
范围反例，另 7 条是零宽无效 IR 的保守失效控制，不冒充真实机器指令差异。
另有旧库同一组小机器码的 30 条无 seed 查询均为 Unknown，这符合旧契约，不计错误。

独立 Unicorn 参考有 45 次小夹具完整返回和另外 2 次 CALL 返回槽完整返回；后两次还精确
检查 46 个架构字段及全部 64 KiB 内存。参考返回不等于 Dragon IR 的完整函数执行对拍。
每个最终库配置各实际执行 86 次只读 API 调用，得到 166 个固定单元结果：24 Finite、
142 Unknown，Finite 均包含全部对应观测值，漏值为零，Graph/IGNIL/边的前后指纹均相同。
两配置引用同一批冻结 CPU 记录，不把重复使用计成新增 CPU 样本。

另有冻结 Tigress MBA O2 机器码的 8 条 kernel 入口到首次表 LOAD 前缀，包含输出指针与
表重叠的条件；两策略共 96 个表单元结果均为 Unknown，没有新增完整恢复收益。
本组不执行包装入口或 `megaInit`，不注入初始化堆快照；SBOX 保持 ELF `.bss` 零值，
首次 LOAD 前尚未读它。kernel 内真实 `memset` 仍执行，分析保留其未证明调用屏障。
该参考使用当前平坦 Guest 模型：1 MiB 内存整体映射为可写，不模拟 Linux 的 ELF 段权限。
前缀停在首次 LOAD 前，不计完整函数返回，也不是前阶段的整程序重放。

最初 Tigress 参考误以为首次表跳转前只有初始化向量写入；真实轨迹还经过 default 分支
的输出更新。原失败及第一版计划保留，第二版增加逐输入路径、完整输出和写事件检查，
没有删单元或放宽比较。读范围修补前的 22 个语料任务，以及写范围修补前的 56 个任务、
对应库和查询，分别封存为 pre-hardening，不算最终覆盖。

最终主配置重新执行两套语料的 69 个输入、138 个独立 Graph 任务：205021 个函数候选中
205018 个 Graph-valid 检查点，全部与已冻结的 `9993` 记录相同。完整比较 17160761 条
指令查询、3146633 条块查询，共 23659048 条 canonical 记录、6722719390 字节；
指令字节、边、调用、函数成员和 IGNIL 均逐条一致，原有告警也保持原样。
驱动的 28 个负控制通过，完整任务集及原始退出记录的最终审计通过。

这轮语料验证是默认 Graph/IGNIL 的结构兼容性，不是全程序语义证明；没有新执行
objdump 或全语料 LLVM 生成/原生对拍。上一阶段的 1671 个 raw IR 模块不算本轮新执行。

原始参考、旧库失败、SDK、源文件与实际 DSO 映射记录位于 VM
`~/vmc-runs/entry-state-memory-20261108/` 和开发机
`/Users/c/Desktop/deob/validation-artifacts/entry-state-memory-20261108/`。
旧 `9993` 库可运行副本在 VM `/tmp/entry-state-baseline-9993/`；长期恢复以 Mac
冻结包及逐成员清单为准，不依赖可清理的 `/tmp`。
