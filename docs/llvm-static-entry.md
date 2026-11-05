# 限定函数入口的静态分析视图

2026-11-05 开始，2026-11-06 验证完成；基线 `23f46f2`。这是显式选择的 LLVM 静态入口契约，
不执行目标程序，不改变反汇编、CFG 恢复、IGNIL、跳转目标解析或 Guest CALL/RET。

## 要解决的问题

原静态函数接受 `dragon.entrypoints` 中的所有 PC，包括内部基本块和调用后续。
因此，调用者可以绕过函数开头的赋值或守卫，优化器不能把这些操作当作所有路径的前提。
例如 `mov eax,7; jmp body; body: mov [rbx],eax; ret`，直接从 body 恢复时
必须允许存入任意 EAX；从函数起点进入时才能把它简化成存入 7。

新增视图只接受 `Function::startAddress()`。这里的“起点”指调用者选定的 Function，
不保证启发式函数边界本身正确。循环仍可回到该地址，内部跳转和 CALL 返回后的
续块不受外部入口限制。错误输入 PC 不执行任何 Guest 指令，返回现有
`Unsupported/None`、detail `Entry`，source/next PC 等于输入 PC、valid=1、return_pc=0。
Guest 寄存器和内存保留，Exit 被更新；没有用 assume/unreachable 排除错误输入。

## 接口与兼容性

- C++：先 `setExecutionMode(ExecutionMode::StaticAnalysis)`，再
  `setStaticEntryPolicy(StaticEntryPolicy::FunctionStart)`，然后 lift。
- C：`dragon_llvm_lifter_set_static_entry_policy(l, DRAGON_LLVM_ENTRY_FUNCTION_START)`。
- Python：`LLVMLifter(arch, execution_mode=LLVMExecutionMode.STATIC_ANALYSIS,
  static_entry_policy=LLVMStaticEntryPolicy.FUNCTION_START)`。

策略必须在空的静态 module 上选择。无效值、非静态模式、首次 lift 后修改均拒绝。
已选 FunctionStart 时不能切换到 Legacy/V2；空 module 可先恢复 Resumable 再切换。
默认仍是 Resumable，既有调用者不需要传入新参数。新组件不依赖 Z3。

新符号为 `dragon_static_entry_<arch>_<hex>`，入口属性仅列出函数起点，另有
`"dragon.static-entry-policy"="function-start"`。已确认内部函数的直接 CALL 使用
相同命名和契约；前向声明支持先调用者或先被调用者降级。外部未知调用仍是
`dragon_static_external`，没有增加内存、寄存器保存或返回行为属性。
四指针布局和 Exit reason 不变；这种静态函数仍不能注册成 V2 实现。

## 回归与语义验证

- 四项 C++ 回归共 242 条断言，涵盖三架构、两种反汇编策略、默认/限定入口、
  常量传播、策略拒绝及内部声明的两种定义顺序。
- 仅把测试加入旧基线后构建失败：`LLVMLifter` 不含 `setStaticEntryPolicy`，
  `StaticEntryPolicy` 未声明。新增 API 不能编译的例外按仓库规则单独记录。
- `static_entry_diff.py` 分两次运行 42 个三架构核心夹具和四个 ELF/PE 导入镜像，
  覆盖两种入口视图及 product O0/O2：合计 3776 次比较匹配，1520 次有效入口比较、
  2256 次拒绝入口检查。有效比较中 1040 次完整根返回、480 次既定边界；
  导入镜像的 128 次有效比较全部是边界，不能算完整函数成功。
  包含循环、递归、CALL/RET、间接调用、内部声明和真实内部 PC 恢复。
  比较全部已建模字段及全部 64 KiB，没有新增排除、跳过或 Legacy 归一化。
  导入镜像还核对新函数符号对应的 import 来源元数据。
- 对冻结旧双共享库的三个常量写入夹具，同一 336 次比较有 288 匹配、48 个
  预期入口契约差异，六个模块不具备限定入口契约。144 次合法入口比较仍全部匹配；
  差异来自旧模式接受内部 PC，不能称作旧指令执行算错。
