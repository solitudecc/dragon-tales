# 渐进式去混淆验收：下一级样本准备（2026-11-14）

状态：已完成当前 main/no-Z3 各一次普通根 IR 基线，间接边/函数覆盖验收失败，去平坦化精度未完成。0 新 native、C、Unicorn 或 checked 三组件执行，语义验证仍 pending。下表机器 CFG 数字来自本地冻结 ELF、完整函数反汇编及跳转表解析；本轮实际 Dragon 结果单列于后文。工作树基于 `1e45a5b`，产品算法、默认配置和预算均不修改。空间清理由根代理另行记录。

## 下一阶只选现有 Tigress Flatten O2

推荐首先使用同一业务的 `flatten-switch-O2.elf`。它是实际 Tigress Flatten/switch，开启块随机排序和基本块拆分，seed `2026111307`；不是仅更换 seed，也不包含实际未运行的 EncodeArithmetic/AddOpaque 保护。业务仍是有限 unsigned32 kernel、局部数组和短循环。O2 kernel 没有调用；CIL 声明中的 malloc/pthread 不构成本样本的堆或系统调用行为。

| 本地静态指标 | 已完成 manual O2 | 下一阶 Flatten O2 |
|---|---:|---:|
| kernel 字节数 | 1528 | 2020 |
| kernel 指令数 | 434 | 443 |
| C 源 switch case 数 | 7 | 74 |
| C 状态编号范围 | 0..6 | 0..79 |
| 中央跳转表槽数 / 不同目标数 | 6 / 6 | 80 / 75 |
| 可达机器基本块 / 边 | 15 / 23 | 78 / 152 |
| 含中央分发的 SCC 块数 | 13 | 76 |
| 可达 CFG 的 E−N+2 | 10 | 76 |
| kernel call / 条件跳转指令数 | 0 / 4 | 0 / 1 |

74 个 source case 与 80 个表槽不能混称“80 个业务状态”。源缺少编号 `26,30,34,38,43,46`；默认回路和重复表项仍计入实际 CFG。边按机器分支目标集合计数，IR 的 successor slot 数另报，不能混用。kernel 指令数只增加约 2%，中央 SCC 从 13 增到 76 块，适合在相同业务上提高结构复杂度。

两 ELF 的符号都实际给出 `oracle_entry=0x1000`、`kernel=0x1010`。manual 中央 jmp 是 `0x12b3`；Flatten 是 `0x1126`。Flatten 的 `0x1120` 先检查 `RCX<=79`，越界回自身，随后执行 `jmp [RCX*8+0x2000]`。这些地址是本样本的输入结构和定位证据，不能成为生产算法特判。所有 80 个表项已在本地检查为 kernel 内指令边界。

原 manual 最后集中写输出，Flatten 则把 `dst+0/+4/+8/+12` 四次写拆成 case 7/6/5/4，每次写后回中央分发；另有单字节异或状态。这是新的 alias 难点：任意 out/RSI 可以覆盖尚要重读的表。ELF 的只读节权限、正常输入 out=0x8000、某次 table 静态字节都不能直接变成 LLVM 内存不变事实。

## 冻结来源和独立参考

本地复核包为 `/Users/c/Desktop/deob/ida-review-20261114`。其 README 中 Dragon IR 的历史版本是 `fb85840`，不是本轮新三组件结果；`*-dragon-before.ll` 已经过上游优化，不是原始 lift。

| 内容 | 绝对路径 | SHA256 |
|---|---|---|
| 原业务 C | `/Users/c/Desktop/deob/ida-review-20261114/sources/original.c` | `67beb5c85a91a4550e9b8a9a80f949a956b989e27e7c31c57686a1487f634369` |
| Tigress C | `/Users/c/Desktop/deob/ida-review-20261114/sources/flatten-switch.c` | `249e4bc3ed549b4a01556b95a43eb8a995fd0c3267d357094c2006f5fc494eb0` |
| Flatten O2 ELF | `/Users/c/Desktop/deob/ida-review-20261114/flatten-switch-O2.elf` | `bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b` |
| manual O2 ELF | `/Users/c/Desktop/deob/ida-review-20261114/manual-O2.elf` | `f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe` |
| Flatten 反汇编 | `/Users/c/Desktop/deob/ida-review-20261114/disasm/flatten-switch-O2.asm` | `83c85eace924043177f729f9310e7a8faae6e66cb99405a70f51efc88c636dce` |

