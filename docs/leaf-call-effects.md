# 从实际指令证明叶函数的保留状态

2026-11-06，基线 `5c68ce7`。这是有限值集分析的可选能力：遇到可以证明效果的
x86 直接调用，保留未被改写的寄存器字节和未被 CALL 覆盖的内存事实，继续恢复分发器。
证明来自当前 Graph 的机器指令和 IGNIL，不依赖函数名、API 名称、固定地址、调用约定或
混淆器模板。分析仍是静态的；Unicorn 仅用于验证。

## 证明范围

当前接受 x86-32/x86-64 无前缀的 `E8 rel32` 调用，要求被调目标满足：

- 遍历所有可达块，所有出口均已解码，返回均为普通 `C3`；直接分支的两条路径都检查。
  可以保守检查有循环的 CFG；证明不承诺循环终止、无故障或必定返回。
- 所有块都有完整 IGNIL，且没有 unlifted、局部控制流、嵌套 CALL、间接跳转、trap 或
  未列入支持范围的操作。当前范围是整数操作、LOAD、UNDEF 和直接控制流。
- 没有 STORE，也没有任何 SP/ESP/RSP 别名写入。即使 push/pop 最后恢复 SP，目前也拒绝。
  不把“最后 SP 相等”误当成“没有内存副作用”。

收集每条路径可能写入的寄存器字节，包括 32 位写隐含的父寄存器清零。
只保留完全未被写过的字节；返回值和写后恢复的寄存器目前不做表达式摘要。
IP 始终按改变处理。未知写、未知出口和不支持的指令使整个调用保留旧的全屏障。

近 CALL 自身写入调用前 `[SP-pointerSize, SP)` 的返回地址。证明成功后仍使这个区间的
既有内存值失效；只保留已证明不相交的槽。SP 不可定位时清空内存事实。
不同符号基址、具体地址与符号地址仍可能别名，未知地址 STORE 仍清空内存事实。
不会因被调函数没有 STORE，就把所有调用前栈值视为不变。

每个被调目标最多检查 32 个块、4096 个检查/操作，并受本次有限值集的全局预算限制；
局部证明超限只拒绝该调用，全局超限使所有查询 Unknown。证明缓存也计入事实预算。
缓存只活在一次不可变 Graph 的分析中；每次扩图重新建立，最终入边完备性复验和事务回滚不变。

## API 与兼容性

默认关闭。C++ 设置 `FiniteValueLimits::proveLeafCalls=true`；只读 `finite-values`
和已注册的分发器 Pass 都接受整数选项 `prove_leaf_calls=0/1`。

```python
facts = graph.finite_values(entry_points=entries, prove_leaf_calls=True)
with dragon.PassPipeline() as pipeline:
    pipeline.register_finite_dispatch(code, base, entry_points=entries)
    pipeline.add('finite-dispatch', prove_leaf_calls=1)
    report = pipeline.run(graph)
```

`proven_calls`、`rejected_calls` 为一次分析内尝试过的唯一调用地址数量，不是循环访问次数。
分发器报告取最后一轮的计数；若扩图回滚，这些计数只是尝试过程的诊断，不代表已发布新的边。
`complete`、`ok`、`Success` 的含义仍见[有限值集契约](finite-dispatch.md)。

C 新增 `dragon_graph_finite_values_with_calls(..., uint8_t prove_leaf_calls)`，使用原有的
owned JSON/free 契约。旧 C 函数签名及默认全 CALL 屏障保留；JSON 增加两个计数字段。
C++ 的 limits/result 结构追加字段，使用这些结构的 C++ 客户端需要重新编译。
默认反汇编、旧常量切片器、指令语义、IGNIL 优化器、LLVM 优化顺序和 Guest ABI 没有改变。

## 实际停点与边界

这一步适用于状态值或显式初始化的栈表跨过只读叶函数调用的情形。
更改叶函数的指令后会重新证明效果；不符合条件时退回 Unknown。
有栈帧的 helper、memcpy 一类写内存的函数、嵌套调用、堆初始化和调用返回值推导仍需后续组件。

