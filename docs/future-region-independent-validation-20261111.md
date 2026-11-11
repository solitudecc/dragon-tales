# 后续上下文旧执行值：独立验证与真实表项契约

基线 `0b5c2ec`；本记录由独立验证侧维护，不改生产算法、原九模式 producer、字节模型或
精度门槛。旧冻结 backend SHA-256 为
`3ad2fc5cec5dd709936788d786b03eeb7c94e9a4ccef92ab2e645949ddc6d9de`，
VM 路径为 `~/vmc-runs/loop-carried-root-20261111/final-runtime/libdragon.so`。

## 旧实现的实际边界

原 `loop_carried_context_native.cpp` 的 `actual.table`（下称 S）计算
`nextIt = tail.iteration + 1`。S 的 switch 可以进入 body，body 写 Guest[0x100] 后回到
`guard.head`（下称 B）；B.iteration 的此条 incoming 直接使用旧 S.nextIt。S 还有进入
后续 `bounds.join`（J）的回边，J 的 PHI 同样接收该次 S 计算的值。

它不等于把 add 提前到 body 的上一轮隔离变体。本轮候选必须通过原夹具的精度门槛，
同时区分 body→B 的旧值、全新 B/S 执行、S→J 重新执行以及内存写后的新 LOAD。

新证据位于 VM `~/vmc-runs/future-region-reference-20261111/`。执行前磁盘可用
506 MiB；前轮同源完整组约 13.9 MB，本轮两份完整组预算不超过 35 MB，不展开旧真实
证据目录、不建主工程。以下旧组在本轮重新执行，不能另算新增样本或输入覆盖。

| 旧组 | 实际结果 |
|---|---|
| `baseline-gain-negative`，原 `--require-gains` | exit 1；`old-execution body context missing`；native 0 |
| `baseline-original-native`，原纯语义模式 | 5760/5760 完整状态匹配；2240 个错误 IR 观察中 604/604 预声明差异检出；15843 个比较器控制 |

旧 `phi/load/freeze` 各只有一个 body 入边安全前缀，body_redirected=1、index16=0；
`multi` 已能一次特化三个入口并保留 index16/23/50。`call/store/atomic/volatile/fence`
均零改写。所有组 IR verify 成功，unknown 入边保留；排除、跳过、归一化均为零。

原完整矩阵的 1440 个输入，以 before/after、O0/O2 编译执行产生 5760 次观察；比较
返回值、调用次数、512 字节寄存器、完整 64 KiB Guest、descriptor data/size 及七个
Exit 字段。其 producer/model/comparator 源 SHA-256 分别为：

```text
loop_carried_context_native.cpp 2f848f65357613a3f8906f7a31adc9858f0e2c4c6733627450bbcc2ed9b77a16
loop_carried_context_diff.py    4f91eefafdcc39c79e277df9aa5b26dac0252bc715422d2cc52bd112ac52841b
state_guard_diff.py            f0681d56576e192d4809ee2cc043f978a0d618911c28fa3a12a4da53a1cafc84
```

## 候选 v1 的原精度门槛

root 集中构建并冻结的候选 v1 backend SHA-256 为
`ec77a3960eab53eb9e8bf93cc7d1ae00dcb720c270542e3b6b7f190a6c625f7f`，
shim 为 `475286bd5ffcdac337306c3a1a8b559232ed4757fe23da49c7391a532f84bb5a`，
位于 `~/vmc-runs/future-region-root-20261111/candidate-runtime-v1/`。

`candidate-original-native-v1` 使用以上逐字未改的原文件和 `--require-gains`，完成
**5760/5760** 完整状态匹配，2240 个错误 IR 观察中 **604/604** 预声明差异检出，
15843 个比较器控制通过。`phi/load/freeze` 各有且仅有一条 body 特化，且实际
index16 LOAD 从旧版 0 变为 1；多入口和五种效果屏障保持原精度。所有原始模块 verify
有效、unknown 入边保留，无排除、跳过、归一化。

这次没有使用隔离变体或纯语义通过替代原精度门槛。其收益是原 future-S 形状到达实际
table LOAD，不是对真实表项内容的证明，也不等于真实 CFF 已消除。

同一候选的未改隔离矩阵 `candidate-isolated-native-v1` 完成 **1920/1920** 完整状态
匹配、2240 错误 IR 观察中 **604/604** 预声明差异检出、5283 个比较器控制通过；
未改旧 corridor 的纯语义兼容组 `candidate-context-compat-v1` 为 **6912/6912**、
1920 错误 IR 观察中 **486/486** 预声明差异检出、19011 个比较器控制通过。未放宽
它们原来的精度门槛；隔离矩阵仍用 `--require-gains`，corridor 不要求旧单入口副本数。

这四个正常 native 组及旧精度负控制另做 Mac 只读复审：全部源快照与当前同源文件一致，
每个报告引用的原始产物 SHA 均匹配，**16 条**完整 before/after、O0/O2 正常 snapshot
streams 逐字节与独立模型吻合；故障记录的应失败集合重新由模型计算并核对，非从结果
反推。此复审新增 native/analysis 执行数均为零。

## 新库真实四 ELF 的兼容重放

`candidate-real-raw-v1` 与 `candidate-real-post-v1` 使用同一原脚本、四个冻结 ELF、四个
输入 `0/1/0x80000000/0xffffffff`，线性 strategy0、StaticAnalysis/FunctionStart 和
product-O0。后者只按原选项额外在 GVN 后运行 GuestSSA，没有改变输入或 comparator。
两组各 **C16/LLVM16** 完整根返回匹配；各 16 个寄存器、16 个内存和 112 个七字段
Exit 故障控制全部检出，比较全部寄存器和完整 1 MiB Guest。Graph 及前后 runtime/input
pins 不变，无排除、跳过或归一化；实际 Guest megaInit 照常执行。