VM 的原始 frozen 根为 `/home/cc/vmc-runs/practical-region-holdouts-20261113/frozen-normal`：`original.c` 和 `flatten-switch/input.c` 都是上表原业务 SHA；`flatten-switch/transformed.c` 和 `flatten-switch/O2/fixture.elf` 分别是上表 Tigress C 和 Flatten ELF SHA。`FROZEN.json` SHA 是 `0ec963329f95b987f93cc0de8f6700b8497068f76b88fdf7e7dbd49101d9f0db`，包含参数、完整文件清单、符号和固定 16 输入：

```
0, 1, 2, 7, 127, 128, 32767, 32768,
2147483647, 2147483648, 4294967294, 4294967295, 8, 15, 16, 31
```

既存 C 结果 `/home/cc/vmc-runs/practical-region-holdouts-20261113/normal-flatten-switch/flatten-switch/results.json`，SHA `430fc8616240af56ff88290b2f6557054044672c1d69972fad171d2d4bb29e08`，该 case `2026111307-flatten-switch-O2` 有 16 条 C return/out 对拍全部一致。这些是历史校准，不计为新运行。

旧成功完整状态参考在 `/home/cc/vmc-runs/ordinary-cff-predicate-20261114/real-replay-v2`：

- `RESULTS.json` SHA `cd8157572e85ac0d099673c6b123bbc0832a95e639482aa470856d40fc6ec6d6`；其中 Flatten O2 原有 16 次 UC/Python 参考和 O0/O2 各 16 次 LLVM native。
- `flatten-switch-O2/expected-{trial}.bin` 是该 ELF 的 16 份完整 66,136 字节参考。`expected-0.bin` SHA `2d5e3cae56faa5553242f2d2b4024a27f455c05f9cf858c1899fb548ca4c131e`；未来需要冻结全部 16 个 SHA，不能只用这一条代替完整绑定。
- `flatten-switch-O2/O0/input-{trial}.bin` 是相同 ELF/输入域的原始状态；driver `flatten-switch-O2/driver.c` SHA `e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb`。
- 旧 ready 计划 `/home/cc/vmc-runs/ordinary-cff-predicate-20261114/real-replay-ready-v3/replay-plan-READY-v3.json` SHA `593a12fac16c4475f60f883ffdb68710e6189533fadfdf7e049ebd4c520e9e78`，精确约束 raw/parser/driver/输入、原 1,000,000 Guest steps 和 O0/O2 编译方式。

完整比较为 **66,136=24 header +512 原始寄存器字节 +65,536 Guest +56 Exit（7×u64）+8 budget**；另核 76 个命名寄存器字段。不得复用 manual 的机器快照来代替 Flatten 的参考。既存 C16/UC16/LLVM32 只引用，不因复制或重审而累计为新执行。

历史 expanded 文件若已退休，先按对应归档 exact restore，不能假装旧绝对路径仍 live：

- `/home/cc/vmc-runs/practical-holdouts-B-20261113-v1.tar.xz` SHA `9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359`；payload manifest SHA `e34e8a645f1a685967bb7fad894c35ef5f0db27747ff56d4a23c0b2c8867d1b8`。
- `/home/cc/vmc-runs/ordinary-cff-predicate-evidence-20261114-v1.tar.xz` SHA `51798f581c7f523ff9160efd11b5b285bb19351480b68d861efb24dcef3fdadf`；Mac 对应 `/Users/c/Desktop/deob/validation-artifacts/ordinary-cff-predicate-20261114/ordinary-cff-predicate-evidence-20261114-v1.tar.xz`。

