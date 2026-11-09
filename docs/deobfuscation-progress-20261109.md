# 去混淆实施进度

实施依据：[阶段方案](deobfuscation-implementation-plan-20261109.md)。本记录按实际完成的组件更新，
不把验证工具恢复计作新去混淆算法，也不把尚未完成的 S1/S2 标为通过。

## S0 验证工具与完整性门禁

基线 `12b0043`。新增 `campaign_diagnostics.py`，用于 Tigress challenge 和静态区域 replay。

- 只在探测到 `dragon_symexec_create` 缺失时，将分支诊断标为 `unavailable`。未执行的诊断计数
  为 null；完整 C、动态 CFG 和 LLVM 原生状态/内存比较照常执行。
- `--require-diagnostics` 要求该诊断也完整。语义完整与诊断完整分别报告，可选诊断没有被算作证明成功。
- 可用诊断抛出的异常先保存为 `diagnostic_error`，随后原样抛出；不会把实现错误当成能力缺失。
- 最终汇总逐项核对请求的 case、plan、trial，拒绝缺失、重复、额外及非法类型记录。
  构建失败、参考失败及 incomplete 不能被其他已匹配用例掩盖。bool/float 不能冒充整数编号或计数。

### 回归和真实旧版失败

13 项纯 Python 测试覆盖能力缺失、真实异常、空诊断、非法站点图、严格模式、缺用例/计划/输入、
重复行、CFG 错误、错误类型及语义失败。该新增辅助模块在旧源码中不存在；旧行为的失败证据来自
下面直接运行旧脚本的控制，而不是把无法导入新模块当作旧版执行错误。
完整 CTest：主构建 **801/801**、无 Z3 **669/669**，包含新增的 13 项辅助测试（一个 CTest 入口）。

1. 同一无 Z3 库、冻结 f-mba ELF，旧脚本在 C/Unicorn/native 检查前因缺失符号返回 1。
2. 仅对准确匹配冻结 f-combo ELF 的构建过程注入失败，保留 f-mba 的 C8、LLVM16 完整匹配。
   旧脚本错误返回 0；新脚本返回 1，明确记录 build_error 与缺失比较。这是独立的报告门禁修复。
3. 对已可用诊断分别注入 AttributeError、RuntimeError，整个新脚本均返回 1，原异常类型/marker
   保留，结果记录为 error；C/LLVM 尚未执行，没有伪装成 unavailable 或成功比较。

### 最终冻结输入验证

两份原有 C O0 ELF，线性策略、FunctionStart、product O0/O2、每 ELF 八组输入。每次比较全部
模型状态、1 MiB 内存及原 Exit 协议，运行真实 Guest 初始化，没有注入初始化后的堆快照。

| 最终组 | C | LLVM | 返回码与诊断 |
|---|---:|---:|---|
| 主构建两 ELF | 16 匹配 | 32 完整根返回匹配 | 0；两份分支诊断完整 |
| 无 Z3 两 ELF | 16 匹配 | 32 完整根返回匹配 | 0；语义完整、两份分支诊断 unavailable |
| 无 Z3 返回寄存器负对照，f-mba | 8 匹配 | 16 次指定 RAX 单字段差异 | 1；每次检出指定差异 |
| 无 Z3 内存负对照，f-mba | 8 匹配 | 16 次指定内存单字段差异 | 1；每次检出指定差异 |
| 无 Z3 严格诊断，f-mba | 8 匹配 | 16 完整根返回匹配 | 1；语义完整、诊断不可用 |

最终正常组覆盖矩阵的 missing/unexpected/duplicates/invalid 全为 0；Graph 保持不变，CFG wrong、
unlifted、executed-undecoded、语义差异及 incomplete 均为 0。控制与正常结果分别统计，重复参考
输入不算新增独立样本。第一版校验器的中间队列只作历史，未与最终源码覆盖混合累计。

S0 没有修改四个生产库或 Guest 比较器，没有生产构建，不新增字段排除、跳过或归一化。
这里的语义匹配是既有区域能力的回归；当前真实样本的去平坦化仍未完成。

### 证据定位

VM `/home/cc/vmc-runs/deobf-s0-20261109/`；Mac
`/Users/c/Desktop/deob/validation-artifacts/deobf-s0-20261109/`。

最终入口 `serial-validation-final-v1/freeze.json`，SHA256
`438aad647d02d756515f8ad19bed2632a45fbf4cca804130dd34bea3ffb1837d`。
其 summary、源/库 pins、原始 argv/退出码和审计记录双端核验；851 份 raw 文件保留在 VM。
旧无 Z3 失败为根目录 `old-noz3.*`；完整 CTest 日志为根目录 `build[-noz3]-ctest.*`。

## S1a 区域冻结与操作来源

