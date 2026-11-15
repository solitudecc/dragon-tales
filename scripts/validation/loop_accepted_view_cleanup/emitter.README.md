# 已闭合分析视图清理 emitter：准备稿 v1

**历史准备稿状态（首次冻结时）：只写源码和静态检查，未编译、未执行 pass、未执行 Guest。后续机械自测、独立构建和一次真实B清理已完成，实测结果见 `mechanical-and-cleanup-validation.md`；本说明的源码/recipe/范围合同不变。** 文件不注册 CMake，不增加公共 API，不改变产品默认 pipeline。实际使用须等生产 B 闭合并完成封存，由 root 给出明确窗口及所有真实 pin。

源码 `loop_accepted_view_cleanup.cpp` 独立解析已封存的生产 `.ll`。只对显式 `view_symbol` 的 Function 跑一轮固定 recipe；不优化普通 root 或 caller。不对 hypothetical table CFG 试优化。

## 固定 recipe

原序来自 `lib/LLVMLifter.cpp::runControlFlowSimplification`：

1. `SCCPPass()`
2. `DFAJumpThreadingPass()`
3. `JumpThreadingPass()`
4. `SimplifyCFGPass()`
5. `InstCombinePass()`
6. `DCEPass()`

没有额外前置阶段、重复轮次、可选 pass、默认 O2、阈值提升或“尝试到通过”的分支。四类 AnalysisManager 和 PassBuilder 正规注册、交叉代理注册；每阶段只调用 `FunctionPassManager::run(selectedView,fam)`。不调用会遍历整个 Module 的现有公共 runFPM 包装器。

## 输入和执行守门

接口为 `emitter ABS-READY-PLAN.json ABS-NEW-OUTPUT`。输出目录必须原本不存在；任何失败保留独立 REPORT 与可打印的 rejected.ll，不覆盖源或历史树。

所有 pin 都是绝对路径、精确字节长度和完整小写 SHA256。实际读入内容也再核 SHA，结束重核全部 pin 和 plan。plan 必须显式 `ready=true`、`sealed=true`、指定唯一 recipe 与编译 LLVM 版本；例子默认 false，不能拿来执行。

实际 producer REPORT 必须精确匹配 schema 和唯一 `stages[].case`，顶层 ok=true；该 stage 要 verified/closed/changed=true、budget_exhausted=false、ordinary_bodies_and_identity_unchanged=true、guard_miss_calls_ordinary_root=true，并且其 after_sha256 等于本次原始 `.ll` 文件 SHA。拒绝 hypothetical schema。整个 Module 的 `dragon.guarded-entry` manifest 只用于确认所选函数确是 view、普通 root 另有其人；metadata/report 不作为 LLVM 常量、noalias、不可变内存或 definedness 事实。

封存门要求外部已经生成、已经 pin 的 `exact-fileset-v1` FILESET 及真实 seal status。检查成员名/计数/总字节一致；source、producer REPORT、seal status 三者必须是该 ledger 下唯一、路径/size/SHA 全匹配的成员，禁止 `..` 逃出根。seal status 的显式字段 `no_more_writes` 或 `no_more_writes_after_fileset` 必须 true。工具不会自行创建这个前置 ledger/status 来给自己授权，也不会仅靠 plan 的 sealed=true 忽略它。

这只是复用外部完整封存证据的 readiness/provenance 门。本 emitter **不重新校验 ledger 内所有其它大 payload，不代替双端 archive/完整成员核验**。已封存文件的实际 schema 若与此草稿不同，先依据真实字段写另版并审核，不加默默跳过或“尽量解析”后备路径。

## 解析与字节记录

只用已存在的 AsmParser `parseAssemblyString`，不使用 IRReader，不给 SDK 增加 LLVM 链接组件。保存原始文件 `00-source.ll` 的精确字节和 SHA，再保存 parser 实际生成的 `00-parsed.ll`、实际 ModuleID 和 SHA。ModuleID 是 parser 的真实结果；不 strip、不 set、不补偿它来伪造 raw/parsed 相等。raw_first_line 和 raw_and_parsed_byte_equal 如实记录。

后续逐步比较以这个实际 parsed Module 为基线，不声称它与 raw 源字节相等。外部 source pin 和实际 closed producer after_sha256 始终绑定原文件完整字节。普通函数和全局状态的前后比较均在同一个 parsed Module 中进行。

## 限定范围和逐阶段证据

源和每一步都跑 whole-Module verify。源 view 的 EH、debug、blockaddress 或局部 metadata 身份在此验证工具中保守拒绝，避免 bodyless snapshot 隐藏全局跨局部身份变化；不因此修改产品的接受合同。

- 固定全部 Function 集合、顺序及 Function/Argument 对象身份，view 对象和参数身份也必须保留。
- 所有其它 Function（包括 root/caller/声明）全文必须字节相等。
- 在独立 Module clone 上只删除 view body，形成 bodyless skeleton。它保留所有声明/接口、global/alias/ifunc、attributes、inline asm、module flags 和 named metadata；逐步全文比较。snapshot clone 本身还要证明没有更改实际 Module。意外新增 intrinsic 声明也会拒绝，不能默默允许全局变化。
- 各阶段保存完整 `.ll` 和 Blocks/Instructions/Switches/实际 successor slots（含重复边）/全 CFG SCC，以及 reachable 子集。SCC 在所有基本块上建立，不能把不可达块未遍历当不存在。
- Lookup 是 readiness plan 按初始 block/instruction ordinal 和 LOAD/terminator 文本 SHA 指定的真实原 simple i64 LOAD。这里没有从预测 metadata 猜目标，也不以名称继续冒认一个已经删除的指令。
- `WeakVH` 跟踪原 LOAD、父块、terminator 和原 successor 身份；删除如实标记。原 LOAD 仍存活时，记录其实际父块、新 terminator、每个 successor slot 和当前 ordinal。原父块仍存活也单独记录当前切点。不能解引用旧悬空指针。