B 包内原始 canonical lift 成员是 `practical-region-holdouts-20261113/normal-flatten-switch/flatten-switch/2026111307/flatten-switch/O2/product-O2/unoptimized.ll`（747,549 字节，SHA `4315ddcd08fd7e26861197be9a550ca7a1eee685be4206cc56f8c828d07abc05`）。该 expanded 文件已不 live；未来 checked recipe 需要时只恢复这个成员和其准确来源，不复制完整旧命名空间。成员同目录 `raw.ll` SHA `15b75e16cbcf7cda4fcea10711b6749e9595efceaa2ae30ef77808824c6ce252` 是优化后 raw，不能冒充 canonical。

## 最小可运行 IR 基线

新脚本 `scripts/validation/progressive_deobfuscation_baseline.py` 使用现有 Python public shim，无需编译新 probe。旧 `practical_holdout_diff.py --frozen --family flatten-switch` 会连带两 C 优化级别、四计划和 native/UC，不适合本轮最小基线。

新入口只处理一个 frozen ELF：读取 manifest 的 strategy/符号，加载 ELF、disassemble、lift 全部有效函数（StaticAnalysis + FunctionStart）；保存原始 IR；运行原 B 的十步 pipeline 一次；保存完整报告与前后 IR；分别 `opt-21 -passes=verify`。原顺序为 mem2reg、instcombine、gvn、guest-memory、mba、predicates、dispatch-specialize、dispatch-deflatten、control-flow、dce，原 MBA/predicate 参数保持 20ms/64queries/128nodes/100000resource，MBA 3variables，pipeline 1iteration/16runs。GuestSSA/dispatcher 未显式配置项继续使用现有默认值。

此入口不执行 opt-in ImageTableContext/DecoderSSA/Corridor，三组件调用数明确为 0；这一阶段测普通根能力和残余，不能冒充 checked de-flattening。main/no-Z3 各一次，No-Z3 的 unavailable、Incomplete、unknown 和 budget 必须原样报告，不能强求其 predicate 输出与 Z3 相同。

输出包含完整 Graph 前后快照及 exact gate、未 lift 清单、真实 ModuleID 的 before/after、每 pass 全字段、每个函数的 CFG/SCC/环内 switch、runtime `/proc/self/maps` 与 LD_DEBUG。小 CFG 读取器仅接受当前生成 IR 的 br/switch/ret 方言；遇 EH 等未支持语法会保留 IR 和失败，不猜边。它只计实际语法边和环，不能把 cyclic-switch 数直接解释成业务 dispatcher 数。

以下准确命令已在根释放窗口后顺序执行一次；不可原路径重跑。初版源 SHA `3c399e7e327883836f341a41001974819990d39aa6f77462bc3f4304c15729ba`，部署到 `/home/cc/vmc-runs/progressive-deobfuscation-20261114-source/progressive_deobfuscation_baseline.py`，两端相同。`DEPLOYMENT.json` SHA `193c912e169cd91479a9b551ea508f24aaa746f669e6914bc5c16de01e1c6d5c`。源和所有初版结果保留，不因后续覆盖门而改写。

```sh
env PYTHONDONTWRITEBYTECODE=1 \
  DRAGON_C_LIB=/dev/shm/ordered-dispatch-production-v1/main/libdragon_c.so \
  LD_LIBRARY_PATH=/dev/shm/ordered-dispatch-production-v1/main \
  python3 /home/cc/vmc-runs/progressive-deobfuscation-20261114-source/progressive_deobfuscation_baseline.py \
  --frozen /home/cc/vmc-runs/practical-region-holdouts-20261113/frozen-normal \
  --frozen-sha256 0ec963329f95b987f93cc0de8f6700b8497068f76b88fdf7e7dbd49101d9f0db \
  --family flatten-switch --c-level O2 \
  --python-root /home/cc/dragon-tales-overlap \
  --backend /dev/shm/ordered-dispatch-production-v1/main/libdragon.so \
  --backend-sha256 2645b01d50cd9336a25ff4aadea87abe6ccc64ab615b2dca2063136bc7e610f8 \
  --shim /dev/shm/ordered-dispatch-production-v1/main/libdragon_c.so \
  --shim-sha256 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 \
  --opt /usr/bin/opt-21 \
  --out /home/cc/vmc-runs/progressive-flatten-O2-ordinary-main-20261114-v1
```