冻结的 Tigress GCC indirect O0 还有未知输出指针 STORE，可能别名栈表；其 helper 本身也有
栈写。GCC indirect O2 虽有可证明的纯叶函数，前面的表初始化仍读取未证明不变的 `.rodata`。
Clang O0 还通过 Guest memcpy 初始化表。这些均不能仅靠本组件解决，不以 ABI、映像字节或
测试时的内存布局替代证明，也不恢复 CET 修复删除的内部伪函数入口。

本组件不执行去平坦化、不折叠堆不透明谓词，不生成去混淆后的二进制。

## 验证

- 五个 C++ 用例、242 条断言通过。首批四个仅测试改动在旧源码上构建后全部失败；最终五用例加载
  冻结的 `5c68ce7` 旧库全部失败（20 条断言中 5 条失败，均在新选项/行为的入口）。
  另外分别故意去掉返回栈槽失效、寄存器 clobber，两组各失败 4 条断言，确认测试能抓住
  实际错误证明。旧库不支持选项的失败不能单独当成语义验证。
- 六个永久 x86-32/x86-64 机器码夹具覆盖栈表、寄存器状态和有分支的叶函数。
  两种反汇编策略 × V2/静态 IR/产品优化 IR × O0/O2 × 8 个边界/随机输入：
  新实现 576 次完整 root 返回与 Unicorn 一致；同一输入上的旧有限值集 Pass 为
  576 次 incomplete，不能计作语义通过。静态两条路径均使用 FunctionStart 入口策略。
  所有模型字段（x64 76 / i386 52）及 64 KiB 内存均比较。
- 无 Z3 追加 48 次完整 root 返回。结果、内存负对照分别检测 24/24 个单字段差异；
  CFG 对照漏掉一个目标时，检查器抓到 12 次实际缺边，执行语义本身仍有 24 次匹配。
  上述对拍的排除、跳过、归一化均为零；生产运行的 wrong edge 和完整执行语义差异均为零。
- 最终 CTest 主构建 607/607，无 Z3 482/482；Python API、守卫和寄存器/内存/循环切片
  检查通过。新旧 API 的默认 CALL 屏障和显式启用路径均有检查。
- 两个真实语料库 69 个输入、138 次策略分析，共枚举 205409 个函数候选入口，
  包含原有 3 个无效候选；与冻结的 CET 记录核对后有效函数仍为 205406 个。
  默认 Graph、IGNIL、函数成员和 unlifted 指纹与冻结的 CET 旧库逐输入完全相同；
  输入哈希全部匹配。没有重跑全语料 raw/optimized LLVM IR campaign，默认 Graph 到 IR 的
  生产路径没有改动。这项检查只证明兼容性。
- 冻结的真实 Tigress 2.1 seed 1901 原 helper 调用样本：GCC/Clang × 两策略，
  每组四个 goto/indirect O0/O2 ELF × 六种提升/优化计划 × 16 输入，共 256 次 C 匹配，
  LLVM 576 次完整匹配 / 960 次 incomplete，完整差异与 unlifted 均为零。
  与 CET 阶段一致，没有新增完整执行收益；O2 goto 的只读 helper 调用能被证明，
  indirect 的内存/栈帧停点仍保留。不能把“证明了调用”写成“完成了 indirect 去混淆”。

没有新增单指令或 IGNIL 优化语义 campaign；相关实现没有改动。全语料不执行本可选 Pass
的兼容性检查与针对新能力的完整函数语义检查应分开理解，均不证明任意混淆程序可完成分析。
原始输入/输出、IR、可执行文件和报告共 8436 个文件存入逐文件 SHA256 校验过的
`raw-artifacts.tar.xz`；新旧及无 Z3 双库、接口快照、源文件清单和对照日志保存在
`~/vmc-runs/leaf-calls-20261106/`。
