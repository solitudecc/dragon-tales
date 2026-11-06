# 外部 API 调用的分析能力审计

2026-11-04；基线 `060733c`。在 [Tigress 外部 API 研究](tigress-external-api-validation.md)
（x64 ELF、Guest DSO）之外，补测真实程序中的导入调用（PE/ELF，i386/x64/AArch64），
以及 i386 stdcall、Win64、AArch64 PLT 的 Guest 执行语义。发现并修复两个分析缺陷；
另外记录若干测量到但**未修改**的精度限制。

## 1. 真实二进制审计（`external_call_audit.py`）

真值不依赖 Dragon：

- 导入槽：pyelftools 的 JUMP_SLOT/GLOB_DAT 重定位（含可被抢占的已定义符号），pefile 的 IAT。
- 调用点：objdump 自己的列表。`<x@plt>` 存根、`jmp *slot` thunk 及其跳转链（不动点，最多 8 层）、
  `call/jmp *slot`（含 `rex.W`/`bnd`/`notrack` 前缀）、`mov slot, reg; call *reg` 及
  AArch64 `adrp/ldr/blr`（向后最多 10 条直线指令，寄存器被改写即放弃）。
- 函数范围：objdump 的符号头。noreturn API 用固定名单（exit、abort、`__stack_chk_fail`、
  `__assert_fail`、longjmp、`__cxa_throw`、`_Unwind_Resume`、ExitProcess 等），只用于分类统计。

每个调用点检查：是否解码、直接调用的 callee/branch 边、返回型调用的落空块是否在同一函数、
noreturn 调用后的边，以及调用方函数 StaticAnalysis 降级后的 `!dragon.imports` 来源
（每个记录都判定：正确 / 名称错误 / 真值中不存在的多余记录；真值点缺记录计为缺失）。

负对照（`--control`）：`names` 把所有期望名称改错，Lua x64 PE 上 106/106 判为 wrong；
`fallthrough` 把落空地址加 1，398/398 返回型调用判为 continuation missing。
初版落空检查对块中间的调用只比较所在块，对照只抓到 33/405，已改为逐条指令比较后才使用。

语料：VM 上两个语料库全部 69 个样本 × 线性/递归两种策略，降级所有含调用点的函数。

### 修复前后（`060733c` 快照 vs 修复后）

`!dragon.imports` 记录数，两种策略合计（同一构建上 CFG 统计完全相同；修复 2.1 只改元数据）：

| 平台（样本数） | 正确 前→后 | 名称错误 前→后 | 多余 前→后 | 缺失（不变） |
|---|---:|---:|---:|---:|
| AArch64 ELF（31） | 105043 → 105127 | 84 → **0** | 219 → **0** | 12456 |
| x86-64 ELF（16） | 21793 → 21803 | 10 → **0** | 68 → **0** | 2 |
| x86-64 PE（12） | 16888 → 16888 | 0 | 80 → **0** | 94 |
| i386 PE（10） | 13874 → 13874 | 0 | 0 | 56 |
| 合计 | 157598 → 157692 | 94 → **0** | 367 → **0** | 12608 |

修复前 138 次运行中 62 次有错误或多余名称；修复后 0 次。原先错误的 94 项变为正确，
没有任何原本正确的名称丢失。

### 修复后各平台（含两项修复的最终构建）

线性 / 递归两种策略分列（`a/b`）：

| 平台（样本） | 调用点 | 未解码 | 返回型后继 ok / missing | noreturn 落空边 | 合并 | 降级函数 | 来源 正确 / 缺失 / 错误 / 多余 |
|---|---:|---:|---|---:|---:|---:|---|
| AArch64 ELF（31） | 118145 | 0 / 2779 | 43249/848，42752/4 | 9890 / 9201 | 0 / 0 | 20551 / 20463 | 105115 / 12456 / **0** / **0** |
| x86-64 ELF（16） | 20404 | 4 / 103 | 7194/62，7169/7 | 1952 / 1951 | 0 / 0 | 5327 / 5314 | 21771 / 2 / **0** / **0** |
| x86-64 PE（12） | 17850 | 0 / 480 | 7023/46，7023/0 | 333 / 333 | 0 / 0 | 3602 / 3600 | 16798 / 94 / **0** / **0** |
| i386 PE（10） | 13834 | 0 / 15 | 5800/6，5790/2 | 283 / 283 | 4 / 2 | 3049 / 3047 | 13874 / 56 / **0** / **0** |

直接调用 PLT/thunk 的边缺失 0；lift 失败 0。与只含修复 2.1 的构建相比，x86-64 PE 线性
continuation missing 14→46、递归未解码 434→480、来源正确数 16888→16798：这是修复 2.2 不再解析
部分跳转表后 case 块离开函数的结果（§2.2），AArch64 因 sqlite O3 多解析一表而略增。

