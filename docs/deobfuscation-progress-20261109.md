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

## 后续组件

S1b 继续建立 LLVM 真实访问、成功检查和故障出口的一次性映射；S2 才实现 Guest 内存转发。
这些尚未完成的能力不包含在上述验证结果中。
