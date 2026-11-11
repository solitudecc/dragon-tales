# Guest 地址与内存失效证明修补

基线 `d1a8467`。本阶段修改可选 GuestMemorySSA 的地址计算与写入屏障，保持 Graph、
IGNIL、Guest/C ABI 和默认优化顺序。实现里程碑为 `6f8c51b`；该提交已通过聚焦、完整
main/no-Z3 和独立原生矩阵，提交时上一轮组件及真实 ELF 的兼容性复验仍在进行。

## 具体问题及修复

| 问题 | 旧行为 | 当前处理 |
| --- | --- | --- |
| 窄位宽常量算术 | 例如 i8 的 200+100 被按宿主 u64 算成 300，实际为 44 | APInt 保留每个值的真实位宽及模算术 |
| GEP 符号扩展和缩放 | 负 i8 索引、typed GEP、截断/扩展可能得到错误地址键 | 最终索引先按 64 位 index 规则符号扩展/截断，再按固定分配大小缩放 |
| 不确定索引 | 危险标志、非法 shift 或 scalable size 可能被当成普通常量 | 保持动态地址身份；128 位值、256 节点、64 层之外也保持 Unknown |
| 未证明独立的写入 | 普通指针参数或已逃逸 alloca 被当作 local write | 只有显式 noalias 参数、证明未逃逸的私有 alloca 保留事实 |
| 两份 Guest base LOAD | 字节路径可能把不同 SSA base 当作独立对象 | Guest STORE 清除其它可能别名 base 的字节事实 |
| 原子及同步 | 普通 word/byte 路径可能跨 RMW、cmpxchg、fence 等保留旧值 | 未知写、非 simple 访问和同步清空相关事实 |
| 半地址空间距离 | 对 INT64_MIN 取负，触发宿主 C++ UB | 以 DataLayout 位宽进行双向无符号模距离判断 |

例如同一槽先写 7、再 atomic add 1、随后普通 LOAD，旧优化器可能仍给出 7。
另一个反例通过额外的普通 pointer 参数改写同一 Guest 字节；没有 noalias 证明时，
该写入必须影响后续 LOAD。合法的精确转发、显式 noalias 和私有 alloca 正例继续保留。

## 合同与证明边界

执行合同要求寄存器快照与 Guest 内存分离。本轮的有效别名反例使用额外参数或两次
加载的 Guest base，保持该对象分离。captured alloca 安装为 descriptor.data 的例子
属于未注解通用 LLVM IR 的分析控制，不能据此声称它是符合 V2 caller-owned 稳定映射
合同的机器执行错误。原生通用 IR 夹具在返回前恢复 descriptor，比较器还核对其 data/size。

固定地址键仍以 64 位字节偏移表示。pointer/index 布局不是 64/64 时，GEP 保留动态身份，
不按宿主全宽字节距离重建；scalable 类型大小不取最小 vscale 当常数。相同 SSA 指针的
精确匹配仍可使用。非零地址空间等更广模型没有在本轮证明。

局部常量计算达到宽度/深度/节点上限时返回 Unknown，没有增加公开预算字段；
`complete` 不能解释为每个地址都已得到常量证明。当前分析也没有把镜像字节、`.rodata`
或一次运行快照升级为不变性事实。

## 实际失败控制与最终验证

所有新回归使用已有 API，能对冻结旧库实际编译运行：

- 索引组：旧 **7 cases / 154 assertions，45 failures**；最终宽度源的组件快检
  **7 / 172** 全过，含 scalar/narrow/wide、非法标志、scalable 和非原生布局控制。
- 别名组：旧 **4 cases / 188 assertions，60 failures**；组件快检 **4 / 188** 全过。
- 实际旧生产翻译单元在 UBSan 下触发 INT64_MIN 取负；新生产翻译单元的 distance
  和 **100 组**模区间分析探针通过。高地址只建立 IR 并运行分析，未宿主解引用。

