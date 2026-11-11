# Entry-prefix 真实验证

基线为 `071c775e`，生产实现里程碑为 `bb0c429`。固定生产配方已通过四个真实ELF的C/LLVM完整状态验证，并达到 f-cff O0 首次实际分发目标直连的精度门槛。它隔离的是检查成功后的首次入口前缀，不是整个函数去平坦化；原始动态分发、循环回入、未知路径和错误出口仍保留。

## 执行前固定的配方

新 `scripts/validation/guarded_entry_prefix_real.py` 包装已冻结的 `guarded_entry_real.py`（SHA256 `812c7862616804db32a40ace8984da38634362017a64f172180d752443239b0e`），未修改旧脚本、输入或比较器。旧 SP / SP+output 两组精度失败及完整归档保持原样，不将本次结果回填到旧组。

raw SP-only guarded view 创建后，显式运行一次 builtin `entry-prefix(max_blocks=32,max_instructions=2048)`，设置 `max_iterations=1,max_pass_runs=1`。随后原样运行此前三轮pipeline：mem2reg、instcombine、gvn、guest-memory、dispatch-specialize、control-flow、dce、只读checkpoint。旧 dispatch-specialize 的6块限制、旧3轮/32run上限不变。每个模块实际记录 **1 + 3×8 = 25 runs**，其中3个是只读checkpoint；新prefix不是免费初始化或暗增旧预算。

入口候选仍为RSP `(32,64,51192)`，四张CFF表仍各408字节，来自原ELF，必须通过真实runtime byte checks；没有把文件权限当Guest不可变。原四ELF、16输入、C reference、完整寄存器/Guest/Exit比较及144故障控制不变。每次prefix核对普通root/递归callee逐函数字节不变；拒绝且未peel时，整个module IR逐字节不变并保留原拒绝原因。正常无seed或单入边skip使用真实counter解释，不冒充错误或收益。

执行前草案仍保留在 `PREDECLARED-DRAFT.json`，最终契约为 `PREDECLARED.json`：

- 最终契约SHA256：`43d366ec9b9f44b57d4eea5846906fac128ba9c77ffed0917dcf9cf2059d74d7`
- 新adapter SHA256：`54db29bd0a691fbf00f539374e7201d2e23935ffe41d94f8c4d0ffa820fbbf7c`
- 自测：3正例、9个错误报告控制通过，另有独立精度检查器4个错误IR控制通过。

## 真实执行结果

runtime为 `~/vmc-runs/entry-prefix-production-20261111/runtime-main-v1/`：

- backend：`e5d15ecdc1a8ca789169635e335ee1f3ee5c336209a364a0ebaea7526d4d08da`
- C shim：`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`

C reference **16/16**、LLVM完整状态 **16/16**匹配；寄存器16/16、Guest16/16、七个Exit字段112/112故障控制全部检出（共144）。四份source IR与Graph均不变，运行前后runtime、六个输入文件、所有导入验证源码、契约及driver pins一致。没有新增排除、跳过或语义归一化。耗时108.22秒，仅作为这次运行记录，不做速度提升声明。

| 样本 | prefix结果 | 克隆块/指令 | 原始出口 | backedges / terminal edges |
|---|---|---:|---:|---:|
| f-cff O0 | complete，peel1 | 21 / 748 | 67 | 1 / 1 |
| f-cff O2 | complete，peel1 | 22 / 961 | 50 | 1 / 1 |
| f-combo O0 | partial，peel1，budget1 | 32 / 2007 | 2 | 0 / 0 |
| f-combo O2 | unchanged，rejected1，budget1 | 0 / 0 | 0 | 0 / 0 |

combo O2 的原始拒绝原因是 `non-PHI or unsupported external live-out`；前后IR相同，不算新增收益。combo O0保留了预算内安全前缀，但不能称complete。后续旧pipeline的 GuestMemorySSA仍12/12次incomplete（budget counter合计12），dispatch-specialize仍12/12次incomplete（budget counter合计16）。这些统计独立于新prefix，不能用某个prefix的complete推导整套分析complete。