基线 `ced8210`。将 `liftStaticRegion` 中的区域收集、私有切分、机器语义重建及预算检查抽到内部
`LLVMPreparedRegion`，提升与后续分析可以使用同一个拥有精确内容的对象。

- Graph 在固定堆地址持有，以 const 方式提供；移动 PreparedRegion 后，其中的 Function/BasicBlock
  视图继续指向同一份图。源 Graph 改动或销毁不会改变已准备的内容。
- 严格 IGNIL 与机器重建路径都使用快照；前者保留调用者改写和 unlifted markers，后者先登记所有
  起点再重建每条路径及旧前缀。原 Function 成员和额外覆盖集合分别保留。
- OpOrigin 以准备实例、块地址及 op 序号区分操作；机器 PC 仅作诊断，同 PC 的两个操作不是同一
  来源。即使图内容相同，不同准备实例也不能混用来源。来源身份不表示跨循环迭代的同一内存快照。
- 普通 `lift()` 入口保持原路径；区域视图的入口策略、ABI、默认退出与未知语义规则保持原契约。
  严格区域路径新增整图快照成本；现有块/case 上限仍不是整图 CPU/内存硬上限。

内部 `prepare()` 只准备内容，不验证全部 IGNIL 可降低性，也不单独保证 LLVM 模式或 FunctionStart。
这些条件仍由正式 lifter 入口检查。其原 Graph 后继不能冒充区域运行时 switch 的全部操作 CFG；
本阶段没有用这些后继授权值替换或删边。

新增四个 C++ 用例、223 条断言；主构建及无 Z3 的完整区域组均为 **14 项 / 1103 条断言**，
完整 CTest 分别 **805/805**、**673/673**。覆盖快照所有权/移动、实例隔离、同 PC 多 op、严格与
重建差别、源 marker 保留、块内切分/前缀重建、非法位点/预算、外来 Function 和失败后重试。
既有十项区域测试继续覆盖缺 MCInst、缺 IGNIL、空预登记、晚失败及前向声明恢复。

新内部头文件在旧代码不存在，tests-only 构建的真实编译失败已保留；不是旧版运行语义错误。
本阶段不增加优化收益，before/after 应保持同范围的原始 IR 和状态行为。

三架构原生对拍：主构建与无 Z3 各 240 次匹配，分别为 96 次完整根返回、48 次区域外 Transfer、
96 次内部 PC 拒绝；全部模型字段、64 KiB 内存及原 Exit 协议一致。主构建结果与内存负对照各检出
240 个指定单字段差异，使用相同三架构选择器，未与正常根返回合并计数。

两份冻结 C O0 ELF 的线性/product O0/O2/八输入矩阵：主构建与无 Z3 各 C16、LLVM32 完整根
返回匹配，零 incomplete、错误和差异，Graph 保持不变。无 Z3 分支诊断为 unavailable，语义检查完整。
与 S0 对应的 36 份 IR、16 份 Graph 比较：48 份整文件字节一致；另外四份 optimized.ll 仅确切
raw.ll 输入路径的 ModuleID 首行不同，其余字节完全一致。未修改或归一化原件，没有新增排除或跳过。

构建/测试及源码库 pins 位于 VM `~/vmc-runs/llvm-prepared-region-20261109/`，Mac 同名
`validation-artifacts` 目录。实际构建来源为 `ced8210 + patch`，后续同步 Git HEAD 不算再次构建。

## S1b 一次性 LLVM 位点映射

2026-11-09–10，基线 `9ff29d8`。新增内部 `LLVMLiftProvenance` 和 `RegionLowering`，在同一次
同步调用中完成原始 lowering、只读消费及失效。正式区域接口也使用此捕获路径；普通 `lift()`
使用空 capture 参数。没有新增 lifter 实例字段、C/Python API、Guest ABI 或 IGNIL 操作码。

- 操作、操作数及结果按原始 block/opIndex 记录，Unsupported 前缀使用原序列 span，不通过 PC
  或复制后的地址找回序号。同一 LLVM 常量可以对应多个不同来源。
- LOAD/STORE 记录实际指令、地址/归一化 index、数据、真实字节宽度、检查的成功/失败边和访问
  完成后的位置。寄存器 alloca、快照、size 和 Exit 访问不计作 Guest 数据访问。保留原 STORE
  参数求值表达式，不重排或重复 resolve。
- x86 CALL 返回槽与 RET 目标读取单列角色，宽度取实际 2/4/8 字节；A64 LR 操作不伪造成内存
  访问。CALL 记录真实 target 快照、请求、合法性检查、正常继续与传播边；请求不是返回效果，
  传播 callee Exit 的直接 Return 不伪造已知退出字段。
- switch 的 case/default 及各后继索引来自实际 terminator；Entry 拒绝、零 IR marker、流末尾
  使用无 OpOrigin 的边界来源。记录有效的七字段 Exit，包括 UnknownTarget 调整后的 reason；
  测试独立读取对应 LLVM STORE，逐项核对 Exit 和调用请求。