No-Z3 命令相同，将全部 runtime `main` 路径换为 `noz3`，backend SHA 换为 `80dc8fe64694ecbd836cf28fdaf05ad82770423f9e347055e22a9ca469c36b6c`，shim SHA 换为 `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`，输出换为 `/home/cc/vmc-runs/progressive-flatten-O2-ordinary-noz3-20261114-v1`。不带 LD_PRELOAD，不回落 owned build/RPATH。

资源估计而非测量：每套预留 16MiB 输出（完整两份 IR、Graph/CFG、日志）；无需复制库、输入 arena、生成 ELF 或 snapshot。预计单套数秒至数十秒；仍以完整 CLI 180s 截止为准。进程可能使用约 0.5–1GiB RSS，需实测；不以此估计放宽守卫。启动 512MiB、运行余量 64MiB、180s watchdog 已写入入口。完整 native 迁移不在本轮最低交付中。

本地准备检查运行了 stdlib AST 解析和对既存本地两份 Flatten IR 的 CFG reader 检查：历史 before kernel 360BB/1 个环内 switch；历史 after 484BB/51 个环内 switch。这是旧上下文副本造成的语法结构，不是本轮收益。根另用独立 Floyd 传递闭包核对全部512个三节点图及512个含重复 successor 的版本，SCC 全相符；3种坏 CFG（缺目标、terminator后仍有指令、未支持terminator）全部拒绝。收据 `/Users/c/Desktop/deob/validation-artifacts/progressive-deobfuscation-20261114-v1/LOCAL-COLLECTOR-CONTROLS.json` SHA `24032fa08b677a626e418de9eb6e891c8afc95e137a6c810cdafcf9c071da490`，已绑定初版源 SHA。它验证统计工具，不是产品语义测试。

## 本轮实际结果：普通根未覆盖业务分发

两次 CLI 退出0，watchdog 无触发，分别耗时 main `0.716788s`、no-Z3 `0.503093s`（守卫进程记录）。每套2次函数 lift、1次 pipeline、10次 pass，合计4次 lift/20次 pass/4次外部 LLVM verify；未修改 Graph。两套 unlifted 清单均为空。精确报告如下：

| 实际量 | main | no-Z3 |
|---|---|---|
| kernel IR BB / successor slots，前→后 | 65/65 →64/94 | 65/65 →64/94 |
| kernel IR 环内 switch，前→后 | 0→0 | 0→0 |
| kernel before cyclic SCC | 1个单块自环 | 同左 |
| source kernel Function 机器块数 | 3 | 3 |
| 中央 jmp 实际分支 / 独立表不同目标 | 0/75 | 0/75 |
| 表目标属于 source kernel Function | 1/75 | 1/75 |
| 显式 entry/case CFG coverage gate | **false** | **false** |
| 精度 / 语义完成 | **false / pending** | **false / pending** |

原始 Graph 解码了448条全局指令，kernel 的 Function 仅含 `0x1010,0x1120,0x1126` 三块。`0x1126` 指令字节 `ff24cd00200000`、`jmp [RCX*8+0x2000]`，实际 `branches=[]`。原始 IR 的 `bb_4390 → memory.valid57` 保留真实表 LOAD `%2052`，随后写完整 Exit：reason=TRANSFER(2)、kind=BRANCH(1)、source=4390、target=`%2052`、valid=1，并返回。唯一 kernel raw switch 是入口 PC 接受检查，不是中央业务分发；单块自环是范围检查 `0x1120`。因此 after 无环不是“移出业务循环”，而是原始普通根没有恢复表后的业务边。本轮应表述为 **已解码指令，间接边/函数覆盖未完成**，不能将零 cyclic-switch 当成功。

