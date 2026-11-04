# 外部 API 与 Tigress 混淆的组合验证

2026-11-04；生产后台为 `65aac4c`，本轮仅增加测试、研究工具和记录。
没有修改 API 效果模型、公共接口、IGNIL CALL/RET、LLVM 函数签名或名字。

结论：有实现代码时，受测 GOT CALL、间接调用及回调等可以完整验证；没有实现时能验证准确的
调用边界，但不能确定 API 后的行为。静态 PLT/尾跳还需要消费者接续 Transfer。
混淆 C O2 仍有分发恢复缺口，**完整矩阵不能称为通过或任意外部 API 已支持**。

## 场景

源文件：[调用方](../tests/fixtures/tigress-external-caller.c)、
[独立测试库](../tests/fixtures/tigress-external-provider.c)、
[验证脚本](../scripts/validation/tigress_external_api.py)。

| 场景 | 重点 |
|---|---|
| return | API 返回 0 / 全 1，控制后续分支和输出 |
| write | API 改写别名内存，同时返回状态；比较调用前后的加载值 |
| memcpy | 零长度/非零复制、字节值和目的指针返回 |
| memcmp | 无符号字节比较，负/零/正三类结果 |
| strlen | 空串、不同长度，返回值控制分支 |
| indirect | 参数传入函数指针；没有导入名时不虚构 API 名 |
| callback | 外部库调用当前模块的回调，再修改输出并返回 |
| varargs | 八个可变参数，覆盖寄存器及栈上传参 |
| float | XMM 浮点传参和返回，后续转换及比较 |
| tail | C 优化产生的外部尾跳及调用链 |
| stop | 源码声明不返回，验证未知 API 入口前的状态，不执行其无限循环实现 |

`stop` 只计划未知边界检查，不测试操作系统退出、异常或不返回实现的完整运行。
浮点输入特意保持精确有限值；没有借本轮声称覆盖 NaN/溢出或全部浮点 API。

## 编译、混淆及参考

- Tigress **2.1 revision 2128**，固定种子 `81901901`；工具来源同
  [先前记录](tigress-function-validation.md)。plain、switch Flatten、EncodeArithmetic→Flatten。
  变换作用在调用 API 的 kernel 内部。保留真实外部符号声明，使用 `-c` 生成独立翻译单元。
- Clang 21.1.8，C O0/O2、PLT/GOT 两种形式、线性/递归策略。
  对匿名函数指针，两套编译选项可能产生相同指令；没有把它声称为两种不同导入。
- 真实 ELF 共享对象，独立测试库具有 SONAME 和导出符号。主库在 `0x1000`，提供方在 `0x6000`。
  测试加载器按 ELF RELA 处理 64/GLOB_DAT/JUMP_SLOT/RELATIVE，采用立即绑定；不测试延迟绑定和符号抢占。
  `-Bsymbolic` 使已定义的内部 kernel 调用绑定在本模块，未定义 API 保持外部导入。
- 标准 API 的测试库用实际 C 编译成 Guest 指令，**不是注入寄存器/内存效果的模型**。
  ARM 主机另编译两个版本，分别使用这些实现及系统 libc；自定义 API 始终来自独立测试库。
  `memcmp` 的 C 规格只约束结果的符号，不假设系统库必须返回某个非零幅值。
  依据 [系统 API 文档](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/memcmp.3.html)。
- Python 分别计算调用方的返回值及八个输出字；ARM C 与 x64 Unicorn 独立对照。
  不比较不同架构或不同二进制的寄存器分配和栈布局。
- 调用方 IR 与 API 提供方 IR 分别生成、分别优化。调用方优化时**看不到 API 函数体**。
  提供方对象、测试分发器只在最后链接阶段加入。生产库没有新增内置系统 API 实现。
- 两条流程：V2 O0；静态产品优化后再 LLVM O2。静态预算为 0，并检查保持为 0。
  模块有原始、优化后的 IR、独立 verify、原生对象和驱动。

未知 API 只填入可解码的观测标记，两边都在第一条指令执行前停止，不借标记模拟 API 效果。
完整返回和边界快照均比较 76 个已建模字段、完整 65536 字节内存、PC/SP/Flags 和 Exit。
不作 Legacy 归一化，不新增字段排除。它仍不包含未建模的 OS/TLS、MXCSR、AVX 高位等状态。

## 主矩阵：16 个边界输入

11 场景 × 3 配置 × 2 个 C 优化级别 × 2 个调用形式 × 2 策略 × 2 LLVM 流程。
普通场景分别运行“无实现/有实现”；stop 只运行“无实现”。总计 **16128 次执行**。