## 必达精度证据

函数为 `dragon_static_entry_x86_64_102a0.guarded.real_sp`。prefix刚复制、尚未执行原pipeline时，`memory.valid13.entry.prefix` 含真实Guest目标LOAD `%9764`，经RAX快照传到 `%9767` 的CFF switch；这证明后续观察的是实际目标选择，而非入口校验LOAD。

最终 `guarded.next50` 仍真实读取Guest[70032]并检查其值67010：

```llvm
%2676 = getelementptr i8, ptr %2, i64 70032
%2677 = load i64, ptr %2676, align 1
%2678 = icmp eq i64 %2677, 67010
br i1 %2678, label %memory.valid.entry.prefix, label %guarded.fallback
```

检查成功后的私有前缀没有目标switch。末端只保留尚须运行的内存边界检查，实际目标已直连原块：

```llvm
memory.valid11.entry.prefix:
  %2718 = add i64 %2716, -8
  %2719 = icmp ugt i64 %2718, 70031
  br i1 %2719, label %bb_67010, label %memory.invalid14.entry.prefix
```

这里没有目标LOAD。`memory.invalid14.entry.prefix` 仍写Unsupported/Memory Exit（reason=5、detail=3）和完整状态，再返回；不能为得到无条件branch而删掉它。原始 `memory.valid13` 的动态switch仍在，用于普通路径和回入。独立精度检查器核对guard真实LOAD、全部可达私有前缀无selector switch、唯一目标边、错误出口及原始switch保留；四个破坏控制（错目标、错guard地址、删错误出口、重新引入目标块LOAD）均被拒绝。root另行读取同一最终IR确认该链路。

本阶段没有新增真实guard observer运行；此前SP-only独立observer的16次命中属于旧组，不能混入本阶段C16/LLVM16统计。本次实际执行IR保留相同检查候选和wrapper唯一调用绑定，prefix只改检查成功后的body。

## 资源与失败记录

首次尝试在真正oracle启动前被450MiB空间门槛拦截（448.75MiB），因此当时0 analysis/0 native；`PREFLIGHT-SPACE-STOP.json`保留了原因。root完成旧运行库双端归档后释放空间，原样重启，没有降低门槛。运行中每秒监控，低于64MiB即停止本任务进程组并保留证据；实际最小可用空间401,010,688字节，没有触发中止。

独立诊断检查器的初稿误把Exit.reason当i32/3，在已成功主矩阵之后触发断言。根据Execution.h的真实ABI修正为i64 reason5、offset48的detail3；初稿与失败说明保留，不改变生产IR、输入、比较器或精度目标，也不计native失败。

本机必要IR位于 `/tmp/dragon-entry-prefix-real-evidence/result/`；VM原始运行根为 `~/vmc-runs/entry-prefix-real-20261111/`。完整证据须经过VM/Mac归档SHA、精确文件集合与每payload长度/SHA校验后才允许退役展开数据；最终归档定位在交付索引中记录。runtime由root独立冻结归档，本agent不复制库。


## 最终归档

完整包：`/Users/c/Desktop/deob/validation-artifacts/entry-prefix-real-20261111.tar.gz`；VM对应 `~/vmc-runs/entry-prefix-real-20261111.tar.gz`。

SHA256：`81216b4fa0a4d1ef6239504faae9264241f71a52498f79d710f2e9292d097730`。

压缩大小66,767,567字节；330个payload文件共284,727,672字节，另含归档manifest。VM与Mac都已核对压缩SHA、精确文件集合、每个payload的大小及SHA。包中包括源码快照、全部执行/报告/IR/输入输出/controls、首次空间中止、精度检查器初稿失败、最终精度检查及16份输入与旧SP保存流完全一致的审计。归档中的文档为归档前版本；当前版本仅补充该包自身定位。