全 pass 原始报告保留。main 10步状态均SUCCESS，Guest-memory complete=1/forwarded=0/alias-barriers=966/loop-barriers=2，MBA visited173/queries0/simplified0/budget-skipped26，predicates visited35/queries64/folded1/unknown0/unsupported0/budget-skipped2，specialize和deflatten均0次改写。no-Z3 的 MBA/predicates 均UNAVAILABLE（无查询）；specialize改1条branch、0switch，switches-visited2/unresolved2；deflatten visited2/rewritten0/unresolved2。其余状态SUCCESS。两套 pipeline `budget_exhausted=false`、`converged=false`（仅执行冻结的一轮），没有状态INCOMPLETE；这不消除 skipped query 或机器 CFG 覆盖失败，也不能将 Guest-memory 的局部 complete 读成全样本完成。

可确定的边界与仍缺的诊断必须分开：`x86Analysis.cpp` 的 table 候选即便证出 bound，还要通过 `TableContents.cpp::provedTableTargets` 的内容证明；后者仅接受 producing LOAD 的 Guest STORE finite facts 或显式 `readKnownMemory`，ELF 字节不是内存事实。本轮未提供后者。初版 collector 未保存 IGNIL 文本或 SwitchCandidate 查询，所以无法断言运行时具体拒于哪个分支、是否 numCases=0 或哪一内部限额；不重新分析来补写初版证据。现有 public `detect_possible_switch` 仅返回 None 或 candidate（num_cases=0表示未证bound），没有详细拒绝原因字段。

原 B 的差别已只读核实：`scripts/validation/practical_holdout_region_diff.py` 实际使用唯一正长度 ELF STT_FUNC extent、`static_region_replay.select_region` 的已解码 leader，再调用 `lift_static_region(...max_blocks=4096,max_dispatch_cases=65536,rebuild_decoded_blocks=True)`。它私有重建缺失的机器块，保留动态 LOAD/未知目标退出，Graph 不变；不是 ordinary lift 自动恢复这些边。下一步应在环境就绪后明确执行这种 decoded-region 基线，并对私有 IR 的 actual case/decoded coverage 另设门，再评估 checked view，不能把表字节塞进 read_known_memory。

初版 `REPORT.json` 的 `precision_complete=null` 原样保留。新增 saved-only sidecar `progressive_deobfuscation_coverage_audit.py`（SHA `60451ef303a5e146e5bcf57d014eaa4a76fe47d8342be5744356ebb8f1ccf965`）使用独立静态机器 manifest，显式给出 `precision_complete=false`。manifest SHA `88838c61c0682d93f6279406df8af983dbaa4f1b86f1bca434d5139d80522481` 绑定 ELF/反汇编、实际 jump bytes、80个表槽和75个不同目标；只作为覆盖预期，绝不提供运行时 Guest facts。其检查前后重核两个15文件集合的完整 size/SHA，0新增 Dragon/native/reference 运行。

结果与证据绑定：