最终完整库中，main/no-Z3 新组均为 **11 cases / 360 assertions**，相关组件组均为
**81 cases / 2753 assertions**。全量 CTest 为 **916/916** 与 **781/781**，全部通过；
两个完整构建都包含实际 GuestMemorySSA 源码的 sanitizer target。

独立原生矩阵为 25 种模式、440 个固定输入，before/after 各以 O0/O2 编译，合计
**1760 次观测**。相同源码与模型对旧库得到 **400 次错误输出**：所有 before 都正确，
每种编译级别的 after 各 200 次预定义错误。旧报告 `semantic_ok=false`，成功捕获旧错
仅代表 failure control 有效，不能计作旧语义通过。

最终集成库的同一矩阵为 **1760/1760 匹配**，400 次旧错全部消除；64 个错误 IR
观测精确检出 **48/48** 指定错误，**5283** 个单字段/长度比较器控制通过。
逐次比较返回值、调用次数、完整 512 字节寄存器、64 KiB Guest 内存、64 字节 side
内存和七个 Exit 字段。未知写/同步模式保留原 LOAD，合法精度正例确实转发。
正常矩阵没有跳过、排除、归一化或只比较部分字段。

本机 native 域从一开始限定兼容的 DL64。单独的 DL32 pointer-return 微探针在 clang/
llc 下没有得到该布局的预期低位 wrap；原始不匹配及命令完整保留，不称为 LLVM bug，
不算 native 成功。DL32/p128/scalable/undef/poison 的合同由结构或 APInt 控制覆盖，
不执行非法地址或给非确定值伪造固定期望。

上一轮 checked-edge 矩阵复验 **2176 次正常比较零差异**，192 个错误 IR 观测精确检出
**88/88** 指定错误。四个冻结 ELF 的原顺序和 post-GVN 两种显式流水线各为
**C16/LLVM16 完整匹配**，寄存器/内存控制各 16 次、七字段 Exit 控制各 112 次检出。
重复运行同一批 ELF 不计为新增样本覆盖。

真实精度与冻结 `d1a8467` 一致：原顺序 Guest SSA 转发为 **0/0/1/0**，post-GVN
额外 SSA 为 **1/0/4/0**（f-cff O0/O2、f-combo O0/O2）。原顺序两个模块、post-GVN
四个模块继续显式报告分析 incomplete，完整根函数返回不代表分析预算已完成。
实际 CFF 与原程序 switch 仍各 **1→1**，表读取和默认出口保留。本轮没有扩大真实
去平坦化覆盖。

## 证据与范围

细节和各组件双端归档哈希见：

- [索引宽度证明](llvm-guest-address-proofs.md)。
- [别名及屏障证明](llvm-guest-memory-alias.md)。
- [独立原生验证器](guest-memory-address-validation.md)。

最终集成 runtime 在 VM `~/vmc-runs/guest-memory-address-proofs-20261111/final-runtime/`：

- backend：`aeb13989cb31557effcde48872f3f6b6c5b0938fc737f11457bb3ba925130937`。
- shim：`a623a1490d57b9df708194ef9b6a59eec47a5a9c1dc03c72f65e3e98ed4eae6c`。

聚焦、全量 CTest 与 sanitizer 最终日志包为该 VM 根目录下 `final-validation-logs.tar.gz`，
本地副本在 `/Users/c/Desktop/deob/validation-artifacts/guest-memory-address-proofs-20261111/`；
两端 SHA-256 为 `286c06d2f807d515c889a8efb95cea27546830db2f7e750afbdf28cdd1ac6703`。
独立原生和真实兼容验证包为 `final-evidence-v2.tar.gz`，双端哈希和完整路径见原生验证器文档。

本轮没有修改机器指令 lifter，也没有新增单指令/IGNIL 或 69 输入全语料库运行。
这些有限输入、完整字段与 sanitizer 检查不是任意程序或整个优化器的语义证明。
