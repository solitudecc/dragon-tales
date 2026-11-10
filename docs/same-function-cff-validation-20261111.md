# 真实 CFF 状态恢复：冻结样本诊断与区域提升验证

2026-11-11。对象为 Tigress 2.1 revision 2128、seed `4242` 的四个冻结 ELF，
不是 20261110 综合大函数那批不同种子的 ELF。本轮不改生产代码、Graph、表内容契约或
比较器；使用 `9020c70` 加本轮同函数去平坦化安全补丁的已构建库，精确库哈希见下文。

## 先纠正分析对象

`switch(load regs[128])` 是 Dragon 生成的 x86-64 RIP/入口恢复路由。它选择机器入口 PC，
不是 Tigress 的 CFF 状态变量。移除这个 switch 不能计为去平坦化。此前把复杂样本的主要
缺口归为“调用前写 GuestState，callee 入口读取状态”过于宽泛：这描述了框架的入口路由，
没有定位 kernel 内真正的混淆分发器。

真正的 CFF 是同一机器函数内的 `_1_kernel_next` / `_4_kernel_next` 循环。源码中的
另一个 `u32 state` 是业务计算数据；`switch(code)` 是原程序 switch，也不能算 CFF。

冻结根目录：`/home/cc/vmc-runs/crosscall-cff-combo-latest-20261111/`。
下列源码行号均相对此目录中的 `<profile>/transformed.c`。

| 样本 | 真正的 CFF 状态与写入 | 分发读取和未解析 JMP | 原程序 switch 的另一个 JMP |
|---|---|---|---|
| f-cff O0，kernel `102a0..1073b` | 源码 200/204/207：`next=50; switch(next)`；`102b2` 写 `[RBP-0x78]=50`；kernel 内共有 49 条直接写该槽的机器指令 | `102ba` 读槽，`102d0` 读 `0x11000+8*index`，`102d8 JMP RAX` | `1067d`，表 `0x11198`，对应源码 418 的 `switch(code)` |
| f-cff O2，kernel `10080..103fb` | `1009d ECX=50`；`100c6 ECX=4`、`10108 ECX=32`、`102de CMOVAE RCX,RAX` 等在同函数回边写 next | `100b0 CMP RCX,50`，`100b6 JMP [0x11040+8*RCX]` | `10347`，表 `0x111d8` |
| f-combo O0，kernel `10770..15df2` | 源码 257/295/297 定义并分发 `_4_kernel_next`；`10aae` 写 `[RBP-0xa8]`；kernel 内共有 42 条直接写槽的机器指令，另有指针参数 helper 写入 | `10ab5` 读槽，`10ad4` 读 `0x16008+8*index`，`10adc JMP RAX` | `13a87`，表 `0x161a0`，对应源码 383 的 `switch(code)` |
| f-combo O2，kernel `10090..10e10` | `101af ECX=50; 101b4 CMOVE RCX,R15` 给初值；`101df CMOVE RCX,RAX`、`1023c CMOVE RCX,RAX` 等更新 next，源码 helper 已被编译器内联 | `101f0 CMP RCX,50`，`101f6 JMP [0x12040+8*RCX]` | `107f3`，表 `0x121d8` |

f-combo O0 的实际跨调用状态写确实存在，但只覆盖部分 case：源码 301/303、312/314
把 `&next` 传给 helper；机器调用位于 `10b88/10bb0/10cb9/10cd2`，被调函数源码 98、109、
125、229 写 `*next`。这不是普通 f-cff 的先决问题，也不是 O2 样本仍 unresolved 的统一解释。


`machine-state-write-inventory.json` 保留两份 O0 kernel 的全部直接 next 槽写指令，以及四个
kernel 回到实际 dispatcher 的机器指令片段；O2 片段没有把临时 RCX 运算一概标为 next 写。

四张 CFF 表都含 51 项、39 个不同机器目标，文件中的目标都在对应 kernel 范围内。
这只是文件内容和覆盖候选统计，不是运行时内存不变性证明。

## 普通 lift 为什么没有真正的 CFF switch

只读调用 `BackwardSlicer(5).slice_reg(..., RAX)` 和 `detect_possible_switch()` 得到：

- f-cff O0：`MEM[0x11000 + 8*MEM[RBP-0x78]]`，`entry_size=8, num_cases=51`；
- f-combo O0：`MEM[0x16008 + 8*MEM[RBP-0xa8]]`，同样为 51 项。

两处 guard/形状识别成功，但 Graph successors 仍为空。阻塞的是独立的**表内容证明**：
`lib/analysis/TableContents.cpp` 只接受精确读取位点的 `readKnownMemory` 或真实 Guest
STORE 推导的内容；普通 `readMemory`、ELF 字节和 `.rodata` 标志都不构成证明。这些表在
已测程序路径中没有 Guest STORE 初始化，因此普通任意函数入口契约下仍为 Unknown。

