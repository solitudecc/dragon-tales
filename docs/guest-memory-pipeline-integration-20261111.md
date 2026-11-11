# Guest 内存流水线接入与循环闭包修复

本轮从 `ca245bc` 继续对齐 raw/post 分析。已完成显式内建入口
`guest-memory`（`a7ed9bd`）和嵌套自然循环成员修复（`ee041f9`）。
这使调用者能在寄存器和地址规范化之后，按有界配方重新分析当前 LLVM IR 的内存关系。
默认六步 `optimize()`、机器指令语义、Guest ABI 和未知调用屏障均未改变。

## 实现与证明边界

内建入口复用现有 GuestMemorySSA，传递全部 19 个原生统计字段，保留
Unavailable/Incomplete。五个参数采用流水线既有 uint32 范围；标志仅接受整数 0/1。
无 Graph 入口拒绝需要 Graph 的叶函数证明参数。已有插件名称 `guest-memory-ssa`
仍可注册。`max_rewrites=0` 不禁止显式 mem2reg，流水线的 `changed` 依据实际 IR，
不与内存算法自身的改写计数混淆。示例见 [流水线文档](pass-pipeline.md)。

循环修复在普通事实和字节事实两处，为每条回边独立求前驱闭包，再合并循环成员。
旧实现把总成员集合当作本次遍历的 visited，内循环先被访问时可能截断外循环，遗漏
外循环块。修复恢复既定的延迟改写及字节事实屏障，不增加循环证明或别名假设。
根因及结构夹具见 [顺序与循环成员诊断](llvm-memory-pipeline-order.md)。

当前 GuestSSA 每次从当前 LLVM IR 重新求事实，不重用已经被通用 Pass 失效的
S1b 位点指针或旧证书。三轮配方每次调用重新取得各 Pass 的选项预算，并受总调用
次数限制；这不同于原设计中“所有轮共享一个求解额度”的尚未实现契约。
每轮预算重置已在公开流水线文档中说明，不能称为无界不动点求解。

## 实际验证

| 验证 | 结果 |
|---|---|
| 内建入口最终 10 个测试，旧 B244 | 10 cases 失败，97 assertions 中 25 失败 |
| 内建入口最终 10 个测试，新版 | 10 cases / 195 assertions 全通过 |
| 循环成员最终 3 个测试，旧 B244 | 2 cases 失败，1018 assertions 中 432 失败 |
| 相关组件，main / no-Z3 | 各 82 cases / 3516 assertions 全通过 |
| 最终 main CTest | 1013/1013，105.23 秒 |
| 最终 no-Z3 CTest | 878/878，90.19 秒 |
| 嵌套循环独立原生 oracle | 旧、新各 9216 完整状态匹配，各 512/512 指定错误检出 |
| 本地与 VM 构建源码绑定 | 387 文件 SHA 全相同 |

旧测试源可编译，失败来自执行断言，不是缺失新 API。初版 9 个入口测试、两例循环
测试及初次完整测试均单独保留；初次完整测试的唯一失败是 Python builtin 名称集合
未加入新入口，修正精确集合后复跑，而非放宽检查。

原生 oracle 比较返回值、全部 512 字节寄存器、64 KiB Guest、七个 Exit 字段；
旧版也全匹配，不能把循环成员结构失败写成已经证明的旧版执行错值。
字节路径新增测试是结构验证，不计入该原生矩阵。详细覆盖及错误对照见
[独立循环验证](nested-loop-independent-validation-20261111.md)。

真实四 ELF 的逐轮实际 LOAD 索引、完整 C/LLVM 比较、严格统计失败和最终修复版探针
分别记录在 [有界流水线验证](bounded-memory-pipeline-validation-20261111.md)。
历史 raw 精度失败和中间版本的严格计数等价失败仍保留，不能由新配方的成功倒改。

## 运行库与证据

主构建最终 backend：
`2e509ceb03bc5b47d2e95fdacfd579860e520f238ee239746c273b6034ba9bda`；
shim：`e2e9ae122d8f31aac4f2376496cf4e1c1455360f09daddf294b996e86e58b3ec`。

无 Z3 最终 backend：
`c1918077e3e5f27a901aa2b8410e5933ba99323c824578f9571f210eb2b828ed`；
shim：`d4c4319f2741a347a16e5bed8d1a30ccf85c52cd2f526e5a6efc9c015d846772`。

根验证目录：`~/vmc-runs/guest-memory-pipeline-root-20261111/`。
旧 B244 完整库来自此前 `store-hub-root-20261111-artifacts.tar.gz`
（SHA `bd0e59318ea991d019c87fd47b0b5e4eb1c6821b736bdd591c07a2d695fd8105`）。
中间 9e7 库压缩后在 VM/Mac 解压哈希复核，保留在根目录 `runtime/`；
最终库与 no-Z3 压缩副本分别在 `runtime-loop-fixed/`、`runtime-loop-fixed-noz3/`。

根产物完整归档为 `~/vmc-runs/guest-memory-pipeline-root-20261111-artifacts.tar.gz`，
SHA `f67b28fb07ebee7ab219a1813ae3fd3d35483327e9bad8b2c6cb3d5ae3e6bd1a`，
183793378 字节、89 个文件（88 个数据文件及清单）。VM/Mac 分别验证精确文件集合、
文件长度与 SHA，并重新解压检查四份压缩运行库的原始 SHA。
Mac 副本位于 `/Users/c/Desktop/deob/validation-artifacts/guest-memory-pipeline-20261111/final/`。
双端验证后退役根展开目录中的重复库文件；上述库路径是归档内相对路径，恢复说明
保留在展开目录。测试日志、源码绑定和原失败记录没有删改。

本轮不声称一般循环不动点、任意跨调用内存推断、不可变跳转表或完整 CFF 消除。
没有新跑 69 输入全语料或单指令 campaign；本轮改变 LLVM 分析和调度，验证覆盖为
对应组件、完整 CTest、独立循环模型及冻结真实函数重放。剩余工作仍是更多逐边状态/
地址关系，以及表内容的写入来源或独立入口契约证明。
