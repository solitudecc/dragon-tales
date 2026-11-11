# Checked Guest 入边克隆的独立结构回归

本说明只记录 `tests/lifters/TestLLVMGuestMemoryEdges.cpp` 的独立 C++ 结构测试。
它不宣称完整 CTest、无 Z3 构建或原生语义矩阵已经完成；这些结果由集成验证另行记录。

## 测试来源与运行绑定

测试使用既有 `detail::runGuestMemorySSA(module, options)` 接口，没有因缺少新 API 而
让旧代码编译失败。最终同一份源码只编译一次，然后以 `LD_LIBRARY_PATH` 分别绑定三份
冻结后端，每次保存 `ldd`、后端哈希和原始 stdout/stderr。没有用额外组件对象覆盖库中实现。

最终源码 SHA-256：
`918cfca49efe74cd7779591efa49bfc8ce544f2fa27090be6f561fa9b5a32a82`。

独立测试可执行文件 SHA-256：
`7e3a70649e6a1f8b91c40dee70b09b0e24d6d50ec742b68a0f033e788c661012`。

| 后端 | SHA-256 | 实际结构结果 |
| --- | --- | --- |
| 基线 `50f698c` | `7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799` | 16 cases / 871 assertions；10 cases、153 assertions 失败 |
| 初版集成 v1 | `694717a0b0ac8d81ecfdfda114aa2f3f10987c8f1ebbc95f9a499bb0d96e5c70` | 16 cases / 873 assertions；2 cases、50 assertions 失败 |
| 修正集成 v2 | `bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003` | 16 cases / 873 assertions 全部通过 |

三个运行的 verifier stderr 及最终编译 stderr 均为零字节。旧/新断言总数不同，是克隆后
外部块新增修复 PHI，测试对实际 PHI 逐项检查而产生的差别，不能据此推算额外语义输入。

v1 的两类失败来自独立测试发现的具体缺口：Sub 地址无法翻译，及显式不稳定地址被当作
同一地址消费。后者是保守分析合同的结构控制，没有执行 undef/poison 后人为指定结果。

## 被固定的合同

基本图有一个写入 50 的已知前驱和三个没有这个证明的前驱，共享带边界检查的 Guest LOAD。
Guest base 从第二参数 descriptor 读取，Guest 地址为显式 byte-GEP，访问 alignment 为 1。
NoFolder 保留原始 SSA 运算，测试前不依靠 GVN 或 InstCombine 消除状态读取。

已知边上的 `RBP=SP−8` 可把 LOAD 地址 `RBP−120` 对应到该边的 STORE 地址。
测试同时保留 Add 负常量形状和真实原生 oracle 使用的 Sub 形状：

```llvm
%known.rbp = sub i64 %sp, 8
%store.index = sub i64 %known.rbp, 120
; join 中 %rbp 为四入边 PHI
%load.index = sub i64 %rbp, 120
```

正向检查要求已知边进入新路径、其余三边仍进原汇合、原 Guest LOAD 和原 STORE 保留，
克隆后 LLVM 模块合法。检查与业务值确实跨出克隆区；共享成功/失败目标还有原本就存在的
常量 PHI，不能仅修复 region 定义的 uses 而漏加新增 CFG 边的 PHI incoming。

覆盖还包括：

- 外层 guard、连续两层内存检查、失败出口的七个写入标记及共享业务 live-out。
- 未知循环回入；未知 PHI arm 为 undef/poison 时，独立的已知入边仍可使用自己的证明。
- STORE 采用同一个 freeze 后的稳定值，且地址只在复用同一个 freeze SSA 时可匹配；
  两个独立 freeze 不得因源操作数相同而被认定相等。
- 未知 CALL、参数别名写、部分写、fence、atomic RMW/LOAD/STORE、volatile LOAD/STORE
  和副作用汇编，分别位于 STORE 尾部与检查区，共二十个拒绝变体。
- 重复入边或出口边、不同 Guest-base LOAD、部分宽度 STORE、非 64 位地址索引均保守拒绝。
- `enableCrossBlock`、`enablePhi`、零改写预算；跨函数共享 `maxRewrites`；短路径块数、
  指令数及一次调用内共享的 64 次入边克隆上限。拒绝情形对整个 IR 文本检查不变。

## Sub、undef 和旧迭代边界

证明语言不能去掉 Sub 的 nuw/nsw 标志来构造不存在的普通整数 SSA 匹配。四个负例分别
只给 LOAD 或 STORE 侧 Sub 加 nuw/nsw，要求保留原图。显式 undef、poison 和含 undef 的
未冻结 Add 地址也不得仅因 Value 指针相同就取得稳定地址证明。

另一个循环控制让检查区每轮重新执行 `freeze(poison)` 得到本轮地址；成功路径可经
STORE 块写入本轮地址，然后回到汇合。STORE 使用的索引指令虽然与下轮源码名称相同，
它对应的动态执行已经不同。这个图必须保持原 LOAD，不得把上一轮写入作为下一轮值。
该控制在 v1 也通过，用来约束新增 Sub 结构匹配不能扩大到旧迭代索引。

这些 undef/poison/freeze 场景只创建 LLVM IR、执行分析并检查结构与 verifier。
不把任意位固定成零，不编译执行这些 Guest 路径，也不把结构标记当成全部状态语义证明。
实际值、完整寄存器、Guest 内存与七字段 Exit 的对照属于独立原生验证器的覆盖范围。

## 证据路径

最终同源三库记录位于 VM：
`/home/cc/vmc-runs/checked-edge-tests-20261111/final-controls/`。

其中保留 `TestLLVMGuestMemoryEdges.cpp`、`edge-unit`、`compile.log`、
`source-and-unit.sha256`，以及每个版本的 `.stdout`、`.stderr`、`.ldd` 和 `.backend.sha256`。

早期首版与 Sub 缺口控制分别保存在同级 `old-50f698c/`、`old-bounds-50f698c/`、
`sub-controls-50f698c-v1/`；旧迭代单例记录位于 `prior-iteration-control/`。
它们不是最终同源 16 例的替代，也不重复计入最终结果。
