# Guest-memory SSA 不可约循环的保守覆盖

基线为 `26e1fce`。原前驱计数调度器排除已证明的自然回边，但不可约循环的两个独立入口
可能互相等待，导致后面的可达块没有被扫描。本次在原 worklist、自然循环证明及延迟改写
全部结束后，补扫尚未访问的可达块。

补扫块各自从空事实开始，只使用本块合法 STORE 建立的值；不导入前驱事实、不向任何后继
导出事实，也不构造 PHI。精确宽度转发和小端常量地址的完整字节重建采用相同边界。
未知调用、未知别名写、fence、volatile/atomic 访问仍清空本块事实；不同 Guest-base LOAD
也不是无别名证明。改写上限继续返回 `complete=false`。`blocksVisited` 现在包含这些
保守补扫块，API/ABI 未增加字段，默认流水线未改变。

这不是一般 SCC 不动点，也不建立 SP/RBP 关系、表内容不变性或跨块内存证明。

## 永久回归与原生 oracle

`tests/lifters/TestLLVMGuestMemorySSA.cpp` 中 `[guest-memory-irreducible]` 冻结测试
提交为 `8078b19`。主代理在旧 `26e1fce` 实测 **5 cases / 105 assertions，22 failures**：
旧代码只访问入口块（1，应为 4），没有进行两个本块精确转发和两个字节重建，改写预算
场景也没有触发。负向场景包括缺失前驱事实、未知调用、别名写、fence、atomic RMW、
volatile 读/写、部分写以及可能别名的两个 Guest base。新代码的最终完整构建与 CTest
由主代理集中执行；下面的单文件快检不代替最终集成结果。

原生 oracle 为：

- [`guest_memory_irreducible_native.cpp`](../scripts/validation/guest_memory_irreducible_native.cpp)：
  生成 11 种有限不可约 CFG，保存原始和实际 pass 输出的 IR，并验证模块。
- [`guest_memory_irreducible_native.py`](../scripts/validation/guest_memory_irreducible_native.py)：
  同时执行 before/after 原生函数，与独立字节内存模型比较返回值及全部 64 KiB Guest 内存。

快检只独立编译新的 `GuestMemorySSA.cpp`，链接冻结旧库的其它依赖，没有新建 CMake build。
冻结依赖库 SHA-256 为
`a33dc8c9c1b79125a2a499836d5773ef98bc81eb2cab0afddb6dfcf4a30cf699`。
结果为 **528 输入 / 1056 次原生比较匹配**，返回值和内存的单字段错误负对照各检测
**1056 次**，差异、跳过、排除及归一化均为 0。统计为 `blocksVisited=44`、
`loadsForwarded=4`（精确 2、字节 2）、`byteRebuilds=2`、`phisInserted=0`，模块验证通过。
这验证的是 LLVM 内存变换，不是新的机器指令 lifter 或整程序正确性证明。

VM 快检产物：`~/vmc-runs/guest-memory-irreducible-20261111/quick/`；本地摘要：
`/Users/c/Desktop/deob/validation-artifacts/guest-memory-irreducible-20261111/`。

可在最终完整库上重现（在仓库根目录执行；oracle Python 文件头包含 `helper.c` 的完整内容）：

```sh
clang++-21 $(llvm-config-21 --cxxflags) -std=c++20 -Iinclude \
  scripts/validation/guest_memory_irreducible_native.cpp \
  -Lbuild/lib -Wl,-rpath,"$PWD/build/lib" -ldragon -o /tmp/guest-memory-oracle
mkdir -p /tmp/guest-memory-oracle-out
/tmp/guest-memory-oracle /tmp/guest-memory-oracle-out
llc-21 -filetype=obj -relocation-model=pic /tmp/guest-memory-oracle-out/before.ll \
  -o /tmp/guest-memory-oracle-out/before.o
llc-21 -filetype=obj -relocation-model=pic /tmp/guest-memory-oracle-out/after.ll \
  -o /tmp/guest-memory-oracle-out/after.o
gcc -shared /tmp/guest-memory-oracle-out/before.o /tmp/helper.c \
  -o /tmp/guest-memory-oracle-out/before.so
gcc -shared /tmp/guest-memory-oracle-out/after.o /tmp/helper.c \
  -o /tmp/guest-memory-oracle-out/after.so
python3 scripts/validation/guest_memory_irreducible_native.py /tmp/guest-memory-oracle-out
```

本构建的 `libdragon.so` 已包含 LLVM，oracle 不再额外链接另一份 `libLLVM`。初次快检曾
遇到正在重链的非冻结库无效 ELF，以及同时链接另一份 LLVM 的进程退出错误；没有将这些
未完成尝试计入原生匹配数。之后固定上述依赖库、只链接一份 LLVM，完整矩阵通过。

## 最终集成库的独立复验