| 结果 | 数量 | 含义 |
|---|---:|---|
| root_match | **3936** | 完整根返回，全部受检状态匹配；其中混淆版 1984，原版 1952 |
| api_boundary_match | **6784** | 精确匹配外部 API 入口边界，不能当作整函数完成 |
| incomplete | **5408** | 在预期根返回/API 入口前停止，没有完整语义比较 |
| different / reference error / native error | **0 / 0 / 0** | 未出现已比较状态不一致或执行基础设施错误 |
| 未执行 / 成功跳过 / 排除字段 | **0 / 0 / 0** | 无遗漏计划或放宽比较 |

正常主矩阵命令返回 **1**。完成比较的 10720 个快照涉及 814720 个字段、702545920 字节内存。

| API 代码可用性 | 完整返回匹配 | API 边界匹配 | 提前未完成 |
|---|---:|---:|---:|
| 有实现 | 3936 | 1168 | 2576 |
| 无实现 | 0 | 5616 | 2832 |

有实现仍停在边界的部分主要是静态 PLT/尾跳契约；不能仅因提供了库就假定自动执行完整调用链。
绝大多数提前未完成来自混淆 C O2 的未知分发，尚未到达真正的 API 入口。

| 场景 | 完整匹配 | API 边界匹配 | 未完成 |
|---|---:|---:|---:|
| return/write/memcpy/memcmp/strlen/callback/varargs/float，各场景 | 384 | 640 | 512 |
| indirect | 512 | 512 | 512 |
| tail | 352 | 640 | 544 |
| stop | 0 | 512 | 256 |

其他检查：

- ARM 可观察结果 **1920/1920**；x64 C 规格比较 **5920/5920**，覆盖 **1920 个唯一**场景/配置/形式/输入组合。
  5920 包含不同策略和协议下的重复参考观测，不是 5920 个不同输入。
- 132 份调用方 ELF；528 次调用方降级、8 份独立提供方模块，原始/最终 IR 均 verify。
  提供方在同一 C 优化级别下跨场景字节相同，缓存前校验其 SHA256。
- 528 次入口检查无漏报、多报；独立 ELF 验证的标准 16 字节 PLT 入口单列为合成跳板，不误算 C 函数误报。
- 调用方、提供方的 unlifted 均为 **0**，没有生产指令语义修改，样本前后为 **0→0**。
- 480 个有名字的模块检查均由加载器识别导入；其中 360 项在已降级 IR 中保留来源。
  另 120 项缺少 IR 来源，均位于尚未恢复到调用点的混淆 O2 模块，不能声称其 API 已被完整分析。
  其余 48 项为匿名函数指针，不凭运行时测试库地址虚构静态导入名。