普通 lift 只提升已知函数成员，真实 JMP 被降为真实表 LOAD、`Transfer/Branch` Exit 和
返回。它没有包含 case 块的 CFF switch。旧普通产物可复核：

- `f-cff/O2/s0-product-O0/raw.ll:1152` 是 regs+128，`:1166` 是 RIP router；
  `:1621` 起读取实际表目标，`:1624` 起写 Transfer Exit 后返回。
- `f-combo/O2/s0-product-O0/raw.ll:1157/1171` 同样是 RIP router；
  `:2856` 起真实 LOAD，随后写 Transfer Exit。

因此，对这些普通模块仅统计 `dispatchers_visited/unresolved` 不能说明 CFF 状态恢复进度。
默认普通 LLVM replay 的 incomplete 是保守覆盖边界，不能算完整语义匹配。

## 现有 API 已能安全展开同函数机器覆盖

不必把镜像表内容升级为常量。现有 `liftStaticRegion` / `PreparedRegion` 可以将已解码地址
作为**覆盖候选**，保留实际 LOAD、实际 target 比较和未匹配目标的 Transfer default：

```python
snapshot = static_region_replay.graph_snapshot(graph)
leaders = static_region_replay.select_region((kernel_start, kernel_end), snapshot)['selected_starts']
with dragon.LLVMLifter(2, execution_mode=2, static_entry_policy=1) as lifter:
    assert lifter.lift_static_region(graph.get_function(kernel_start), graph, leaders,
                                    rebuild_decoded_blocks=True)
    lifter.run_mem2reg()
```

`select_region` 只使用有效块、直接分支目标、终结指令之后的已解码指令和孤立解码段起点。
本轮没有安装 `read_known_memory`，没有运行 Graph finite-dispatch，也没有把目标候选发布成
Graph edges。`PreparedRegion` 在私有 Graph clone 中切块和重建 IGNIL；降低后的间接 JMP
switch 比较运行时 LOAD 的真实 target，default 仍写精确 Exit/PC。

四个样本的 leaders 分别为 `66/49/102/74`。12 个 API 探针（普通、39 个文件目标候选、
静态 leaders 三种覆盖）全部 lift/verify 成功且源 Graph fingerprint 不变。只传 39 个表
目标候选仍会遗漏中间条件块或 CALL continuation；完整重放使用静态 leaders，四个 kernel
都获得两个真实 target coverage switch（一个 CFF、一个原程序 switch）。这不是宣称表项
不可改变，也不是已经移除了 dispatcher。

## 指定流水线和完整状态执行

对四个 ELF 分别使用同一接口，顺序为：

```text
guest-memory-ssa(run_mem2reg=False) → mem2reg → instcombine → gvn → mba → predicates
→ dispatch-specialize → dispatch-deflatten → control-flow → dce
```

每模块仅一轮，保持默认 solver/dispatch 预算，逐 Pass verify。继续使用原
`finite_vm_replay` / `tigress_vm_challenge` / `tigress_function_diff` 的编译和全状态比较：
C O0/O2 ELF、线性扫描、LLVM product-O0；每例输入为 `0,1,0x80000000,0xffffffff`。
实际 Guest `megaInit`、malloc、内部 callee 执行，不注入堆快照或外部效果模型。

| kernel | 真实 LLVM 指令数前→后 | RIP entry switch 前→后 | 实际 target switch 前→后 | 每个 target switch 的覆盖 case 数 |
|---|---:|---:|---:|---:|
| f-cff O0 | 14156→6597 | 1→0 | 2→2 | 66 |
| f-cff O2 | 8154→3124 | 1→0 | 2→2 | 49 |
| f-combo O0 | 281571→17141 | 1→0 | 2→2 | 102 |
| f-combo O2 | 37382→9901 | 1→0 | 2→2 | 74 |

`summary.json` 的历史字段 `instructions` 实际按缩进行计数，包含 switch case 行，不能当成
LLVM 指令数；本表由保留的 `count-real-cff-ir.py` 逐条统计 LLVM 语句，结果另存
`real-root-counts.json`。不修改原报告和原始 IR。

四模块的 Guest-memory SSA 分别转发 `0/0/1/0` 个 LOAD、没有插入 PHI。模块级 MBA 改写
`0/0/16/11` 次，谓词折叠 `23/3/1212/16` 次。`dispatch-deflatten` 每模块看到两个真实
target switch，均 `unresolved=2`、`edges_rewritten=0`；跨调用摘要/直接 STORE 消费也为零。
`dispatch-specialize` 在三模块触及条件访问预算，只有 f-cff O2 未触及；没有把预算失败当成
完整证明或据此删边。

C 规格对照 **16/16 match**；LLVM/Unicorn **16/16 完整根返回 match**，different、
incomplete、其它错误均为零。每次比较 76 个模型字段、完整 1 MiB 内存、调用事件和 7 个
Exit 字段。正常结果上的 RAX 单字段和 Guest-memory 单字节负对照各检出 16/16。没有新增
排除、跳过或归一化；源 Graph fingerprint 和输入/实际动态库前后均保持一致。

