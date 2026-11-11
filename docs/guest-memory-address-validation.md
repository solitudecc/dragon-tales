# Guest 地址与别名：独立原生对照

本验证器针对已有 `detail::runGuestMemorySSA()` 的地址计算和内存失效规则，基线为
`d1a8467`。不修改生产代码、不创建 CMake build，不把生成后的 IR 反推成期望值。
源文件为 `scripts/validation/guest_memory_address_native.cpp` 和
`scripts/validation/guest_memory_address_diff.py`。

## 固定输入与比较范围

独立 producer 构造 **25 个 LLVM 模式、440 个输入**。输入覆盖 0、1、全 1、64 位符号位、
0x7f、0x80、0xff、0x100；未知参数写的地址分别为 Guest 同槽、偏移 3 字节、独立 side
对象；循环执行一到四次。正常 before/after 分别以原生 O0/O2 编译，共 **1760 次**比较。
模块验证、调用次数、Guest descriptor 的 data/size 归还值也要正确。

每次完整比较返回值、调用次数、七个 64 位 Exit 字段、全部 **512 字节寄存器、64 KiB
Guest 内存和 64 字节 side 对象**。原生 Guest 缓冲区按 uint64 分配，atomic 的槽具有
8 字节对齐；普通非对齐访问仍使用 align 1。寄存器对象始终与 Guest 内存分离，不利用
违背已声明 V2 分离合同的重叠输入制造差异。未知普通 pointer 参数可以指向 Guest 或
side；有 NoAlias 属性的控制仅使用独立 side 输入。

正常模块包含窄 Add/Sub/Mul/shift 的位宽回绕、负索引符号扩展和 typed GEP 缩放、i32
计算溢出、i64 精度控制及宽中间值截断；word/byte/部分别名写、GEP 后的参数写、独立
NoAlias/private Alloca 控制、captured Alloca、两个不同 Guest-base LOAD、未知调用、
atomic add/cmpxchg、fence/volatile 和循环重入。

captured Alloca 只验证形状识别的通用 LLVM detail Pass：它暂时把 descriptor.data 指向
当前局部对象，先后通过两种来源写同一对象，读后恢复原 descriptor。它不被称为 V2
稳定映射的机器执行样本，也不把局部栈对象当成返回后仍可访问的 Guest 内存。

Python 参考模型独立按声明位宽求模、GEP 的符号扩展/缩放和字节写入计算结果。不会使用
宿主 64 位整数运算代替所有 LLVM 位宽，也不读取 Pass 的常量结果作为真值。

## 验证器控制

两种错误 IR 原生控制分别把 wrap8 LOAD 错换成另一个槽的值，以及把 alias LOAD 错换成
写入前的值。共 **64 次**观测；其中 **48 次**指定错误必须检出，其余 **16 次**仍须匹配。
`--self-test` 逐一扰动全部已声明状态字段，并拒绝错误长度，共 **5283 个**控制。
正常和控制的完整快照流以 gzip 无损保存，另有每条快照 SHA-256；压缩不改变被比较字段。

正常预期不包含 undef/poison 任意位、危险操作标志或非法指针。它们由结构回归和 UBSan
验证，不固定成某个原生数值。顺序原生执行也不宣称证明 fence/volatile 的并发语义；
最终 Pass 必须在这些位置保留屏障，由结构/统计门另行检查。

最终模式要求所有精确宽度/合法 noalias/private 正例仍有真实转发；未知写与同步场景
必须保留原 LOAD。循环允许仅已证初始边的合法复制，所有后续迭代仍受完整状态对照。
不能以整体禁用转发来取得语义通过，也不能放宽比较来保留错误精度。

## 已执行的旧代码错误控制

最终同源旧基线在 VM
`~/vmc-runs/guest-memory-address-reference-20261111/old-native-attempt003/`。
实际链接 backend SHA-256 为
`bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003`。

1760 个正常观测中，所有未变换 before 均与独立模型一致；变换后的 O0/O2 各在预先列出
的 **200 个**输入上错误，合计 **400 次语义差异**。这包括窄索引错误地址、未知参数
覆盖后读取旧值、captured Alloca、不同 Guest-base 字节重建、atomic 写与循环内别名写。
64 个故障 IR 观测精确检出 48 次指定差异；5283 个比较器控制通过，基础设施错误为零。

旧结果明确为 `semantic_ok=false / ok=false`。`--expect-old-bugs` 只把“确实捕获预先声明
错误且完整覆盖”记为 `baseline_control_ok/checks_ok`，不能把旧语义错误算作通过。
首个构建尝试存在 `byte(...,0)` 重载歧义，产物保留在 attempt001，未计入正常比较。
attempt002 是先前报告字段的执行版本，不替代最终同源 attempt003，也不重复计为新输入。

## 32 位 pointer-index 布局的独立范围

主矩阵从一开始限定本机兼容的 **64 位 pointer/index DataLayout**，包含 i8/i32/i64
索引操作数。另做了一个不解引用指针的布局微探针：输入模块声明
`e-p:64:64:64:32`，返回 `GEP(p, add i64 2^32,8)`。在本机 clang 和 llc 链路中，
`p=0x123400000000` 都返回 `0x123500000008`，没有得到本测试所需的 32 位索引截断
参考值 `0x123400000008`。

