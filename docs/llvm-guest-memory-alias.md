# Guest 内存别名与写屏障修补

本组件基线为 `d1a8467`，修补 `GuestMemorySSA` 中精确宽度事实传播和字节重建的
别名判断。公共接口、默认优化顺序及执行协议不变。新条件只在已有证明充分时保留
内存事实，不能证明不别名时清空相关事实并保留 LOAD。

## 四类修复

### 参数与局部对象需要独立性证明

旧 `isKnownLocalStore()` 把任何指针参数及其派生 GEP 都当成本地写入。这不适用于
未标注的额外指针参数：它可能指向 Guest data 中刚写入的同一位置。

例如 `data = load descriptor.data` 后，先经 `data+8` 写入 A，再经额外参数写入 B，
然后读取 `data+8`。若额外参数实际等于 `data+8`，正确结果是 B；忽略这次写入却会
前推 A。窄读取还可能在字节重建阶段得到 A 的旧字节。

现在只有两种明确情况可保留事实：

- 非逃逸的私有 alloca，由 LLVM capture analysis 判断，并限制最多检查 128 个使用。
- 带显式 LLVM `noalias` 属性的指针参数；属性是原 IR 的调用前提，不能由参数位置或
  名称猜测。原生正例只能传入实际独立的对象。

捕获的 alloca 不能仅凭分配指令取得独立性证明。通用 LLVM IR 可以先把其指针存到
descriptor，再从 descriptor 读出 Guest base；这时局部对象和 Guest base 可引用同一
批字节。该结构回归要求保留 LOAD。

这里必须区分执行合同层次：[执行契约](llvm-execution-contract.md)第 3 节规定 V2 的
寄存器快照与 Guest 内存是不同对象，Guest 地址不能指向寄存器快照。普通别名反例使用
额外指针参数，遵守这个分离条件。捕获 alloca 并改写 descriptor 的控制则属于未标注
通用 IR 的变换正确性检查，不能描述为符合 V2 caller-owned、稳定映射合同的机器执行。

### 分别加载的 Guest base 不是无别名证明

两次读取 `descriptor.data` 会产生两个 SSA 值，但运行时可以得到相同指针。
旧字节事实按 base 分组，更新第二组时没有失效第一组，可能从第一组重建旧值。

现在每次 Guest STORE 都清除其它 base 组的字节事实，再更新本 base 的覆盖字节。
不因 SSA 身份不同就推定两批内存独立。精确宽度路径中已有的保守失效规则继续有效。

### 普通事实传播也保留原子和同步屏障

旧普通扫描主要识别 CALL、LOAD 和 STORE，可能越过 `atomicrmw`、`cmpxchg` 或 fence。
例如同槽 `STORE 7 → atomicrmw add 1 → LOAD`，后一次读取不能前推为 7。

精确宽度及字节扫描现在都会在未知内存写入、fence、非 simple 的 volatile/atomic
访问处清空事实。局部存储独立性判断也只接受 simple STORE，不能借局部对象分类越过
这些屏障。普通调用的既有内存效果和已证明摘要合同没有扩大。

顺序原生输入能核对原子写入后的实际字节，但不能证明一般并发语义；atomic read、
volatile 和 fence 的保守边界还由结构回归固定。

### 区间距离不再经过有符号溢出

旧范围比较把模 64 位距离转换成有符号距离。当距离为 `2^63` 时，会对 `INT64_MIN`
取负，触发 C++ 未定义行为。普通优化构建可能仍给出预期布尔值，不能据此认定安全。

现在从实际 DataLayout 取得 GEP 索引位宽，计算两个方向的无符号模距离。仅在两起点
都不落入对方半开字节区间时证明不重叠。未知 base、动态地址、不同 base 或无法表示的
位宽继续保持可能重叠。证明不执行 Guest 地址，也不把大偏移当成可映射宿主地址。

## 实际旧失败与组件快检

永久测试为 `tests/lifters/TestLLVMGuestMemoryAlias.cpp`，共 4 cases、26 个形状变体。
所有测试使用已有 `runGuestMemorySSA()` API，因此旧头文件可以编译：

| 版本 | 实际结果 |
| --- | --- |
| 冻结 `d1a8467` 生产库 | 4 cases / 188 assertions，3 cases 失败、60 条断言失败 |
| 修复后的单独生产翻译单元，链接旧库的其它依赖 | 4 cases / 188 assertions 全过 |

未知参数、参数 GEP、捕获 alloca、两份 Guest base 及原子/同步访问都有拒绝控制；
私有 alloca 和显式 noalias 参数的精确/窄读转发正例保留。原 LOAD 的存活检查使用
`WeakVH`，不会因旧 LOAD 删除或 RAUW 而解引用失效对象。

`tests/semantic/LLVMGuestMemoryAliasUBSan.cpp` 另组成独立 sanitizer target，直接
编译实际 `lib/GuestMemorySSA.cpp`，而不是复制一份距离判断到测试中：

- 旧源码的 half-range probe 实际退出码为 1，UBSan 报告 `provablyDisjoint` 中对
  `-9223372036854775808` 取负。
- 修复源码的同一 half-range probe 退出码为 0。
- 修复源码的 100 组模区间分析探针通过。独立逐字节模型核对已折叠结果；不能重建的
  环绕读取允许保留 LOAD，不能把这种保守结果计为一次原生语义匹配。

以上大地址仅建立 LLVM IR 并运行生产分析，没有宿主解引用、Unicorn 执行或虚假的
高地址映射。组件快检不是最终完整库：旧库 backend SHA-256 为
`bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003`，新代码当时只
独立编译了本翻译单元。最终集成构建、完整 CTest 和独立原生矩阵由根代理另行汇总，
不把这些快检或有限区间探针冒充整程序语义证明。

## 证据位置

- VM：`~/vmc-runs/guest-alias-barriers-20261111/`。
- 本地：`/Users/c/Desktop/deob/validation-artifacts/guest-alias-barriers-20261111/`。
- 两端 `quick-component-evidence.tar.gz` 的 SHA-256：
  `9f1621e2491c519f066a2c62bd0b7ea118e2d7c639b7730393b02a0d3793c137`。

归档保留精确旧/新源码、单测与 sanitizer 可执行文件、编译记录、原始 stdout/stderr
及输入/二进制哈希。未加入排除、跳过或字段归一化来放过原生差异。