这些是小规模路径验证与完整状态覆盖证据，不是全程序等价证明。与先前静态区域提升已知的
完整根返回相比，它也不是新的去平坦化收益；本轮新增的是正确识别和计数真正的 dispatcher。

## 仍需的最小通用组件

1. 分析入口显式选用已有 decoded-region 覆盖，先让同一函数内真实 case/回边进入 IR。
   普通 lift 默认行为和 Graph 的内容证明契约保持不变。
2. 从真正的 next 追踪状态：O0 是 Guest 栈帧相对槽，O2 已是 RCX 的多入边 PHI。
   当前 GuestMemorySSA 的动态地址按 SSA 身份匹配，循环只接纳不变或规范两块形状；
   不能把 O0 的复杂 dispatcher 循环等同于已经完成的一般内存 SSA 固定点。
3. 处理真实的 `LOAD(table + 8*next) → target switch`，保留访问检查、标志/寄存器写入、
   LOAD 的发生位置和 default Exit。现有纯 PHI/单 LOAD shape 的变换不能将未知表内容变为
   常量；含 GEP、跨块检查和在 default 中仍被使用的 target 都需要精确的按边线程化。
4. 在无表内容证明时，next 已知也只代表表项地址已知。可做保留 LOAD/target 检查的受守卫
   专门化；无条件删掉表分发则仍需来自真实初始化路径/写入与别名分析的内容证明。
   不能以 `.rodata`、一次运行结果或文件目标集替代它。
5. f-combo 再单独消费其堆/全局相等关系、真实 helper 写回和 ObfuscateNext 关系。它们是
   更复杂状态表达式的额外障碍，不应挡住无 opaque/无跨调用状态的 f-cff 基础组件。

## 冻结文件、后端与复现

| 文件 | SHA256 |
|---|---|
| f-cff/O0/fixture.elf | `20948e657826406c23c9945ce1b7e74605a5b033349daf3c495273a24aa537fe` |
| f-cff/O2/fixture.elf | `c8ad204e75a5b2212875a73fe4000781c37b003f39fc6411d4bccf6417c51bd4` |
| f-combo/O0/fixture.elf | `1f221ba41e5b21c740c229d68bc10348d21aabdb91f612bfaa8be99e83c91a08` |
| f-combo/O2/fixture.elf | `88b06df91d6c577fbc08aa811d9eb0dbe0490946292c5d7fb0c9a132e90a803d` |
| build/lib/libdragon.so | `dd112256a13b1888210d4433cced2858cf915fdf4a3dc108574ea87d70a52c73` |
| build/c_shim/libdragon_c.so | `b3ac77bd25f0e426e2aea10bdaf6d1c7e054ed72bc87fe24f208ab1ca1d7275f` |

独立 VM 产物：`/home/cc/vmc-runs/cff-real-region-20261111/`。本地产物：
`/Users/c/Desktop/deob/validation-artifacts/cff-real-region-20261111/`，含四个冻结 ELF、源码、
带行号可核对的反汇编、普通/区域原始 IR、所有原生输入输出、双库快照、源码 patch、manifest、
`probe-static-region.py`、`replay-static-pipeline.py` 和计数脚本。未修改冻结输入。

```sh
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH=/home/cc/dragon-tales-overlap/python:/home/cc/dragon-tales-overlap/scripts/validation
export DRAGON_C_LIB=/home/cc/dragon-tales-overlap/build/c_shim/libdragon_c.so
python3 /home/cc/vmc-runs/cff-real-region-20261111/probe-static-region.py \
  --frozen /home/cc/vmc-runs/crosscall-cff-combo-latest-20261111 --out FRESH_API_DIRECTORY
python3 /home/cc/vmc-runs/cff-real-region-20261111/replay-static-pipeline.py \
  --frozen /home/cc/vmc-runs/crosscall-cff-combo-latest-20261111 --out FRESH_REPLAY_DIRECTORY \
  --tigress /home/cc/bench/tools/tigress-2.1-github/run-tigress
```


可复现脚本 SHA256（API/replay 脚本在本地和 VM 保存同一份；计数脚本在本地产物中）：

- `probe-static-region.py`：`22acfe07ef93db970d971a65890b6efaa1feeeb998d83bf88b16ae6aaa4521a4`。
- `replay-static-pipeline.py`：`2d56c71ad9cf9455dfe751a477fbf57249748c37d101f5b967076a1a769e9a16`。
- `count-real-cff-ir.py`：`884336b9bab5cffffe814528451b0b75bbbcb8fe70101d0de52f2ded593a0973`。

上述 API 探针不执行目标程序；replay 中的 Unicorn/原生执行只属于独立验证，不进入静态分析。
