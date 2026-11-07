# x86 有效地址宽度与线性内存地址

2026-11-07，基线 `72c37b8`。本阶段修复普通 x86 内存访问没有按地址宽度回绕，
以及将有效偏移宽度混用于线性内存跨度的问题。修改限于三个 x86 私有 lifter 文件；
没有公共 C/C++/Python ABI、Guest 字段、IGNIL opcode 或 Graph 恢复策略改动。
地址 IR 变正确后，依赖它的常量、别名和间接目标分析结果可能随之改变。

主库 `223b1239…`、无 Z3 `c408fe2f…` 的组件、原生、参考抽查、全语料及有限 Tigress
验证均已完成，测试快照保留于 `candidate-v1/`。最终 Git 状态另由发布 receipt 记录。
验证完成不代表所有程序语义或去混淆覆盖完整。
上一阶段的[段内存回退](x86-segmented-memory.md)保留为独立历史记录。

## 已确认的错误

一个不依赖 CALL 或 image 初值的反例：先向 `0x100000004` 写入 `0x1060`，
再在 `EAX=0xfffffffc` 时执行 addr32 `STORE [EAX+8],0x1040`。第二次写入的有效地址
必须回绕到 `4`，不应覆盖第一个槽。随后读取第一个槽并间接跳转，CPU 目标为 `0x1060`；
旧有限值分析却给出 `complete=true` 和 `0x1040`。本阶段用真实 Guest STORE 的回归
在两种遍历策略中区分这两个地址，修正该错误目标。i386 addr16 有对应的 `4/0x10004` 反例。

无基址/索引 LEA、EIP-relative、moffs、scratch 改写、BT 位串和内存 POP 还要求保留原始
指令的地址上下文。只修某一个 MOV 或根据结果寄存器宽度猜测地址宽度，不能覆盖这些形式。

## 地址计算契约

私有 `AddressContext` 分开保存 `effectiveWidth` 与 `pointerWidth`：

| 模式 | 默认有效偏移宽度 | `0x67` 后 | 线性 Guest 指针宽度 |
|---|---:|---:|---:|
| x86-64 | 64 | 32 | 64 |
| i386 | 32 | 16 | 32 |

