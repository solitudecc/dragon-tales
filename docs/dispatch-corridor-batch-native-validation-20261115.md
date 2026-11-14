# 批量走廊：独立原生验证准备

这是新验证域的准备与执行记录。下方先保留执行前计划，实际结果单列在文末。
旧 C、LLVM、UC 的次数均不加入本次计数。候选枚举不是状态、内存或 definedness 证书。

`scripts/validation/dispatch_corridor_batch_native_source.py` 独立给出 structured C、Python
模型与 LLVM 输入。source427 首版已保留；sourceed673 只去掉一个重复 comparator offset
（36864 与别名 RegBytes+48），27 个不同字节的覆盖保持完整，未删输入或失败。

两个源各有固定 8 个 arena：

| 源 | 计划 | 特别检查 |
| --- | --- | --- |
| seventy-state-history | 70 个状态，seed0 两条真实 slot，共 71 条原候选 | 正常输入迭代75–78次覆盖全部状态；历史 counter、共享 PHI、出口 liveout、STORE→fresh RegBytes LOAD |
| two-state-bound-order | 1 条原候选，两态2/3；保留另一路非候选控制 | 原 bounds 条件先消费，随后 GuestSize 错误出口，再 STORE/LOAD/limit 错误，最后 switch 消费 |

物理 arena 是完整66112字节：Guest65536、独立Regs512、Exit56（7个u64）、Return8。
RegBytes 由真实 `Execution.h` descriptor 指向独立区或 Guest0x8fd0；Guest36864 输出
实际覆盖别名 RSI。wrapper 检查所有 descriptor 不变，保存实际 RAX 到 Return8。
raw、历史 RSI 和 effect counter 包括0/1、全1、符号位、交替位和 u64 wrap。
trial6 的逻辑 GuestSize8191、物理 arena65536 用来检查错误路径，错误前不会访问输出地址。
trial5 的 limit0 检查 STORE/LOAD 之后的错误和旧历史值。源无 noundef/noalias/assume/freeze。

验证顺序固定，不用产品结果生成 expected：

1. emit 并冻结 source/raw/8inputs/candidate manifest/三个显式 fault 源。
2. BEFORE_ONLY parser 只 parse/verify/print，符号表不能含 Corridor API import/implementation。
   C 和 LLVM 在 O0/O2 各比较32完整 arena（C16+LLVM16），先通过保存流审计。
3. 三个 fault 在每个 O 级别各运行24观察：wrong-history 指定8，skip-size-error 指定1，
   repeat-effect 指定7；16个指定错误必须检出，其余8个未触发观察仍须完整匹配。
   它们是刻意 mutants，不能称旧产品错误。所有观察均保存。
4. 前两级校准及审计全部通过后，rewrite producer 才能调用 installed Batch API 两次，
   只能 producer UND→实际 SDK T，禁止显式实现对象或共享LLVM叠加。要求71/1重写、完整、
   无预算耗尽、实际循环 switch0；失败完整保留，不改候选、输入、阈值或硬门槛。
5. after LLVM O0/O2 各比较16完整 arena，再审计保存流。每个正常比较执行27个 comparator
   corruption controls。audit 检查每条输入/expected/actual、全部键、SHA、物理单一XZ EOF。

全部都是计划数字，执行后按实际分别报告。正常总计划为 before64+after32=96完整匹配；
fault 总48观察/32指定检出/16未触发匹配；正常 comparator controls 共2592。
没有新增 C/UC 原程序引用，没有用旧16真实样本的期望替代此独立模型。

runner `dispatch_corridor_batch_native.py` 分离 prepare-before、run、audit、prepare-after。
每次 CLI 使用原512MiB启动/64MiB运行/180s外层守门；每条子命令180s，不改变 solver 或证明预算。
显式提供绝对 compiler/opt/llc/include-root，pin实际 `Execution.h`、Python、source、raw、
producer build收据、SDK manifest 和 DSO SHA；before parser 与 after producer 使用各自独立构建。
所有子命令完整 stdout/stderr（包括失败和部分超时）落盘。禁止复用已存在的 out 目录。
两版 harness 均保留。v2 实际消费 SDK manifest 的 runtime 路径/SHA、solver精确路径/SHA
或null；producer报告运行时DSO映射，runner检查路径、设备号和inode。
main必须映射已pin的Z3.so.4.13，No-Z3必须零solver映射；LD搜索前缀显式设置。

## 实际 before 与故障校准

ROCK `/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-20261115-v1` 已冻结：
241个payload、52,076,676字节，双端全部成员/size/SHA一致。
FILESET SHA `03956017c6a5800c396b8908be66ec07713441c3eae61759c29b2a0389fba11e`；
BEFORE-SUMMARY SHA `b13bfc202decaf4bb6d7a4a0be506256dd4e8f1cb481b0d1b064d23afc92e90f`。

- 新C32、新LLVM32：共64次完整66112字节匹配，1728 comparator controls全检出。
- 显式fault48观察：32指定错误全部在指定8字节检出；另16未触发观察完整匹配。
- 4条保存流均完成所有frame/键域/SHA/物理单一XZ EOF审计。
- 15个外层守门命令全通过，最长21.065秒；0skip、0排除、0normalization。
- BEFORE_ONLY parser5次均零API调用，无Corridor导入；实际SDK4a89、Z3.so.4.13路径及
  设备/inode完整匹配。source/SDK前后pin不变。没有新的Unicorn执行。