- 已降级未知声明及调用点没有发现 `memory(none)`、只读、`willreturn` 或 `nounwind` 的错误保证。
  这遵守 [LLVM 的调用副作用规则](https://llvm.org/docs/LangRef.html#function-attributes)，不靠 API 名字推测效果。

## 静态 PLT 与尾跳的具体限制

普通 CALL→PLT→API 在 V2 可以继续分发；静态 PLT 函数的外跳则返回 Transfer。
调用方传播该出口，而不会擅自把未知跳转变成正常返回。若消费者希望继续运行，需要处理该目标并接续状态。
因此测试中的静态 PLT 即使给出了实际 API 代码，也可能只有精确边界比较，没有完整根返回。

原版 C O2、PLT 尾调用、递归策略的静态流程还会先停在 PLT 自身：

```text
actual   [2, 1, 0x101a, 0x1030, 1, 0, 0]  # kernel → PLT
API end  [2, 1, 0x1030, 0x6000, 1, 0, 0]  # PLT → API
```

主矩阵仍将这 **32 次**记作“未到预期 API 入口”，没有改写原始结果。
另取其实际 PLT 入口为已声明静态边界，独立 Unicorn 复验 **32/32** 快照匹配，
另比较 2432 字段、2097152 字节。它证明这个更早的出口状态正确，不证明 API 或整函数已执行。
剩余 **5376 次**未完成均属于混淆 O2 的分发覆盖缺口。

## memcmp 相等分支补充

主矩阵的 16 个数值没有 `0x80000001`，因此另加该值，使用 17 个输入重跑 memcmp 全部配置。
当前脚本默认 17，主矩阵可显式指定 `--trials 16`；已保留主矩阵实际使用的旧脚本及 SHA256，
输入扩展前后前 16 个输入和执行逻辑相同。

补充 **1632 次**：408 root_match、680 api_boundary_match、544 incomplete；0 difference/error。
ARM 204/204、x64 规格 612/612；入口、加载器导入及 unlifted 无错误。
12 项未恢复到 API 的 O2 模块缺少 IR 来源。新增相等输入本身 96 次中：
24 次完整返回、40 次 API 边界、32 次未完成。补充命令返回 1，没有把缺口改成预期通过。
补充的其他 16 个输入与主矩阵重复，不把两轮数字当作全部不同测试输入。

## 检测器负对照和项目回归

plain、C O0、GOT、线性、16 输入、两 LLVM 流程，分别包含未知及已实现 API：

| 故意错误 | ARM / x64 规格 | 已实现 API 的 LLVM | 未知 API 前缀 |
|---|---|---|---|
| 改坏测试库返回值 | 32 / 32 different | 32 root_match，忠实于错误库 | 32 boundary_match |
| 返回后错误清零 RAX | 两层各 32 match | 16 different、16 root_match | 32 boundary_match |
| 返回后丢弃 API 内存写入 | 两层各 32 match | 32 different | 32 boundary_match |

清零 RAX 在返回值本来为 0 的输入上不会产生差异；测试严格要求这种分布，没有把它们误算抓错。
普通命令均返回 1。`--expect-control` 严格检查每个输入、层次、边界/完整返回、导入和未执行数，
不接受任意非零退出或基础设施失败。

新增四项 CTest：11 场景 plain/O0、两形式、17 输入的协议/执行检查，以及三项上述负对照。
不依赖专有 Tigress，也不声称混淆全矩阵通过。完整 CTest **499/499**，无 Z3 **350/350**，
分别 75.99/71.10 秒；完整混淆主矩阵使用有 Z3 的后台，无 Z3 只重复项目 CTest。

## 复现与证据

```sh
cd ~/dt-agent-all
DRAGON_C_LIB="$HOME/dt-agent-all/build/c_shim/libdragon_c.so" \
TMPDIR="$HOME/dt-agent-all/validation-tmp" CILLY_NATIVE_CC=x86_64-linux-gnu-gcc \
~/ucdev/bin/python3 scripts/validation/tigress_external_api.py \
  --tigress "$HOME/bench/tools/tigress-2.1-github/run-tigress" \
  --trials 16 --out validation-results/external-reproduction
```

补充加 `--case memcmp --trials 17`。快速协议检查用 `--profile plain --c-level O0 --strategy 0`。
故意错误用 `--case return --control c-result|stale-register`，或 `--case write --control stale-memory`；
仅 CI 检查指定错误时加 `--expect-control`。

VM `~/dt-agent-all/validation-results/` 保留：

- `external-full/`：主矩阵 C、独立 DSO、真实重定位、命令、IR、原生驱动、逐项输入输出及 JSON。
- `external-full/harness-used.py`：主矩阵实际脚本，SHA256 `775dcb30be1060ecb2ce7a86a3abde667ef7f28718eb2c175306d48b8c66f148`。
- `external-equality/`、`external-tail-boundary.json`、三份 `external-negative-*/`。
- `external-check-progress.json`、`external-final-{ctest,noz3-ctest}.log`。

本机 `ANALYZE/tigress-external-api-20261104/` 保存审阅包和展开产物：794 个文件，
其中清单的 793 项 SHA256 已核对。包含生成 C、132 份调用方 ELF、全部分析结果、
代表性静态 IR/驱动、主矩阵原脚本和当前脚本。完整逐项快照留在 VM，未把大型实验数据加入 Git。

初次探测还保留了默认内部符号走 PLT 的更早 Transfer，以及 Tigress 未加 `-c`、试图链接尚未提供 API
而失败的日志。后者是测试驱动配置问题；没有忽略生成器失败来计为成功。
最终内部绑定和生成命令明确记录在各份 `commands.jsonl`，没有修改生成 C 的算法来消除失败。

生产 LLVMLifter SHA256 保持 `aa2bb840993c2773b69d4281088265d895ffe4bac68bdee664303ae6e2d0e567`。
本轮未改产品语义，因此未重复单指令和 69 份真实库矩阵。
没有证明真实 x64 glibc 的内部代码、PE/WinAPI/其他架构、新版 Tigress、多种种子、全部输入、
延迟绑定/抢占、TLS/errno、分配器对象生命周期、系统 I/O、异常/longjmp、真实进程退出或并发。
拿不到库代码或可靠效果约束时，这些 API 仍应保持未知，不能推测调用后的值来删除分支。