- 返回寄存器和内存负对照各在 84 次比较中检测到唯一被破坏的字段。
  入口负对照故意以 Resumable 替代限定视图，12 次内部入口检查均抓出差异，
  其余 36 次合法入口检查保持匹配。
- CTest 573/573；无 Z3 449/449。
- 冻结的无 Z3 库与对应 C/Python 接口另做三架构原生回放：84 次比较全部匹配，
  其中 36 次完整根返回、48 次错误入口拒绝；没有新增排除或跳过。

## 真实样本与限制

公共 Tigress 2.1 revision 2128，seed 4242，f-mba/f-opaque/f-combo、C O0/O2、
两种反汇编策略、product O0/O2、每镜像 24 个输入：288 次 C 规格检查及
576 次 LLVM 完整根返回匹配，CFG wrong、unlifted、incomplete 均为零。
真实 Guest megaInit 在参考和被测程序中执行，没有把运行快照或正确堆字节注入分析。

单 kernel 测量使用上一轮保留的同一批 ELF，两个策略结果相同。以下指令计数包含
生成的 Guest 状态、内存检查及退出代码，不是原始机器指令数：

| 样本 | 默认产品管线 Resumable → FunctionStart | 再经 LLVM O2 Resumable → FunctionStart |
|---|---:|---:|
| f-mba O0 | 4702 → 3603 | 3486 → 4112 |
| f-mba O2 | 3339 → 2746 | 2366 → 1837 |
| f-opaque O0 | 9134 → 5395 | 8871 → 5433 |
| f-opaque O2 | 4472 → 3457 | 4306 → 3372 |
| f-combo O0 | 18497 → 15613 | 17165 → 13377 |
| f-combo O2 | 11577 → 8882 | 11356 → 8244 |

f-mba O0 的 LLVM O2 输出反而增大；额外优化有自己的启发式，不能保证所有配置
都变小。这些减少也不能全部算作原程序不透明谓词被消除。

两套真实语料库完成 69/69 个输入、138 种策略、206649 个已恢复有效函数检查点，
raw/optimized 各 6530 个静态模块 verify 通过。每个模块优化前后均核对函数符号、
限定入口属性和唯一入口地址，覆盖全部已恢复有效函数，提升失败及覆盖遗漏为零。
全部输入哈希与旧基线一致，已记录的 CFG、函数划分和 import 引用数量等结构字段
也完全相同，包括原有异常。已验证的输入结果经审计复用，没有把未执行的检查算作成功。
这是 IR 合法性及入口契约检查，不是整个语料库的语义证明。

入口限定只提供“先经过函数起点”这一事实，不提供 megaInit 成功、堆对象内容、
指针不为空、malloc 成功、未知调用效果等信息。它没有实现调用摘要、有限值集解析、
去平坦化或二进制重建。堆链表谓词仍须后续初始化和内存模型；不能把 IR 缩小
全部归因于消除了不透明谓词。

本轮未修改指令语义、IGNIL 优化器或反汇编器，未另跑单指令和 IGNIL 基本块全量差分。
函数级语义覆盖来自上述 Unicorn 参考及负对照；语料库 IR verify 不能代替全程序语义证明。

VM 产物目录：`~/vmc-runs/llvm-static-entry-20261105/`。保留旧/新双共享库、
旧版编译失败、完整输入输出、原生目标、负对照、Tigress 及逐输入语料结果。
`final-summary.json` 汇总语义、边界、负对照和完整语料数字；`baseline/`、`final/`
及 `final-noz3/` 各自保存双共享库、C/Python 接口与 Execution.h 布局。
回放时同时设置同一快照的 `DRAGON_TEST_PYTHON=<snapshot>/python`、
`DRAGON_C_LIB=<snapshot>/libdragon_c.so` 和 `LD_LIBRARY_PATH=<snapshot>`。
