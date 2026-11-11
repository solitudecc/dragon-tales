# 按入边的 checked Guest 内存验证器

本组验证 LLVM 变换，不修改生产代码。基线为 `50f698c`，冻结 backend SHA-256 为
`7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799`。

## 独立原生矩阵

`scripts/validation/guest_memory_checked_edges_native.cpp` 直接构造 LLVM，调用现有
`detail::runGuestMemorySSA()`，保存每个模式的原始与实际 Pass 后 IR，并验证模块。
它不读取镜像、函数名称或样本快照，不以目标地址表常量充当不变性证明。
Python 参考模型在 `guest_memory_checked_edges_diff.py` 中按源级步骤独立计算结果，
不从 Pass 输出反推期望值。

四个基本正例有一个 STORE 入边和三个未知入边；共享 RBP PHI 和按 byte-GEP 计算的
LOAD 跨越一到两个 bounds guard。保存的值和地址仅在已知入边上成立。普通地址形式
明确包含 `sub(rbp,120)`，不能因当前 PHI 翻译器不支持而删除模板或取消收益检查。
另有动态冻结存值、外层 guard、跨块 live-out，以及 alias、部分写、未知调用、fence、
volatile、覆盖写、循环重入模式。循环最多四次，后续迭代转到未知地址，不能把首次
已知边当作循环不变量。

544 个输入覆盖四条入边、逻辑范围有效/少一字节/起址越界、0/1/符号位/全 1 输入。
循环额外覆盖首次成功后再次检查失败。实际 host buffer 始终为 64 KiB，所有生成地址
都落在 host buffer 内；逻辑 Guest size 可以更小，故故障控制能以退出字段/状态差异
检出跳过检查，不靠 SIGSEGV 或编译器 UB 作为控制成功。

原始与 Pass 后代码分别用原生 O0/O2 编译，共 **2176 次正常比较**。每次比较返回值、
未知写函数调用次数、全部 **512 字节寄存器、65536 字节 Guest 内存和七个 Exit 字段**。
Guest descriptor 的 data/size 还要保持原值。每条原生快照及其 SHA、完整期望快照序列、
所有编译/运行命令、stdout/stderr、IR、源文件和实际库哈希都会保存。

两种明确错误的 IR 控制也经过原生编译执行：

- 全局把共享 LOAD 替成 50：必须在未知入边且 bounds 有效的 **24 次**观测上报差异。
- 把 bounds 条件改成 true：必须在逻辑访问失败的 **64 次**观测上报差异。

两组控制共 192 次观测；88 次指定错误必须全部检出，其余 104 次必须保持匹配。
`--self-test` 另外逐一修改返回值、调用次数、七个退出字段、寄存器和内存，验证 **5987**
个单字段/错误长度控制。没有跳过、排除、字段归一化或不确定值固定预期。
顺序原生执行不证明 fence/volatile 的并发语义；其拒绝边界另由 C++ 结构回归检查。

收益与语义分别把关。`--require-gains` 必须确认四个基本正例确实转发并改变对应入边，
所有三个未知入边都保留，屏障模式不做入边特化；仅有原生结果一致或合法 IR 不算完成。
覆盖写模式可能由原有块间 SSA 先转发一次，这不计为新克隆收益。
producer 用不跟随 RAUW 的 `WeakVH` 检查原 LOAD：除 overwrite 应被原有 SSA 删除外，
其它模式都必须保留原 LOAD 给未知入边使用。早先保留的记录采用 `WeakTrackingVH`，
不会悬空，但观察对象可能随 RAUW 改变；最终矩阵需使用加强后的原对象存活门重跑。

在 VM 上复现：

```sh
python3 scripts/validation/guest_memory_checked_edges_diff.py --self-test
python3 scripts/validation/guest_memory_checked_edges_diff.py \
  --repo /home/cc/dragon-tales-overlap \
  --library /absolute/frozen-runtime/libdragon.so \
  --work /absolute/new-output --require-gains
```

旧库应使用 `--baseline` 替代 `--require-gains`；输出目录必须不存在，不覆盖旧失败。
旧库已实际完成：2176 次正常比较零差异，192 个控制观测准确检出 88 次指定错误，
5987 个比较器控制通过；四个基本正例 `redirected=0 / forwarded=0`。旧库的 overwrite
模式保留原有一次转发，其余没有伪造克隆收益。证据在 VM
`~/vmc-runs/guest-memory-checked-edges-20261111/baseline-native-attempt001/`。

首个候选集成库的收益门曾实际拒绝全部 Sub 正例，正常比较数保持 0，不能称为通过。
只读 `PHITransAddr` 探针证实 `checked.load→join` 保留原 Sub，但 `join→known` 返回 null，
且模块未改变。这一失败保存在 `candidate-v1-native-attempt001/` 和
`sub-translate-probe.log`，测试模板没有改成 Add 以迁就它。
最终实现另行记录如下，不能把基线正常比较或候选拒绝计作最终成功。

## 最终 v2 集成结果

最终 backend SHA-256 为
`bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003`，shim 为
`cb01b3cb7723d69e1902fc89e393c62729c01e3fdc25cfd94aeef5827e11e855`。
新/旧库都以最终 `WeakVH` 和原 LOAD 存活门运行，保存为 `final-v2-native/` 与
`baseline-native-attempt002/`。两者验证器源码哈希相同，**11/11 原始 IR 逐字节相同**。

新库 **2176/2176 正常观测匹配**，192 个故障 IR 观测准确报告 **88/88** 个指定差异，
5987 个比较器单字段/长度控制通过，基础设施错误、跳过、排除和归一化均为零。
四个基本正例及 loop 各恢复一条已知入边；合计新增 **5 条入边、12 个克隆块、29 个
SSA 修复 PHI**。加上原有 overwrite 转发，`loadsForwarded` 合计 6，其中只有 5 个是
本次新增收益。所有模式的三个未知入边均保留；除 overwrite 外原 LOAD 均存活。
六个屏障/覆盖模式没有做入边克隆。