完成生产集成后，重新编译上述 producer，**只链接真正集成库，不再编译或链接独立的
`GuestMemorySSA.cpp/.o`**。`nm` 记录的 `detail::runGuestMemorySSA` 为未定义导入符号，
`readelf` 记录的 RUNPATH 指向冻结目录，没有额外的 `libLLVM` 依赖。

冻结目录为 `/home/cc/vmc-runs/llvm-local-state-20261111/final-runtime/`：

- 本次实际使用的 `libdragon.so` SHA-256：
  `7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799`。
- 同批冻结的 `libdragon_c.so` SHA-256：
  `cb01b3cb7723d69e1902fc89e393c62729c01e3fdc25cfd94aeef5827e11e855`。
  本轮 producer 和 IR 探针均调用 C++ 接口，没有声称重新验证 C shim。

最终集成原生矩阵仍为 **528 输入 / 1056 次返回值及完整 64 KiB 内存匹配**；返回值和
内存单字段错误负对照各 **1056 次被检测**。差异、跳过、排除和归一化均为 **0**。
统计严格核对为 `blocksVisited=44`、`loadsForwarded=4`、`byteRebuilds=2`、
`phisInserted=0`、`complete=true`、模块验证失败数 **0**。变换前后两份 IR 分别与
快检对应文件逐字节相同。执行前后库哈希相同。

原生最终产物为
`~/vmc-runs/guest-memory-irreducible-20261111/final-native-attempt001/`，
真实 IR 最终探针为同目录下的 `final-real-ir-attempt001/`。八份输入哈希与冻结输入一致，
最终输出 **8/8** 与快检新实现逐字节相同，下表的 `blocksVisited` 计数和零转发结论
全部重现。每份输出仍有 **1 个 RIP 入口 switch、2 个真实 Guest LOAD 目标 switch**：
后两者分别对应 CFF 和程序原有的 switch，不是两个已恢复的 CFF；表 LOAD 和 Transfer
default 都保留。这次没有重新执行四个 ELF，也没有得到新的完整函数去混淆收益。

可复现的 `run-final-native.sh`、`run-final-real-ir.sh`、
`summarize-final-real-ir.py`、输入/输出 IR、逐组统计、完整原生记录、动态链接证据和哈希
已打包保存：

- VM：`~/vmc-runs/guest-memory-irreducible-20261111/final-integrated-evidence.tar.gz`。
- 本地：`/Users/c/Desktop/deob/validation-artifacts/guest-memory-irreducible-20261111/final-integrated-evidence.tar.gz`。
- 两端 SHA-256 均为
  `1781e1d9f76483072ced08c1e1d8ab371eea53363e24a2d26b91d5e2cb74d717`。

本节只记录上述真正集成库的原生和 IR 复验；完整主/no-Z3 CTest 由主代理集中执行，
不把此前单文件快检记作最终构建，也不把 IR 验证记作真实 ELF 语义回放。

## 真实四 ELF 的覆盖计数

使用先前冻结的 4 个 Tigress ELF 的 static-region IR，各取 mem2reg 与
mem2reg→InstCombine→GVN 两种输入，同一输入分别调用旧库与新单文件实现的
`detail::runGuestMemorySSA(module, {65536, false})`。这里测量扫描覆盖及 IR 合法性，
没有重新执行 ELF 或声称新的去混淆收益。

| 输入 | 预处理 | 旧 blocksVisited | 新 blocksVisited |
| --- | --- | ---: | ---: |
| f-cff O0 | mem2reg | 17 | 384 |
| f-cff O0 | GVN | 15 | 399 |
| f-cff O2 | mem2reg | 22 | 167 |
| f-cff O2 | GVN | 21 | 169 |
| f-combo O0 | mem2reg | 46 | 2906 |
| f-combo O0 | GVN | 47 | 2918 |
| f-combo O2 | mem2reg | 30 | 426 |
| f-combo O2 | GVN | 43 | 435 |

八组均 `loadsForwarded=0`、`byteRebuilds=0`、`phisInserted=0`，模块验证通过。
旧/新输出 **8/8 字节相同**，原始输入 SHA-256 逐组一致。真实 Guest LOAD/STORE 通常
被成功检查分隔到不同 LLVM 块，本次保守本块补扫没有建立跨块关系。
真实的表 LOAD、目标 switch/default 及 RIP 入口 switch 均保留；没有消除真实 CFF。
探针不使用 Graph 或镜像读回调，也不假设表不可写。

可复现输入和 `guest-memory-ssa-probe.cpp`、`guest-memory-real-ir-probe.py`、
旧/新逐组 stdout/stderr、IR/hash 明细保留在上述 VM 产物目录；计数表的原始来源为
`real-ir-probe-results.json`。ELF 来源、真实 dispatcher 与 RIP 入口的区分见
[`same-function-cff-validation-20261111.md`](same-function-cff-validation-20261111.md)。