地址大小规则参见 [Intel SDM Volume 1 §3.3.7](https://cdrdv2-public.intel.com/819711/253665-sdm-vol-1.pdf)。

上下文由完整机器指令的真实前缀区取得，随后传给所有支持的内存处理器和 scratch 改写。
扫描遇到操作码即停止；立即数或位移中的 `0x67` 不改变地址宽度。32 位模式中的
`0x40..0x4f` 也不能当成 REX 前缀继续扫描。

普通内存操作先计算：

```text
effective_offset = (base + index * scale + displacement) mod 2^effectiveWidth
linear_pointer  = zero_extend(effective_offset, pointerWidth)
```

`emitEffectiveOffset` 在有效偏移宽度内运算，`linearizeOffset` 再扩展到线性指针；
`emitAddress` 组合两步。数据操作数宽度与隐式栈宽度独立于这两者。LEA 只取有效偏移，
按目的寄存器宽度显式扩展或截断后写回，不执行内存访问，也不引入段基址。

未知地址寄存器不能当成零。只有 NoRegister 和 LLVM 的 EIZ/RIZ 伪零寄存器允许省略；
无法表示的真实 base/index 在 scratch 改写之前进入已有 unsupported 回退并登记
`unlifted`，x87 专用路径保持对应检查。FS/GS、i386 显式非平坦段的拒绝规则继续生效。
这不是段描述符、FS_BASE/GS_BASE 或内存异常模型的实现。

### 需要分别处理的形式

- **RIP/EIP-relative：** 私有 EIP 视图与 RIP 共用寄存器偏移。每条指令先安装完整
  `address + instruction.size()` 的 next-PC，再按有效宽度读取、加位移和回绕。
  因而高于 4 GiB 的代码地址、跨 4 GiB 的 next-PC，以及同一 trace 内多条相对寻址均不复用旧 PC。
- **moffs：** LLVM 的布局是 `immediate offset + segment` 两项，不是普通内存五元组。
  地址寄存器门禁须先识别这个布局，不能把合法 A0–A3 全部误拒；偏移按原指令地址宽度
  处理，累加器的数据宽度仍独立。
- **BT/BTS/BTR/BTC 寄存器位索引：** 先在 `max(indexWidth,effectiveWidth)` 中保留有符号
  索引，必要时 SEXT，再做算术右移求 word 和 byte delta。最后才把 byte delta 截到
  有效宽度并加到偏移上，然后线性化。过早把 32 位索引截成 16 位、或 64 位索引截成
  32 位会选错 word。编码为 imm8 的位索引仍按 word 内索引处理。
- **PUSH/POP 内存：** PUSH 的显式源 EA 和读值在 SP 改动前取得；POP 先从旧的完整
  SP 读取、按数据槽大小更新完整 SP，再计算显式目的 EA。`0x67` 不能把隐式 RSP/ESP
  也截成地址操作数的窄宽度。scratch 回写使用先前捕获的同一地址，并完整恢复 scratch 寄存器。
- **宽访问与字符串/x87：** 有效偏移只决定访问的第一个字节。例如 x64 addr32
  `LOAD32` 从 `0xfffffffe` 开始，后续字节可到 `0x100000001`；不能逐字节回绕到 0。
  SSE 的第二个 qword 使用线性指针宽度的 `+8`。字符串索引/计数在地址宽度内更新，
  每次访问再线性化；REP 零次保护和读取先于写入的顺序保留。x87 共用该地址上下文，
  宽 LOAD/STORE 的内存跨度不缩成有效偏移宽度；未知地址的 UNDEF 也使用线性指针宽度。

这次只修现有已支持形式。LEA16、word PUSH 等原先未支持的形式仍按 `unlifted`
处理，不能从“支持 addr16 内存地址”推导出所有 16 位数据指令都已支持。

## 已完成的验证

### 回归与稀疏地址参考

同源测试在旧实现上，10 个用例中 8 个失败，共 75 个失败断言；新实现 10 用例 /
10087 断言通过。无 Z3 的三个组件用例 / 212 断言通过。主 CTest 642/642、无 Z3
510/510 通过。

独立 Unicorn 与 IGNIL/Z3 的稀疏地址 trace 包含 76 个配置（x64 51、i386 25）：
旧库 35 个差异 / 41 个匹配，新库 76 个匹配。它覆盖窄地址回绕与高地址干扰槽、LEA、
EIP-relative、moffs、scratch、BT、POP、访问跨度及 x87 地址上下文；这是 trace 级证据，
不能称为 76 个 LLVM 函数或 JIT 根返回。

### 完整原生根返回

26 个永久机器码夹具 × O0/O2 × 8 输入 × 六个 mode/strategy 组合，共 2496 个完整
根返回与 Unicorn 匹配，312 份根 Graph 报告无 unlifted。六组合是 V2、Static、product
各配两种遍历策略。比较现有模型的全部 76/52 个字段、PC/SP、64 KiB 内存和完整的
RootReturn 退出契约。主六矩阵对应 312 个可执行文件；另有 8 个负对照及 4 个旧库新增
CALL/JMP 对照，合计 324 个 ldd 已核对。不能把 324 全部归入 2496 个新主库根返回。

无 Z3 另跑 **product-s0 单组**：416 个完整根返回、52 份 Graph 报告及 52 个 ldd 均通过。
它不代表无 Z3 也重跑了六组合。两部分合计 2912 个完整根返回。结果单比特与内存单字节
负对照各 32 次，均检出精确差异，负对照原生输出与正常 product-s0 的原始 snapshot
逐字核对。新原生矩阵没有增加排除、跳过或归一化。

旧库对照共 416 项，须分开解释：原 24 个地址夹具的 384 项先在**本阶段**使用旧库
fresh 执行并冻结，再与后增两个内存 CALL/JMP 夹具的 32 项旧库 fresh 执行合并审计；
并非复用上一阶段的 native 比较。原 24 夹具另对应 48 个旧库可执行文件，与上述 324
及无 Z3 的 52 分开计数。合计 128 个完整根匹配、32 个完整根的
RAX 差异，以及 256 个 MemoryBounds 停止边界差异。不能把 288 个旧差异全部称为
“完成执行后算错”，也不能把停止边界计入新库的完整根成功。

正式组件入口为 `native-final-audit.json` 与 `native-noz3-audit.json`；前者的旧 24 和
新增 2 项夹具哈希分别保留，不能混用其输入身份。

### 既有参考脚本与盲区

`reference-audit.json`（`5c3f4567…bc5322`）核对九份执行 receipt 的实际双库 maps、
原始日志、脚本与输入身份。以下计数沿用原始单位：

| 检查 | 已记录结果 | 不能据此声称的覆盖 |
|---|---|---|
| x64 单指令 | 986 ok 模板、0 差异模板、6 unlifted、0 汇编失败；TRIALS=8 | fault 试次及逐字段排除没有汇总，7888 只是计划/尝试量，不能写成 7888 完整比较或 0 fault |
| SSE 单指令 | 393 模板、每模板固定 60 次，共 23580 参考试次；0 差异 / 0 unlifted | 环境 TRIALS=8 不覆盖内部 60 次循环；700 是 NaN 结果未知字段数，不是 700 个跳过试次或全位匹配 |
| i386 真实块 raw / simplify+DSE | 每次 40 块、305 比较、15 reference fault；24 call/system 跳过、1 unlifted；22 次范围外写未比较 | 两次是相同样本的两种处理，不是新的独立输入；内存比较限已知区域 |
| x64 真实块 raw / simplify+DSE | 每次 40 块、298 比较、22 reference fault；23 call/system 跳过、1 unlifted；37 次高地址内存未比、23 次范围外写未比 | 40×8 不等于 320 个完成比较，也不是完整函数或全内存语义证明 |
| i386 Legacy LLVM 真实块 | 新、旧各 24 块，均跳过 2 个 Unicorn fault 和 1 个 unlifted，均保留 1 DIFFERENT | rc0 不等于无差异；原始新旧 stdout 逐字一致，不能改写为 0 DIFFERENT |
| x64 Legacy LLVM 真实块 | 24 块、4 个高地址跳过、1 个 Unicorn fault，报告 0 DIFFERENT | 跳过与未知字段仍限制覆盖，不能概括为所有块全状态通过 |

i386 `0x43319d` 的 FLD/FXAM/FNSTSW 差异与上一阶段相同。原始 raw80、寄存器、标志、
内存页、TOP/TAG/CW/SW 和机器字节已按冻结 seed 重建，与之前 Intel SDM + Rosetta 的
独立证据吻合：Dragon/Rosetta 为 EAX=`0x10000000`、CF/SF=`1/1`，Unicorn 为
`0x10000400`、`0/0`。分类仍是已确认的 Unicorn FXAM unnormal 参考缺陷，保留该
1 个原始差异，没有新排除或归一化，也不把它列为待定位的 Dragon x87 错误。

这些真实块脚本仅比较具体结果、布局内已知写入的 qword；Legacy 还省略 UNDEF 来源字段
和多位移位 OF。既有 x87 helper 的 CW 保留位归一化及 FCOMI 参考修正没有扩展，
此 FXAM 块也不涉及 FCOMI。未逐字段记录的 unknown/排除数量不能填零。

## 全语料执行与资源恢复

冻结 69 个输入已全部执行完毕：基线、新库各为 69 输入 / 138 策略的 fresh Graph
inventory。新库使用 unchanged `check_one.py`、`LLVM_MODE=v2 LLVM_OPTIMIZE=0`、
chunk 128，覆盖 205406 个有效函数检查点和 1675 个 raw 模块。旧库 LLVM 没有重跑，
旧阶段 raw 复用为零，本轮没有全 static/optimized 语料验证。

运行比较记录中，旧新 unlifted 总数均为 233134；警告均为 6 个 missing IGNIL、
26 个通往未解码地址的边、3 个 invalid function candidates、0 个 empty symbol
functions、146 个 objdump missing。60 个策略图的 canonical hash 改变，涉及 3440 条
kind-2 IGNIL block 记录；3796 条 PC 级差异记录均严格归因于同宽字符串指针 COPY 的
插入及其 temp 重编号。硬件指令、CFG、目标、函数成员、unlifted 和警告变化均为零，
没有未解释的改写。

独立终审 `corpus-forward-audit.json`
（SHA256 `f1c57a8082fbefe75d954809f061a68f3ad1a33e4d8c34422d938ef7332da05e`）
核对 255 项源码身份、完整定义集合、覆盖、verify 输出、保留差异和恢复证据，审计通过。
基线 canonical gzip 已由冻结 producer 完整比较后按 receipt 删除，成功 raw IR 也按原
checker 的临时目录规则删除；终审没有重新计算不存在的完整指纹或再次 verify 全部原始 IR。
其独立性来自保留 payload、计数/类别关系和 provenance 的复核，不是全语料 CPU 等价证明。

这不是单进程无中断跑完的 69 项，也不能报“零基础设施中断”：

- 前 21 项完整通过后，第 22 项启动 worker 前触发原有 512 MiB 根盘空间门槛并停止。
  随后的普通 resume 按设计拒绝了已创建但完全空的 `001` attempt；没有 worker 执行被漏计。
  原始停止、拒绝日志和 recovery review 均保留，显式 `--retry-failed` 从 `002` 继续。
- 65 项完整通过后，第 66 项 `zstd_gcc_x64elf_O0` 的新 worker 被 SIGKILL（-9）。
  内核日志在对应时间记录 Python OOM kill；冻结 v1 没有保存 child PID，故不能宣称
  已凭保存的 PID 建立一对一身份链。失败/partial、原 journal 和恢复收据保留。
- 失败时的 362429756 字节 IR 单独用 unchanged `opt-21` verify 得到 rc0、空 stdout，
  与成功重试中的 recursive 模块 8 的 hash/size 相同；这只证明该文件合法，不算整个
  输入通过，也不额外计入唯一成功模块。失败 attempt 保留的 22 份已验证 partial 模块
  （14 linear、8 recursive）单列，不加入 1675 个唯一成功模块。
- 已完成 attempt 文件经逐 size/SHA 核对后先迁至 tmpfs、再迁回磁盘，逻辑路径与证据中的
  物理路径通过符号链接保留；最后将 `TMPDIR` 指向磁盘目录，仍使用冻结 v1 worker、
  原 plan `1db3302a…ee3a08`、原 checker、chunk、预算和门槛。第 66 项重试两策略通过，
  后三项随后完成；剩余 62 个模块的真实 opt 输入路径全部位于磁盘 TMPDIR。没有发布
  observer v2，也没有放宽任何检查。

observer 的整模块文本副本会跨实际 opt 调用存活；checker 自身也持有 IR 文本，tmpfs
文件与 LLVM 对象会叠加内存压力。本阶段通过存储资源调整完成原版本验证，没有改写这些
生命周期。前 65 项在恢复时按同一阶段 source/plan/evidence SHA 复核使用，属于本阶段
已完整执行的 fresh raw，不能误称为旧阶段 LLVM 缓存。

## 有限 Tigress 兼容性重放

六个冻结 ELF（f-mba、f-opaque、f-combo 各 O0/O2）、双策略、每例 24 输入，
仅 product-O0 / Static FunctionStart 单计划，共六组。每组 48 次 C 匹配和 48 次完整
LLVM 根返回，合计 **288 C / 288 LLVM 完整匹配**；CFG wrong、unlifted、
executed-undecoded 和 incomplete 均为零。

重放使用 unchanged 冻结链，真实执行 Guest megaInit 与分配器，不注入初始化 heap
snapshot。每组外层 rc0 后，独立审计核对 48 份完整 snapshot、全部建模字段、
RootReturn 七字段协议、PC/SP 和代码不变，再生成 gate、逐成员 SHA 封存和 owned raw
清理 receipt。完整比较的是 **1 MiB** 内存，其中 heap 为 64 KiB；不能误报为总共只比
64 KiB。六组的审计、归档和清理证据位于 `tigress-smoke/`。

本轮没有重跑普通 goto/indirect，也没有六个 LLVM 计划的矩阵，不能将旧阶段的 960 个
incomplete 借作本轮结果。这是既有去混淆链的兼容性补充，没有新增去平坦化或堆分析能力。

旧阶段 14 条 TLS marker 后 switch 目标撤回是 `(输入,PC,策略)` 观测，涉及 4 个 Lua ELF /
8 图、去策略为 7 个输入内位点。独立终审逐条确认它们仍未解析、指令记录不变，
没有因本修复恢复。此前撤回的通用 opaque memory snapshot 原型没有随本修复恢复或合并。

## 后续仍需实现

1. **表内容证书：** guard 证明索引范围，不证明 image 表的数据未被 STORE 改写。
   绝对/相对表需要独立的内容来源、alias 和不可变性证明；这是后续恢复 post-opaque
   读取关联之前的必要工作。不能把固定代码字节或只读 `readMemory` buffer 当作 Guest
   数据不可变证据。
2. **Legacy 整函数拒绝边界：** V2/Static 和 Legacy trace 已检查 unlifted，但 Legacy
   整函数仍可能生成部分 IR。本轮没有给该入口加入统一拒绝；调用者须检查 Graph/Block
   marker，不能把部分 IR 当成完整 CPU 语义。

通用堆初始化分析、跨任意调用的参数指针 alias、去平坦化、非线性 MBA、通用 VM
去虚拟化和二进制重写均未由本修复实现。支持正确地址运算是这些分析的基础，不等于
完整自动去混淆。

本阶段证据与库快照位于 VM `~/vmc-runs/x86-addressing-20261107/`、Mac
`/Users/c/Desktop/deob/validation-artifacts/x86-addressing-20261107/`。组件快照、完整语料
证据、独立终审、有限 Tigress 和资源恢复记录分别保留，旧阶段冻结档案不回写。