字段：sites 为真值中经导入槽的全部转移（含 PLT 存根自身的跳转）；cont 为返回型调用的
后继；noreturn 列为 noreturn API 调用后 Dragon 仍有落空边的数量（Dragon 不按名字假设
API 不返回，见 §4）；LLVM 列按记录计数（一个调用点被多个函数降级会计多次）。

## 2. 发现并修复的缺陷

### 2.1 导入来源把落空边和非跳板块标为导入转移

`LLVMImportMetadata.cpp` 把块末尾的落空 IGNIL JMP（`lea`、`nop`，或
`bl __stack_chk_fail` 之后）当成转移，并把任何无 CALL/STORE/CJMP 的块当成跳板。
实例：Lua x64 PE `0x140018e49 lea rdx,[rdx+1]` 被记为 memcpy；zlib A64
`0x223c bl __stack_chk_fail` 同时记为 `__stack_chk_fail` 和 `compress2`（落空进入
`compress: mov w4,#-1; b compress2@plt`）；sqlite A64 `bl sqlite3_free` 记为 fclose；
C++ `call _ZdaPv@plt` 同时记为 `_ZdaPv` 和 `_ZdlPvm`。只是诊断元数据，不改变执行语义，但给出了确定的错误名称。

修复：只有机器 call 指令处的 CALL、机器 jump（非 call）处的 JMP 是转移；跳板除临时值外
只能写 PC（RIP 相对寻址）与 AArch64 X16/X17。回归测试
`Import provenance names only machine transfers into pure thunks`（三例：x64 落空进入纯跳板、
x64 跳入先改 RCX 再尾跳的块、A64 `bl` 后落空进入 `mov w4; b stub`）在旧代码上 3 项断言失败。

### 2.2 反向切片把 CALL 当作保留所有寄存器

`BackwardSlicer::sliceReg` 遇到无 dst 的 CALL 直接越过。`mov eax,5; call f; mov ecx,eax`
切出 ECX=5；i386 `push 7; call stdcall_f`（`ret 4`）后切出 ESP-4。端到端：AArch64 递归
遍历 `mov x0,#0x1060; bl f; br x0`（f 返回 0x1050）把 `br x0` 解析为只跳 0x1060——
确定的错误 CFG 边，正是 `dlsym`/`GetProcAddress` 返回函数指针的形态。x86 同形态只因
`collectTargets` 不直接接受 `ZEXT(const)` 而未发布；它在 C++ `useZ3ForPathDiscovery`
打开时改用 Z3 枚举，这一组合未实测。

修复（经仓库所有者确认采用严格方案）：CALL 之后寄存器为该调用点的独立未知值
`SymReg{reg, origin=(块, CALL 序号, context)}`；同一次调用后读两次是同一值，调用前的定义不再追踪。
不假设被调方保存 callee-saved 寄存器，这与[静态分析契约](llvm-static-analysis-contract.md)
拒绝未经证明的“保持被调用者保存寄存器”一致。回归测试
`BackwardSlicer - registers are unknown after a call`（同块、跨落空边、i386 stdcall ESP）和
`Recursive A64 resolution does not carry a register across bl` 在旧代码上 4 项断言失败。

精度代价（两语料 69 样本 × 2 策略，`table_truth.py`/`truth_rel.py`）：

| | exact | wrong | unresolved | 无可证明界 | 不可读 |
|---|---:|---:|---:|---:|---:|
| 修复前 | 1124 | 0 | 116 | 558 | 42 |
| 修复后 | 1084 | **0** | 192 | 526 | 38 |

减少的 42 个全部在 x86-64（Win64 PE 的 Lua/sqlite/C++、clang x64 ELF Lua/cJSON）：表基址或索引
在 RBX/RDI/RBP/R13 等 callee-saved 寄存器中跨调用保持，例如 `luaK_infix`
`call luaK_dischargevars; cmp ebx,0x14; ja; movsxd rax,[rdx+rbx*4]`——调用后 RBX 高 32 位未知，
EBX 的比较不再界定 RBX。sqlite A64 O3 每种策略多 1 个 exact（净 −40）。Lua 六项：
i386 24/24、A64 7/7 不变，x64 PE 21→17。恢复这部分精度需要对已分析的内部被调函数
**证明**其恢复 callee-saved 寄存器的摘要，不能按 ABI 直接假设。

## 3. Guest 执行语义（`external_guest_diff.py`）

沿用 `function_guest_diff.py` 的 Unicorn 对拍（全部寄存器字段、64 KiB 内存、退出记录，
无归一化），fixture 见 `tests/fixtures/llvm-external-guest.json`；导入槽中保存 Guest 实现地址，
两边执行同一份 API 机器码。每个镜像都检查加载器导入名与根函数的 `!dragon.imports`。