`after.ll` 只在六步都完成、scope/verify/input/runtime 复核都通过后写入。中途失败保留已完成阶段和 rejected.ll，不能把部分输出叫成功清理。

## 结构指标的含义

`zero_reachable_cyclic_switches_observed` 和原 lookup 的 SCC 状态只是实际观测。业务 switch、条件分支式中心分发、SimplifyCFG 改写 switch 等都需要审阅；不能把计数0、换个 terminator 名称或原 LOAD 消失单独当完整去平坦化。

REPORT 固定 `deobfuscation_complete=false`、`semantic_validation=false`、Guest_native_runs0、Unicorn_runs0。实际清理成功仍需真实结构检查和独立执行对照，不能从本 draft 得出结果。外部 ready 不能把闭合 report 或视图范围转换为无条件 root 净化。

## ready plan 字段示例（故意不可执行）

```json
{
  "schema": "dragon-accepted-view-cleanup-plan-v1",
  "ready": false,
  "sealed": false,
  "recipe": "existing-control-flow-six-v1",
  "LLVM_version": "21.1.8",
  "view_symbol": "REQUIRED_EXACT_VIEW_SYMBOL",
  "case": "B-SP-table-RSI",
  "closed_report_schema": "dragon-tigress-dispatch-context-probe-v1",
  "source": {"path":"/ABS/SEALED/B.after.ll","size":0,"sha256":"REQUIRED"},
  "closed_report": {"path":"/ABS/SEALED/REPORT.json","size":0,"sha256":"REQUIRED"},
  "seal_ledger": {"path":"/ABS/SEALED/FILESET.json","size":0,"sha256":"REQUIRED"},
  "seal_status": {"path":"/ABS/SEALED/SUMMARY.json","size":0,"sha256":"REQUIRED"},
  "seal_root": "/ABS/SEALED",
  "source_member": "B.after.ll",
  "closed_report_member": "REPORT.json",
  "seal_status_member": "SUMMARY.json",
  "seal_status_key": "no_more_writes_after_fileset",
  "emitter_source": {"path":"/ABS/OWNED/source/loop_accepted_view_cleanup.cpp","size":0,"sha256":"REQUIRED"},
  "runtime": {"path":"/ABS/PINNED-SDK/lib/libdragon.so","size":0,"sha256":"REQUIRED"},
  "solver": null,
  "lookups": [
    {"block_ordinal":0,"instruction_ordinal":0,"load_ir_sha256":"REQUIRED","terminator_ir_sha256":"REQUIRED"}
  ]
}
```

main 配置 solver 应改为实际 Z3 pin 对象；只有实际 No-Z3 才可为 null。当前 Linux draft 会读取自己 `/proc/self/maps`，要求只映射 pin 的单一 libdragon.so，solver 与 null/对象匹配，并拒绝共享 libLLVM。其它宿主环境未实现此绑定门，不能声称可执行。

Lookup inventory 只指定观测对象，不是证明事实，也不保证枚举已覆盖所有分发器。不能只挑成功消失的一条 lookup 报完成；所有 reachable cyclic-switch 指标、完整当前 CFG 与业务边仍要审。

## 未来编译/运行约束（本任务未执行）

只在 root 单独授权窗口构建。LLVM21.1.8 headers、g++14、emitter 源、最终 executable、固定 SDK 和实际 solver 都要在外层 controller pin。object/executable 放 owned RAM namespace，日志/输出新目录。链接使用该 object 和固定实际 SDK 作为 LLVM provider，沿既有方法给正确 rpath/rpath-link；**不附加 private implementation object，不再叠加 `-lLLVM-21`/共享 LLVM**。这只是未来方式说明，没有在本准备阶段运行任何编译命令。

运行沿 root 既定180秒、启动512MiB/运行64MiB守卫与 CPU 协调；不变更 proof/solver/default DFA 阈值。首轮必须保所有失败（包含 parse/链接/scope/verify）再判断，不能调整 recipe 或源输入凑结果。首个真实执行之前还须核验：未ready、未sealed、closed=false、错误 SHA/lookup pin 均在 passes_attempted0 拒绝；其它函数/全局更改的检出与独立 native 对照由后续独立证据验证，当前没有这些计数。

## 原读取库存的实测标注

实际B计划采用419条全部simple i64 LOAD的LLVM完整枚举，作为原读取身份观测的超集，不是419条T/跳转表读取。前两次尝试严格地址分类只找69条unused读取+80条guard读取，未能覆盖4条动态地址，已拒绝并保留为历史失败；没有把149条当73条成功清单。源中历史字段名lookups/original_lookup_cutpoints沿用不改，其数量变化只能称全部simple i64读取的存活/删除，不能据此把74条删除声明为74个T读取。实际exact producer字段、source7147、固定emitter0ffa和419清单见bit负责的input/inventory记录；本目录不为清单增加数值事实。

本目录的run_toy_v1.py、build_emitter_v1.py、run_actual_B_v1.py是实际执行controller的原样记录，含已封历史绝对路径，不是可直接对现有输出重跑的通用命令。使用新命名空间和新pin须另封新计划；不得覆盖旧树或盲重跑实际六步。