- main：`/home/cc/vmc-runs/progressive-flatten-O2-ordinary-main-20261114-v1/REPORT.json` SHA `7841acafe0a9c38f4b9df1b75a77583d115368da3660157fcb91bb27c2f00eb8`。
- no-Z3：`/home/cc/vmc-runs/progressive-flatten-O2-ordinary-noz3-20261114-v1/REPORT.json` SHA `ebc7c9b2c932ef5ad963252add119cffcfc403a16171e9c4f686e656d9b5a095`。
- 两套 before.ll 均 SHA `305d7a7e1aeef4ef53c81c68ed9308ec7bb733f6fc9f01d05eb29caf76ba5d93`；after main SHA `8cee06f1780765642dee3ad1e53075ffa4439deccc5556ea2797f01409da65cd`，no-Z3 SHA `1584cbb40c734a638fb7f9332f5282886b51092bc038c360f30fdc7e2787b7c0`，不做 normalize。
- coverage sidecar：`/home/cc/vmc-runs/progressive-flatten-O2-saved-audit-20261114-v1/REPORT.json` SHA `c6cacf9ebb623f3d1c1ccef467e23572607357e0969b5af4d9508f5c5666ee8f`。
- Mac 全部小证据镜像：`/Users/c/Desktop/deob/validation-artifacts/progressive-deobfuscation-20261114-v1/actual-ordinary-v1`。两套15文件 baseline 已逐集合/size/SHA相符，不包含旧参考或库的大复制。

四个小证据根（两baseline、source、saved-audit）共36个regular文件/951,020 logical bytes，已双端逐成员集合/size/SHA核验。VM fileset `/home/cc/vmc-runs/progressive-flatten-O2-baseline-FILESET-20261114-v1.json` SHA `5b27b63ffe0500685ada9550d5319d4b0667b65f0bde50b0d92d5a17418779c4`；双端收据 `/home/cc/vmc-runs/progressive-flatten-O2-baseline-DUAL-VERIFIED-20261114-v1.json` SHA `9d63d15d920b85f6b6f4a2335b7a9daf6ec113720743720f61f8e69cf0886d08`，Mac 为镜像根中的 `FILESET.json`/`DUAL-HOST-VERIFIED.json`。控制收据属于额外的小metadata，未追写已冻结的36文件集合。

saved-audit 另以 Mac 临时副本核对：原始有效证据被接受但仍返回 precision=false；同时改坏前后 Graph 的 jump bytes、改坏 before.ll、移除一个 saved 文件，分别被精确拒绝为 `jump bytes differ`、`observed raw IR changed...`、`unexpected saved file set`，3/3检出。临时副本已移除，未触碰原证据，0 Dragon/native/reference调用，没有把人工改 Graph 作为产品正例。收据 `/Users/c/Desktop/deob/validation-artifacts/progressive-deobfuscation-20261114-v1/LOCAL-COVERAGE-AUDIT-CONTROLS.json` 与 `/home/cc/vmc-runs/progressive-flatten-O2-LOCAL-COVERAGE-CONTROLS-20261114-v1.json` 同字节，SHA `76a826b092669efe485a28d03218e8ebb8e18995acf65b36514462aa3fdc57b3`。

## 后续 checked view 需要独立满足的前提

旧 `image_table_context_real_probe.cpp` 的 fixture 固定 manual 的 raw/ELF SHA、6 目标、SP/table 和特定 state LOAD，不能直接换 ELF 后称为通用测试。最窄适配应将输入 SHA、ELF 符号/入口、实际 jmp-index-table 数据流和 table slot/target 列表变为参数，保留通用 candidate provider、actual guard、fallback 和预算；不用 memory.valid 名字或地址挑“正确输出”。