| 用例 | 检查点 |
|---|---|
| `x86_32_import_stdcall` | i386 ELF 导入槽，API `ret 8` 弹出参数，调用后读 ESP |
| `x86_64_import_clobbers` | API 改 RCX/R11/XMM0 以及 callee-saved RBX（非 ABI），调用后经 RBX 存储 |
| `x86_64_import_returns_pointer` | 调用前 EAX 为诱饵 0x1300（无代码），API 返回 0x3000，随后 `call rax` |
| `x86_64_pe_win64_home_space` | PE IAT，API 写 home space `[rsp+8]`，调用方读回 |
| `arm64_import_plt_clobbers` | AArch64 PLT（X16/X17），API 改 x1/x2，调用方存 x0+1、x1、x16 |

V2 O0/O2 各 8 次：80/80 完整根返回匹配；`--corrupt-output` 80/80 different。
`--static`（产品优化管线，静态函数在 API 入口 0x2000 处为声明边界）：80/80 边界匹配，
负对照 80/80 different。`060733c` 快照上同样 80/80 + 80/80：执行语义本来正确，本次的
两个缺陷都在静态分析侧。CTest 增加两项（各 2 次）。

i386 PE 没有加入：`function_image_fixture.py` 只有 x64 PE 模板；i386 stdcall 用 i386 ELF
导入槽验证同一机器语义，导入名识别由 §1 的 10 个 i386 PE 样本覆盖。

## 4. 测量到但未修改的限制

- **noreturn API 后的落空边。** Dragon 不按名字假设 API 不返回（这需要 API 效果模型，属于
  设计问题），所以 `call __stack_chk_fail` 等之后仍有落空边：进入下一函数入口、进入有其他
  前驱的块（伪前驱）或块内继续。这让 CFG 偏大，不产生错误值；函数边界被合并只有 i386 PE
  C++ 的冷代码段（经内部 `terminate` 连续落空），线性 4 / 递归 2。
- **returns_twice。** `call _setjmp` 后 GCC 放置 `endbr64`，被当作函数起点，
  `luaD_rawrunprotected` 在 0x1305b 被拆开。GCC x64 ELF 的 Lua 每份 1–3 处（clang 0）；而紧跟
  noreturn 调用之后、以 `endbr64` 开始的真正函数，抽查的 9 个 x64 ELF 每份 0–21 处。没有导入名就无法区分，未改启发式。
- **缺失的名称都是保守结果。** 抽查各样本记录的缺失项：A64 全部位于同一块内更早的未 lift
  指令之后（系统库为 PAC `paciasp`/`autiasp`，sqlite 为若干窄/向量访存形式），来源追踪按设计在
  第一个未 lift 指令处停止。x86 的几个缺失是导入指针被提升到循环外（`mov __imp_toupper,%r12`，
  循环体内 `call *%r12`）；来源证据按设计不跨 CFG join，且循环回边本身经过该调用。
  另外 libstdc++ 线性策略约 1500 个调用点不在任何 Dragon 函数中（未细分原因）。
  没有发现错误或多余名称。
- 递归策略的 undecoded 是未到达的代码，不是解码错误。

后续 [CET 边界修正](cet-function-boundaries.md) 不再把单独的落点当作函数证据，
保留调用、元数据、完整帧和 RET 后布局证据；它没有增加导入名或 noreturn/returns_twice 模型。
上述为本次审计当时的冻结数据。

## 5. 复现

```sh
# VM, 构建目录 build；两语料
DRAGON_C_LIB=$PWD/build/c_shim/libdragon_c.so python3 scripts/validation/external_call_corpus.py \
    validation-results/external-audit ~/bench/corpus ~/bench/corpus2
python3 scripts/validation/external_call_audit.py BIN OBJDUMP --strategy 0 --llvm 40 --control names
~/ucdev/bin/python3 scripts/validation/external_guest_diff.py --trials 8 [--static] [--corrupt-output]
```

VM 产物：`~/dt-agent-all/validation-results/external-audit-old`（`060733c` 快照库）、
`external-audit-new`（仅修复 2.1）、`~/dragon-tales/validation-results/external-audit-final`
（两项修复），每个样本/策略一份 JSON；跳转表 `~/tables-{before,after}.json`。
修复前快照库在 `~/snap-060733c`（`LD_LIBRARY_PATH` 同时指向其中的 `libdragon.so`）。

CTest：504/504（Z3），355/355（无 Z3）。切片验证脚本不变：`slice_diff` 3000/3000、
`memory_slice_diff` 448 次 / 192 个切片 0 错、`loop_slice_diff` 128 / 4 0 错、
`guard_truth` 27 CFG / 20736 次 0 错。