Mac完整镜像位于
`/Users/c/Desktop/deob/validation-artifacts/dispatch-corridor-batch-native-20261115-v1/ROCK`。
此基线只计一次；after重放不重计这些C/LLVM/fault数字。

## 实际 main after

主SDK SHA `4a89d8a97d996e6423cb2a670924cee21714623bc28e74378d25bd93c0f3b080`。
两个实际installed Batch API调用分别重写71/1，complete=true、无预算耗尽、循环switch均0。
O0/O2各16完整arena匹配、432 controls全检出，两个保存流审计通过。
独立namespace是 `/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-after-main-20261115-v1`。
AFTER-SUMMARY SHA `bf629da7e790a20f36ab48291021986cf94ac1f57a68ab3044b12860032ead4b`；
FILESET SHA `8d3f1af7d5532b081476c4cb83c63e2b93f6f1d1f670f82e862499e877ec050c`。

## 实际 No-Z3 after 与总计

No-Z3 SDK SHA `11f6046d353ae35e936c428a1e0007132df20d22f32a8fb26aa572e7fee589e0`。
manifest solver=null，两个producer的实际solver_maps为空。两个API分别重写71/1，均完整、
无预算耗尽、循环switch0。O0/O2各16完整arena匹配、432 controls全检出；两个保存流审计通过。
独立namespace为 `/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-after-noz3-20261115-v1`。
AFTER-SUMMARY SHA `dc334ff24a20695372cebd54b70a7932fc175bd40783bc9befea5b6fd3cc00b4`；
FILESET SHA `63eac49f8b4abfa003e7be1bacff33d430c6edf6c926a6b014ea61794a27e66a`。

两库各7个外层守门均成功，所有实际源/SDK pin在各自执行前后不变。
两个模块的 parsed-before、after IR 及完整stats在两库间逐字节/字段一致，没有normalization。
正常新比较共128：before C32+LLVM32，只计一次；main afterLLVM32、No-Z3 afterLLVM32。
3456个正常 comparator controls全检出；显式fault48观察/32指定检出/16未触发匹配另计。
8条保存流共176frames均完整复核。实际API调用4次，Unicorn0；没有新机器指令语义或
真实Tigress样本原生重跑。本轮是通用人工IR验证域；表内容/别名不变量仍须调用方组件证明，
不能用候选列表或另一个假设table CFG诊断替代真实证明。

## 新输入补充：clone 内的旧 history 错误出口

首轮8个arena中 trial5 的 limit0 在首次 entry→head 即退出，其他 limitMAX 不触发错误，
因此它们没有验证新clone上的错误出口 history。原数据、模型、raw和所有既有结果保持冻结。
补充只从旧initial派生3个新frame，唯一修改是Guest[24:32]的limit：

| 新frame | 原initial | limit | headers/effects/dispatches | 状态覆盖 | RegBytes alias |
| --- | --- | --- | --- | --- | --- |
| A | 0 | 4 | 2/2/1 | 1 | 否 |
| B | 1 | 2^63+4 | 2/2/1 | 1 | 是 |
| C | 0 | 214 | 72/72/71 | 70 | 否 |

这些coverage来自冻结Python模型；实际完整arena匹配同时核对相应的counter、trace和error。
复用已冻结的C/beforeLLVM/mainAfterLLVM/No-Z3AfterLLVM八个O0/O2 ELF；没有重跑原8输入，
没有新producer或API调用。两个优化等级共24/24完整匹配、648controls全部检出。
另外仅复制原raw，把唯一错误出口PHI的`%history`替为`%advanced`，编译后6次观察均在
Return66104的8字节检出差异。4条保存流共30frames完整审计，零跳过、排除和normalization。

v1在完成O0的12次匹配/324controls后，audit最终报告构造发生同名`frames`键冲突；
其源码、实际流和失败整树均保留，不能称产品错误。v2仅把汇总放入counts子对象，保持
原frames文件pin及全部比较/先决检查，另namespace重跑。v1重复观察不加入最终24的数字。

成功v2：`/home/ubuntu/vmc-runs/dispatch-corridor-batch-history-error-supplement-20261115-v2`。
SUMMARY SHA `11cf33937aedfa5392b31345ca061dd16bfb6a75ea895fe22943198b285d87dd`；
FILESET SHA `71f93b29f94886826afa98e709586a6abd925e65c1a0969af612da336f81dc25`。
8个外层守门全部通过，最长2.209秒。该根91payload/3,108,799字节完整双端保存；
失败v1另31payload/1,384,278字节完整双端保存，原before和两个after根未修改。

当前已验证域合计正常152匹配/4104controls，故障54观察/38指定检出/16未触发匹配；
12条成功保存流共206frames。另保留失败v1的重复12匹配/324controls，单独记账。
原四次API调用不重计，补充API0、Unicorn0。这是新增输入补充，不能替代首轮源或任意输入证明。
