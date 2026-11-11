# 嵌套循环 GuestMemorySSA 独立原生验证

本轮针对 natural-loop 成员遍历修复提供独立语义验证。每条回边必须用自己的访问集合求循环成员，再合并到总集合；复用总集合会让先发现的内环阻断外环前驱遍历。旧版的结构失败由 `TestLLVMGuestLoopMembership.cpp` 覆盖；以下原生矩阵在旧版、新版均通过，**不构成旧版曾产生错误执行结果的证据**。

## 固定夹具与参考模型

`scripts/validation/guest_memory_nested_native.cpp` 直接生成 LLVM IR，调用 `detail::runGuestMemorySSA`，分别保存 before/after。`guest_memory_nested_diff.py` 用独立 Python 循环和模 2^64 运算计算期望结果，不从优化后的 IR 推导答案。

- `nested`：外环前缀读取 Guest 状态，内环更新该状态；外环第二次读取必须观察前一次内环写入。
- `multi`：外环有两个 latch，前缀先写再读状态，其中一个 latch 另写 Guest 内存。
- `self`：内环为自环；零次内环直接退出，正次数执行有限次写入。
- 每种结构采用 8 种基本块分配顺序与宿主分配 padding，共 24 个函数。padding 只改变构造环境，不参与语义输入或参考模型。

每个函数有 96 个输入：8 个 64 位边界值 × 外环次数 0/1/2/3 × 内环次数 0/1/2。边界值包括 0、1、32 位符号边界、64 位符号边界及全 1。共 2304 输入；所有循环有界，使用 LLVM 无溢出标志的模整数运算，没有故意引入 poison/undef 输入。

在 aarch64 宿主用 clang-21 分别以 O0、O2 编译 before/after。每次比较完整返回值、512 字节寄存器、64 KiB Guest 内存、全部 7 个 Exit 字段；Guest descriptor 的指针和长度另作不变性检查。寄存器和 Guest 缓冲区实际分离。快照中的 call 计数恒为 0，本夹具没有调用，因此不宣称调用语义覆盖。

## 故意错误与比较器验证

每种结构各有两个错误变体：删除内环 STORE，以及将前缀 LOAD 的值错误替换为入口值。Python 在执行前独立算出哪些输入必须不同；每条错误快照还必须逐字节等于对应错误模型。要求实际差异集合与预定集合精确相同，而非仅检查“出现过差异”。

每版共 1152 个错误变体观察，其中 512 个应当不同。其余输入确实不会观察到指定错误，例如零次循环；它们仍完整比较，不作为跳过。比较器另对全部输入逐字段翻转一位，并拒绝三种错误快照长度，共 25347 个自检。

## 同源结果

| 项目 | 旧 B244 | 修复后 |
|---|---:|---:|
| 原始输入 | 2304 | 2304 |
| 完整状态比较 | 9216 | 9216 |
| 正常差异 | 0 | 0 |
| 错误变体观察 | 1152 | 1152 |
| 指定错误检出 | 512/512 | 512/512 |
| 比较器自检 | 25347 | 25347 |
| 排除 / 跳过 / 归一化 | 0 / 0 / 0 | 0 / 0 / 0 |
| available / complete | 24/24 | 24/24 |
| budget exhausted | 0 | 0 |

两版冻结的三份 harness 源文件完全相同，24 个 before IR 逐字节相同。各模块 loadsForwarded 均为 1；loopBarriers 旧为 3–5，新为 3–4。13 个 after IR 逐字节相同，其余差异通过完整状态验证；不把计数差异当作去混淆收益。

另运行 `saved-audit.py`，不执行原生函数，重新读取每版 16 个正常/错误快照流和 1 个期望流：逐字节与冻结模型比较，核对输入覆盖、逐条报告字段与快照 SHA，核对全部 artifact/source SHA。两版均通过。这是保存证据复核，不另计为执行样本。

## 固定版本与路径

- 旧 backend SHA-256：`b2449677c1cfaa247d90286dd4ed8dbefbf944c0f2ee1e30488c3c1eac4e747f`。
- 新 backend SHA-256：`2e509ceb03bc5b47d2e95fdacfd579860e520f238ee239746c273b6034ba9bda`。
- producer SHA-256：`22bd233c5abaf91fab2c70543c781b3cffea2b4ed49f1e9fc9025d8513af74f0`。
- Python model SHA-256：`aafabc9291045dd8cb86af9391e8753f0a1a306243ab53021cfa07b61134dbdb`。
- 复用比较器 SHA-256：`af89164eebe5f8fed81093887a38b61bb8a0fe09c657226307773dca8b4c925d`。
- VM 证据根目录：`~/vmc-runs/nested-loop-reference-20261111/`，包括 `old-attempt001`、`new-attempt001`、两个 `*-saved-audit.json`。

完整本轮证据已封为 `~/vmc-runs/nested-loop-reference-20261111-complete.tar.gz`，Mac 副本位于 `/Users/c/Desktop/deob/validation-artifacts/nested-loop-reference-20261111/`。两机分别遍历归档，核验精确文件集合、每个文件长度和 SHA：286 个文件、有效载荷 34739839 字节，结果一致。归档 SHA-256 为 `6c9450669b27b4dec0261704fa62458d036247acbb78f05ce0bb2466a53ae411`。归档包含本轮所有源快照、producer、IR、原生测试库、日志、快照流和报告；作为输入借用的旧/新 Dragon backend 由主任务冻结保管，本归档记录其路径和 SHA，未重复收录。没有删除原始证据。

运行命令形式：

```sh
python3 scripts/validation/guest_memory_nested_diff.py \
  --repo ~/dragon-tales-overlap \
  --library /absolute/path/to/libdragon.so \
  --work /new/output/directory
```

编译 producer 仅链接 backend，不再链接 LLVM shared library，避免重复注册 LLVM 命令行选项。

## 覆盖边界

本矩阵验证宿主兼容 DL64、64 位 Guest 写入、有限嵌套循环、多 latch 和自环。新增 byte-walker 成员回归属于结构覆盖，不在本原生矩阵中。没有新增机器指令、IGNIL、未知调用、非宿主数据布局、全语料或真实 CFF 消除验证；本结果不证明一般循环不动点或分析完备性。