`LiftView` 是弱持有的同步借用。回调返回或抛异常后，保留的 view 查询会在接触 LLVM 指针前抛错。
回调中得到的引用也不得逃逸；消费者是可信只读代码，不允许改写/优化/关闭目标，不提供插件沙箱。
结构 seal 仅检查操作归属、访问成功边及全部实际 terminator 的唯一覆盖，不是外部证书验证器，
也不证明内存别名、表内容或值等价。本阶段不授权值替换或删边。

每函数记录上限为 1048576，可在内部下调，计入操作、操作数/结果、访问、边、退出和调用。
超限或 lowering 失败不调用消费者，沿用 failedLift 的函数体清理及前向声明恢复。该上限不是整个
IR 生成过程的 CPU/RAM 硬限额。消费者抛异常会使 view 失效并传播；已完成的 definition 保留，
调用方须丢弃失败目标，不宣称模块自动回滚。

### 回归及旧版证据

新增 **8 项 / 3725 条断言**；主/无 Z3 的区域与映射组均为 **22 项 / 4828 条断言**，完整
CTest 为 **813/813**、**681/681**。覆盖同 PC 多操作/向量双半、实际 GEP/宽度/访问后位点、
CALL SP/BLR X30、i386 word CALL/RET、RET imm、调用出口、switch/default、局部循环、
Unsupported/stream-end、预算、失败后重试及正常/异常消费失效。
有/无只读 callback 的对比仅证明观察者不改变产物，独立旧库证据如下。

最终测试单独放入冻结 `9ff29d8` 源码，编译因缺少新内部头文件失败；原错误和源码哈希保留。
这不是旧版机器执行错误，未把 API 缺失记作语义差异。

### 最终语义与结构兼容

- 三架构 native 主/无 Z3 各 240 匹配：96 根返回、48 Transfer、96 拒绝内部 PC。主构建结果/
  内存负对照各检出 240 个指定单字段差异，仍比对全部模型字段、64 KiB 和原 Exit。
- 两份冻结 C O0 ELF，线性、product O0/O2、八输入：主/无 Z3 各 C16、LLVM32 完整根返回匹配，
  Graph 不变，错误、差异、incomplete、unlifted、executed-undecoded 均零。全字段、1 MiB、
  真实 Guest 初始化及原比较器不变，无新增排除、跳过或归一化。
- 普通 lift 旧/新主库各 C16 匹配、32 incomplete、零完整根返回，返回码均为 1。32 对七字段
  Exit 逐项相同，均为 Transfer/Branch；这些边界不是完整语义成功。
- 区域与普通 lift 比较 78 份 IR/Graph：72 份整文件逐字节相同，六份 optimized.ll 仅确切
  raw.ll 路径的 ModuleID 首行不同，其余字节完全一致，没有改写原件。
- 独立公共 raw probe 固定 14 个机器码 case、两策略、五种接口/模式，旧/新各 140 次尝试：
  122 接受、18 False 拒绝、零异常；全部状态和 140 份输出字节相同，Graph 不变。该探针仅作
  结构兼容，零 CPU/native/优化/verify，不计作语义匹配。

阶段编排器最初把 Transfer 错写为枚举 3，审计旧普通队列时停止；正式 Execution.h 定义
Transfer=2、NeedTarget=3。原脚本/失败保留，v2 按正式协议修正，鲜核已完成七组的哈希、退出码
和原结果，仅执行剩余新普通队列一次。生产比较器、输入及字段不变，未重跑已过七组，也未把
编排错误记作产品差异或语义负对照。

证据在 VM `~/vmc-runs/llvm-lift-provenance-20261109/` 及 Mac 同名 validation-artifacts。
总入口 `components-ready-v1.json`；八组汇总 `ordinary-continuation-v2/final-summary.json`，SHA256
`0e099d80f9a8d87a28de1e8265e1df43546f02497557f09904f28d162ba4a1b5`；公共探针入口为
`raw-ir-probe-v1/freeze-v1.json`，SHA256
`898d35ddbc0cfe94516971683be7c41837c9b980601009285bbf1d588feabc47`。
实际构建为 `9ff29d8 + patch`，后续 Git 同步不算再次构建。原始日志保留，没有新增单指令或
69 输入全语料活动；这些记录不构成任意程序/全输入语义证明。

## 后续组件

S1 的区域与一次性位点基础已就绪。S2 下一步实现保留 Guest 写入和故障出口的内存值转发，随后
扩展栈槽、跨块/循环 PHI。S2 应在同一 lowering 调用中消费并生成计划，再在借用失效后应用；不能
从已返回的普通 lift 上找回旧映射。S1a/S1b 没有实现这些值改写或新增实际去平坦化收益。