独立保留文件审计重新哈希 **154 个文件**，并逐字节复查旧/新两组保存的 **4352 个正常
快照、384 个控制快照**。这是对已有执行的审计，不是新增独立样本或原生运行次数。
审计记录为 `final-retained-audit.json`，源程序也在证据包中。

## 冻结真实 ELF

`scripts/validation/guest_memory_checked_edges_real.py` 复用既有 frozen replay、原始 C/
Unicorn/LLVM 完整状态比较器和四个输入。四个 ELF 是 `f-cff/f-combo × O0/O2`，输入为
0、1、0x80000000、0xffffffff。完整性门要求 C16、LLVM16、四模块、Graph 不变及输入/
实际库哈希不变；寄存器/内存控制各 16 次，七个 Exit 字段控制各 16 次。

默认运行原始显式流水线，GuestMemorySSA 仍位于 mem2reg/GVN 前。
`--ssa-after-gvn` 只作为另一项显式实验，在 GVN 后额外运行相同 GuestMemorySSA；旧/
新库必须采用完全相同的放置方式和输入，才能归因新增效果，不改变产品默认顺序。
GuestMemorySSA 的 `complete=false` 同时保留在 counter、对应 INCOMPLETE status 和
摘要列表中；完整根函数执行匹配不能冒充分析预算已经穷尽所有路径。

```sh
env PYTHONDONTWRITEBYTECODE=1 \
  PYTHONPATH=/home/cc/dragon-tales-overlap/python:/home/cc/dragon-tales-overlap/scripts/validation \
  DRAGON_C_LIB=/absolute/frozen-runtime/libdragon_c.so \
  LD_LIBRARY_PATH=/absolute/frozen-runtime \
  TIGRESS_HOME=/home/cc/bench/tools/tigress-2.1-github \
  python3 scripts/validation/guest_memory_checked_edges_real.py \
    --frozen /home/cc/vmc-runs/crosscall-cff-combo-latest-20261111 \
    --out /absolute/new-real-output \
    --tigress /home/cc/bench/tools/tigress-2.1-github/run-tigress
```

计数明确区分 RIP 入口路由、真实 CFF 和程序原有的 switch。后两者从各 switch 的默认
Transfer 出口 source_pc 对照冻结样本事实；无法分类时显式记为 unclassified，不把它
算成 CFF 消失。验证专用的四个 CFF 地址只用于报告，绝不进入优化和目标选择。
真实表 LOAD、目标检查/default 仍保留时，不因 RIP 路由消失或 IR 变小宣称完成去平坦化。

最终 v2 在默认原流水线与额外 post-GVN SSA 流水线各完成 **C16/LLVM16**，对应每次
寄存器和内存控制各 16 次、七个 Exit 字段控制共 112 次，全部检出。旧库也用同一份
最终 reporter 和同一 post-GVN 放置方式完成 C16/LLVM16，避免将额外旧有优化归因新实现。
旧/新 post-GVN GuestMemorySSA 的转发分别为 f-cff O0 **0→1**、f-combo O0 **0→4**；
O2 两组都为 0。默认原流水线尚未利用到这一新收益，仍主要作为兼容检查。

八份冻结真实 IR 的最终集成探针结果为：

| 输入 | 预处理 | 新转发 | 新 PHI | complete | verify 失败 |
| --- | --- | ---: | ---: | ---: | ---: |
| f-cff O0 | mem2reg | 1 | 175 | 1 | 0 |
| f-cff O0 | GVN | 1 | 153 | 1 | 0 |
| f-cff O2 | mem2reg | 0 | 0 | 0 | 0 |
| f-cff O2 | GVN | 0 | 0 | 0 | 0 |
| f-combo O0 | mem2reg | 2 | 137 | 0 | 0 |
| f-combo O0 | GVN | 4 | 231 | 0 | 0 |
| f-combo O2 | mem2reg | 0 | 0 | 0 | 0 |
| f-combo O2 | GVN | 0 | 0 | 0 | 0 |

原库在同八份输入上转发均为零。表中六份预算 incomplete 和整模块重放中的 incomplete
原样保留，不因 IR 验证通过或完整根函数执行匹配改成 complete。新代码在真实状态入边
取得局部转发，但两种流水线的四个 ELF 都仍有 **CFF switch 1→1、原 switch 1→1**；
没有未分类目标 switch，实际表读取和 default 保留。RIP 路由的清理仍不能算作 CFF 消失。

证据根目录为 VM `~/vmc-runs/guest-memory-checked-edges-20261111/`：最终 native、两种
真实流水线、同最终 reporter 的旧 post-GVN 基线，以及八 IR 探针分别在
`final-v2-native/`、`final-v2-real-original/`、`final-v2-real-post-gvn/`、
`baseline-final-reporter-post-gvn/`、`final-v2-real-eight-ir/`。旧失败和 v1 候选另存，
没有覆盖或累计作最终成功。完整 CTest 数字由根代理集中记录，本验证器不冒领全量结果。
最终证据包在 VM 同目录及本地
`/Users/c/Desktop/deob/validation-artifacts/guest-memory-checked-edges-20261111/final-v2-evidence.tar.gz`，
两端 SHA-256 均为 `25397456398fb1e7352021f5331e5f29c3c2b7218db322c037ce739b84e26da5`。
上述有限输入、完整字段检查和 IR verifier 都不构成一般堆分析、任意循环或整程序正确性证明。