1. 从本 ELF 机器结构重新发现 jmp、边界检查和表范围，再用 canonical raw 查实际 LOAD/index/target 角色；候选 manifest 只提供不可信预测。不能把 74 个 C case 直接当 LLVM 真值。
2. GuardedEntry 必须在实际 IR 检查候选寄存器/Guest 字节，错误上下文回普通根。SP 来自本 wrapper 和初始帧的调用契约，Flatten 的 frame depth 与 manual 不同，不能复用 manual 的中间 RSP 常量。SP8+表640 共648bytes，在原4096bytes cap内。
3. 当前 Image core 只消费已检查 SP/table 事实。Flatten 在循环内输出 STORE 的地址必须证明不覆盖后续读取的表，否则保持原读/拒绝。可在未来独立方案评估 executable out/RSI guard，但当前 core 没有通用 checked-register fact 消费器，不能仅添加 guard 后手写替换 RSI，或偷偷缩小任意 Guest 语义。
4. Flatten 的 selector 是 64 位 RCX 状态，不保证存在 manual 的 32 位 Guest state LOAD。CellSSA 若不适用应明确标 N/A，不能强选某个 LOAD；DecoderSSA/Corridor 仅使用实际已闭合的 SSA 和逐 Use 的 fresh/history 证明。
5. 默认 Image 上限4096blocks/65536instructions/1,000,000work，Decoder256nodes/128phis/512instructions/16384work，Corridor512candidates/64rewrites/64clonedblocks/16384instructions/262144repaircells/1,000,000work 均保持。80 槽小于 Image 的512目标硬上限，但75不同目标可能触发64克隆预算；灰边/非法状态回路也可能使 provider 不闭合。这些都保留为实际拒绝或 Incomplete，不提预算、删坏路径或追着 actual 修改 expected。

## 层级与完成判据

| 层级 | 此阶段要证明什么 | 本文状态 |
|---|---|---|
| 0：manual 手写组合 | 已交付的条件业务 CFG 与完整状态语义，原输入/失败均封存 | 历史已完成，不重计 |
| 1a：单项 Flatten O2 普通根 | 当前 main/NZ lift/verify、原十步预算报告、残余 CFG；来源/Graph不变 | 两套已执行；IR合法，间接边覆盖失败，精度未完成 |
| 1b：同 Flatten 的 checked recipe | 真实 guard/安全 fallback、table写闭合、三组件逐项报告；业务循环结构验收 | 尚未适配/执行，可能 Unsupported/Incomplete |
| 1c：同一新 IR 的完整语义 | 新 original LLVM32 校准，再 after32；严格引用旧 C16/UC16，不重数 | 待1b及完整绑定 |
| 2：MBA 与 opaque 叠加 Flatten | 仍用同一业务并冻结实际 producer 配方，先单项后组合 | 未生产/未测试；不能由“开了 pass”冒称样本含保护 |

结构完成必须基于实际业务种子到目标的 CFG：在 guard 成立的视图中，业务循环应闭合，中央 target LOAD/dispatcher 不再属于这些业务循环 SCC；原业务 STORE、真实条件、错误侧出口和 guard miss 路径必须保留。只增 copies、某个入口首轮绕过中央、仅 cyclic-switch 数减少，或删除共享业务块后看起来无环，都不算完成。若原 dispatcher 仍在循环中，明确写 partial；保留最短实际 witness。

语义完成仍需要独立 oracle。迁移时全部16输入、ELF/driver/raw/parser/ModuleID/stage literal/hash、初始完整 frame 和原限制先冻结；新 raw 若来源不同，先原始 LLVM 在 O0/O2 的32次完整快照对旧16份 UC 参考校准，全部通过才 after32，配套每32比较组96个既有 comparator controls。参考已有不等于新 IR 已对拍；本轮 verify 和静态 CFG 不能替代这一步。logical-size/guard-miss 若需补域，应独立标 LLVM peer 校准，不称 Unicorn 已模拟逻辑 size fault。

本轮交付是 1a 的双后端 IR 基线与显式覆盖失败；不会把 verified、pass.available、complete 或副本计数写成已经完成去平坦化或语义验证。旧 VM 不再新增 target 运行；后续 decoded-region 基线等待 ROCK 独立构建和根的新窗口。

后续 ROCK5B `192.168.10.241` 可承担独立构建和更多语料：已知为 aarch64、8核、32GB RAM、Ubuntu22.04/glibc2.35/GCC11，空间充足；当前缺少 CMake/Ninja/LLVM21，环境尚未就绪。不能直接将旧 VM 的 Ubuntu/LLVM21 二进制视为兼容，也不能将该机器计入已完成测试。本轮继续使用旧 VM 的已冻结实际库。