两组 f-cff O2 仍为 16 上下文、13 不同状态、7 条未知入边保留，原 table LOAD、49 cases
和 default 保留；conditionsVisited=155、branchesRewritten=16、budgetExhausted=1，
complete=false。四个模块仍为 **0/16/0/3** 分支改写、dispatch 全四模块 incomplete；
GuestSSA 为 raw2/post4 incomplete。这是同源真实兼容重放，本轮没有新增真实状态解析
或去掉 CFF switch。旧真实基线未重新执行，不混入新执行数字。

## 真实 f-cff O2 为什么仍有表分发

本轮只读核对前轮已封存的真实 IR，没有新增 native 执行或重新运行分析。输入取自
`loop-carried-reference-20261111.tar.gz` 中
`final-real-post-attempt002/oracle/f-cff/O2/s0-product-O0/after-pipeline.ll`，SHA-256
`7bb34a7e39a049ad4b73ce02dc7e7a5dd1c76dfa220d1ea1a4dad58a2f0a8998`。
其中已知 state50 的实际形式是：

```llvm
%address = getelementptr i8, ptr %guest, i64 70096
%target = load i64, ptr %address, align 1
switch i64 %target, label %jmp.unresolved [
  ; 49 个实际 Guest target / LLVM 目标块
]
```

这证明了 index，不证明该 LOAD 的内容。当前完整状态基线的 FunctionStart 只约束入口
PC，Guest 内存仍任意；镜像读者、只读 section、一个成功 replay 均不能升级为内容证明。
该边界已写入[原实施方案 §2](deobfuscation-implementation-plan-20261109.md)。

例如同一已知 state 的槽合法地含 case A、case B 或其它地址时，当前 switch 分别进入
A、B 或原 Transfer/default。因此在这个输入契约下无条件换成候选 A 不能保持语义。

## 实际 LOAD 比较 guard 的用途与限度

在相同位置复用同一个实际 selector SSA，把 `target == candidate` 的真边连到已有
case，其它值走原 switch，可以保持原定义执行。必须保留原 LOAD、bounds、所有状态和
副作用，修复两个新前驱涉及的所有目标 PHI，不能重新读一次表或只改 indirectbr 目标列表。

但在当前真实 IR 中，原 switch 已直接连到这 49 个业务块；简单剥离一项没有增加目标
知识或消除状态循环，只是把已有 case 边重写成 cmp/branch，LLVM 清理还可能合回 switch。
它可作为阅读视图或明确标注的受检查快速路径，不能计作无条件 CFF 去除；本轮不为取得
表面分支统计而加入此改写。

现有两个 Pass 也不能直接复用来授权此假设：

- `dispatch-specialize` 只将已证明唯一常量 selector 改成直连；它没有 image 内容假设。
- `dispatch-deflatten` 只接受无逃逸定义的 PHI dispatcher，或同前驱精确 reaching STORE
  支撑的 simple LOAD；带 live-out 或目标 PHI 的真实表块不能绕过。

有实际收益的后续输入域应优先复用程序本身的支配守卫或精确 STORE 事实；否则由单独的
上下文提供者声明映像加载/初始化入口、映射/重定位和有限内存前提，产物另名。事实在别名
写、未知调用或同步后必须失效，不能污染无条件基线。若提供运行时检查的上下文分支，需
另外证明检查无新增故障/副作用、事实持续到使用点，失败边仍执行原程序。单纯检查表项
与某 case 相等不构成该完整组件。

## 本轮未覆盖

独立 native 矩阵使用 main 冻结库和宿主兼容 DL64；它不是任意位宽/指针类型、一般循环
不动点或全程序堆分析证明。表项契约调查是已保存文件的只读复核；真实两组仅为上述有限
输入的兼容重放，没有新 CFF 消除或新真实状态覆盖声明。候选的完整测试、无 Z3 及源绑定
由 root 另行记录。

## 完整证据与恢复

完整归档 SHA-256：

```text
ab660459cd88de7eb45268f5b8b15befe84da964fd78d54bf536e45bd76253f1
```

- VM：`~/vmc-runs/future-region-reference-20261111.tar.gz`
- Mac：`/Users/c/Desktop/deob/validation-artifacts/future-region-reference-20261111/future-region-reference-20261111.tar.gz`

归档为 **808 个 members / 772 个普通文件**，其中 771 个数据文件加 manifest；双端
整包 SHA 及全部数据文件 SHA 完整复核。四个正常 native 组的原始 snapshot streams
也在本地另做上述全字节审计，16 条完整 streams 全部吻合；20 个实际 harness 源文件
与同源 worktree 逐项绑定。最终 `final-summary.json` 汇总四个正常 native 组、精度负
控制、两组真实 replay 和指定错误控制；失败 stderr、编译/生成日志和产物均保留。

为释放 VM 空间，确认当前 expanded 文件集与归档 manifest 完全一致后移除了本轮
`future-region-reference-20261111/`。完整双端 tar 未删除，VM 相邻的
`future-region-reference-20261111-ARCHIVED.txt`、`-local-archive-verification.json`
和 `-cleanup.py` 保存恢复说明、Mac 逐文件校验回执及清理检查。恢复命令：

```sh
tar -xzf ~/vmc-runs/future-region-reference-20261111.tar.gz -C ~/vmc-runs
```