因此这条本机编译链不能直接充当该布局的独立内存执行参考；没有冒险访问 4 GiB 外的
指针，没有把微探针不一致称为 LLVM bug 或通过。原 IR、两个编译命令/日志、结果在 VM
`~/vmc-runs/guest-memory-address-reference-20261111/source/index32-*` 保留。
非本机索引布局由本轮 C++/APInt 结构回归检查；2^63 距离仅分析/UBSan，不作原生访存。
零 skip/排除/归一化只描述上述预先固定的 440 输入原生集合。

## 复现与最终验证

```sh
python3 scripts/validation/guest_memory_address_diff.py --self-test
python3 scripts/validation/guest_memory_address_diff.py \
  --repo /home/cc/dragon-tales-overlap \
  --library /absolute/final-runtime/libdragon.so \
  --work /absolute/new-output
```

旧错误控制使用 `--expect-old-bugs`；输出目录必须不存在。脚本保存源文件、IR、所有
命令及 stdout/stderr、加载库 ldd、哈希、每次结果与原始快照，不重用已生成的 LLVM
native 模块。

## 最终集成验证

最终 runtime 是 VM
`~/vmc-runs/guest-memory-address-proofs-20261111/final-runtime/`，backend SHA-256 为
`aeb13989cb31557effcde48872f3f6b6c5b0938fc737f11457bb3ba925130937`，shim 为
`a623a1490d57b9df708194ef9b6a59eec47a5a9c1dc03c72f65e3e98ed4eae6c`。
producer 只链接这份冻结集成库，未额外替换生产组件或链接另一份 LLVM。

同一 **440 输入 / 1760 次** before/after O0/O2 观测，最终全部匹配，
`normal_mismatches=0 / semantic_ok=true / ok=true / errors=[]`。
同源旧库的 **400 次错误→最终 0 次**，不是新增真实混淆程序覆盖。
64 个故障 IR 观测精确检出 48/48 个指定错误，其余 16 次保持匹配；5283 个比较器
控制也全部通过。无跳过、排除或字段归一化。

全部十种精确宽度形式和 NoAlias/private Alloca 精度控制均发生真实转发。十一种未知写入
或同步边界保留原 LOAD、没有错误转发；循环 word 场景仅复制已证明入口边，原循环 LOAD
仍保留，四个迭代次数和三类 alias 输入都由完整状态模型检查。
旧/新 producer 与 Python 模型源哈希相同，25 份 before IR **逐字节相同**，没有把错误
模板换成支持的形式来取得最终通过。保留审计为 `final-retained-audit.json`。

已有 checked-edge 组件完整重跑：**2176 次**正常观测零差异，192 个故障 IR 观测精确检出
**88/88** 个指定错误，5987 个比较器控制通过。原 LOAD 存活、三条未知入边保留和必要
精度门均未放松。这属于相同组件输入的兼容复验。

同四个冻结 ELF 在原显式顺序与额外 post-GVN SSA 顺序分别重新执行，每种均为
**C16/LLVM16** 完整根匹配；寄存器和内存单字段控制各 **16/16**，七个 Exit 字段控制
共 **112/112** 检出，Graph、输入和实际库哈希不变。两种顺序的结果分别保存在
`final-real-original/` 和 `final-real-post-gvn/`，没有合计成新的唯一 ELF 覆盖。

与保留的 `d1a8467` 同顺序记录相比，转发计数保持一致：

| 顺序中的 GuestMemorySSA | f-cff O0 | f-cff O2 | f-combo O0 | f-combo O2 |
| --- | ---: | ---: | ---: | ---: |
| 原始步骤 | 0→0 | 0→0 | 1→1 | 0→0 |
| 额外 post-GVN 步骤 | 1→1 | 0→0 | 4→4 | 0→0 |

原顺序 f-combo O0/O2 的分析仍 incomplete；post-GVN 顺序四模块均保留分析 incomplete，
对应 counter 与 INCOMPLETE status=2 也一致。它们是 Pass 预算边界，不是根函数执行
不完整。实际 CFF 和原程序 switch 均 **1→1**，没有未分类目标 switch；表 LOAD、
运行时目标检查和 default 仍保留。RIP 入口路由清理不能记为消除 CFF。

## 最终证据

VM 根目录为 `~/vmc-runs/guest-memory-address-reference-20261111/`，其中最终新矩阵为
`final-native-attempt001/`，旧矩阵为 `old-native-attempt003/`，checked 兼容矩阵为
`final-checked-compat/`。原始输入、模型、IR、完整快照流、每条字段差异、编译命令和实际
runtime 绑定均保留；第一次失败构建与布局微探针也保留，没有改写成最终成功。

报告、源、IR、快照和命令日志的压缩包为 VM 同目录的 `final-evidence-v2.tar.gz`，本地为
`/Users/c/Desktop/deob/validation-artifacts/guest-memory-address-reference-20261111/final-evidence-v2.tar.gz`。
两端 SHA-256 均为
`b649ef4457c1e44803bc8173da1be9ad7d3f3570821a62b1cfa29da7a97b1917`。
`final-archive-manifest.json` 记录逐文件哈希；重复的已编译 ELF/objects 和 runtime 没有
打包，执行时引用的原件及其哈希仍在 VM 保留，这不改变语义输入集合或比较字段。

这验证可选 LLVM 分析/改写的局部语义，不是新的机器指令/Unicorn、一般堆或整语料库
语义证明。全量 CTest、no-Z3 构建和合并由根代理集中完成。
