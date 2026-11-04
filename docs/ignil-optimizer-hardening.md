# IGNIL 优化器正确性修补（2026-11-05）

审计基线 `69ee6f1`。先前 CTest 和语料 IR verify 通过，仍不能证明优化器没有语义错误。
本轮把寄存器快照、常量极值的已确认问题分别修补，下文保留各自基线和验证数字。

## 寄存器快照与隐式清零

最小反例先保存 `RAX[63:32]`，写 `EAX=7`，再把保存值写入 EBX。
输入 RAX=`0x1122334455667788` 时，原始 IGNIL 与 Unicorn 参考结果均为
EBX=`0x11223344`；旧 `simplifyBlock()` 把最后一次读取替换为清零后的高位，得到 0。

原因是复制传播及表达式身份记录仅按显式目标宽度失效，漏掉 32 位写入的隐式父寄存器清零。
该问题同样影响 double-NOT 消除和 half-adder 身份：旧值和新值不能因寄存器位置相同而合并。

修复在每次 32 位寄存器写入时，令所有直接依赖寄存器的复制与表达式记录失效，保留原始
Temp 快照。该简化器没有架构或寄存器文件参数，无法证明父寄存器到底有 8、16 或更多字节，
因此不按某个固定范围猜测。常量/Temp 传播和不相交的 8/16 位写入仍可优化。
没有改动公开 API、执行器、指令提升或参考排除规则。

这种保守失效可能少做部分优化：固定随机样本的 simplify+DSE 操作数从旧库 11437 变为
新库 11444，原始为 25348；本轮没有为保留旧压缩率而放宽正确性。

## 回归和独立参考

仅向旧代码加入三个结构回归，旧代码 80 条断言中 26 条失败；正对照通过。
加入 Unicorn 对照后，四个测试在旧代码上共 78 条断言失败，其中 52 条来自错误的输出寄存器。
新代码四个测试的 13151 条断言全部通过。

Unicorn 对照覆盖 x86-64、AArch64，8/16/32 位保存视图、COPY/double-NOT 两种形状，
八个极值输入，共 96 次机器参考执行；原始和优化后的 IGNIL 分别比较，共 192 组状态匹配。
参考机器程序以不改变标志位的 RORX/BFXIL/LSR/MOV 提取旧高位后清零源寄存器，与所测
IGNIL 的最终效果等价。比较全部通用寄存器（含 A64 SP/XZR）、建模标志位和完整 64 字节
数据区，没有新增排除或跳过。这里验证直线 IR 的寄存器/数据语义，不冒充完整 CALL/RET 函数。

旧失败输出：

```text
old-tests.log:
  test cases: 3 | 1 passed | 2 failed
  assertions: 80 | 54 passed | 26 failed
old-oracle.log:
  test cases: 4 | 1 passed | 3 failed
  assertions: 13151 | 13073 passed | 78 failed
```

完整 CTest 524/524；无 Z3 构建 419/419。随机优化等价检查旧/新库各四种方案 × 1148 次，
均无错误；13 个随机汇编组合不能编码，明确报告，不计成功。故意把所有寄存器设为不活跃的
负对照得到 1145 次差异 / 1148 次检查，其余三次没有可观察差异；正常方案仍无差异。

## 优化后真实基本块对照

`REALBLOCK_OPT=1`，Lua 三架构各 150 块 × 3 个初态，旧/新完整输出逐字节一致。

| 样本 | 实际比较 | Unicorn fault | 高地址内存未比较 | 区域外写入未比较 |
|---|---:|---:|---:|---:|
| lua32.exe | 431 | 19 | 0 | 27 |
| lua64.exe | 414 | 36 | 62 | 35 |
| lua_a64 | 390 | 60 | 52 | 19 |

合计 1235 次比较中未报告 DIFFERENT；115 次 fault 不计成功。
另外在选择块时跳过 call/system 块 234 个、未提升块 7 个。高地址/区域外内存比较限制是
原有框架范围，不是本轮新增的归一化；1235 次不能称为完整全状态比较。
本轮未选择到此前 x87 FXAM 的参考分歧，不因此撤销已记录的那处告警。

产物：VM `~/vmc-runs/simplify-snapshots-20261105/`，含双共享库旧快照、构建/CTest、
旧失败、新通过、优化等价负对照及 `realblocks-summary.json`。

本次没有重跑两个语料库的全函数原始 IR verify 或全部 Tigress 矩阵：生产调用关系中，
`simplifyBlock()` 仅由独立优化 API 进入，反汇编/CFG 恢复和 LLVMLifter 不调用它；
这些未改变的路径已有上一轮完整记录。本轮针对实际改变的优化路径重新执行上述参考检查。
完整 CTest 仍重新执行了现有 IR/执行/加载管线回归。未做全优化器形式化证明，不能承诺
所有未覆盖的类型组合和未来改写规则都无错误。

## 整数常量折叠的宿主未定义行为

第二个修复基线为 `454fb45`（已含快照修复）。旧源码的 UBSan 反例包括：

- `NEG(0x8000000000000000:i64)`：对 INT64_MIN 做 C++ 有符号取负；
- `SDIV`、`SMOD(INT64_MIN, -1)`：C++ 的商不可表示，除法和取余均存在未定义行为；
- `SEXT(Const{0,0})`：畸形零位宽进入 `signExtend`，产生非法移位计数。

修复把 NEG 改为无符号减法再按原位宽掩码，准确保留模整数位模式。
有符号除法/取余在除零或宿主 INT64_MIN/-1 时保留原操作，不猜 Guest 异常语义；
零位宽常量不进入折叠，保留原式供调用方识别。没有新增指令语义或公开接口。

仅向旧代码添加三项普通回归：145 条断言中 11 条失败；NEG 的原始位模式在当前 ARM
宿主上可能恰好正确，因此普通数值比较不足以检测 C++ 未定义行为。增加 Unicorn 算术
参考后，四项回归在旧代码上仍有 11 条失败；新代码 4761 条断言全部通过。
无 Z3 的三项普通回归 145 条断言全部通过。

新算术参考使用 AArch64 的非陷阱 NEG/SDIV/MSUB，加显式符号扩展及结果截断，
覆盖 8/16/32/64 位、零、正负极值和正负除数，共 416 次机器计算，原始及优化后的
IGNIL 共 832 个结果值匹配。此处仅核对算术结果，不冒充完整机器状态或 x86 异常执行。
VM 原生 AArch64 内联汇编也确认三个 64 位极值结果：NEG=min、SDIV=min、SREM=0。

新增持久化 `dragon_optimizer_ubsan` 测试目标，单独对生产 `IGNILSimplify.cpp` 开启
`-fsanitize=undefined -fno-sanitize-recover=undefined`，无需重编译 LLVM。
GCC/Clang 且编译/链接检测支持 UBSan 时，CTest 注册 neg/div/mod/zero/sweep 五项检查。
持久化探针编译旧生产源码后，四个独立反例均被 UBSan 拒绝；新源码全部通过。

`sweep` 遍历全部 1–64 位整数宽度、23 种二元及五种一元折叠，包含移位边界和多种
目标宽度，共 **109568 个组合**。输出消费折叠结果，避免算术被当作死工作删除。
它用于检测宿主未定义行为，不是对所有混合位宽 IR 语义的形式化证明。
初次临时 Sanitizer 探针在主库同时重建时链接失败；改为主库完成后顺序链接，未改变
检查条件。最终接受数字来自成功构建后的运行及持久化 CTest。

```text
old-tests.log:
  test cases: 3 | 1 passed | 2 failed
  assertions: 145 | 134 passed | 11 failed
old-oracle.log:
  test cases: 4 | 2 passed | 2 failed
  assertions: 4761 | 4750 passed | 11 failed
old UBSan:
  negation of -9223372036854775808 cannot be represented
  division of -9223372036854775808 by -1 cannot be represented  (SDIV/SMOD)
  shift exponent -1 is negative                              (zero width)
```

最终 CTest **533/533**，无 Z3 **427/427**，均包含五项 Sanitizer 检查，没有预期失败标记。
优化等价旧/新各四方案 × 1148 次，0 错误；负对照仍为 1145 次差异，13 个不能编码的
生成候选单独报告。优化后 Lua 真实块检查再次执行：1235 次比较、115 次 fault，
旧/新完整日志一致，0 DIFFERENT；上表的未比较内存及选块限制保留。本轮使用与快照修复
相同的真实块初态，不把重复运行算成更多独立样本；simplify+DSE 操作数保持 11444。

产物在 VM `~/vmc-runs/simplify-integer-20261105/`：`baseline/` 保存两个旧库及旧源码，
`permanent-ubsan-old.json` 保存持久化探针对旧源码的拒绝证据，`final-summary.json`
保存最终测试状态、库校验值和比较范围。重跑：

```sh
cmake --build build -j3
ctest --test-dir build -j3 --output-on-failure
./build/bin/dragon_tests '[simplify-integer],[simplify-snapshot]'
./build/bin/dragon_optimizer_ubsan sweep
```

两个已确认根因已按通用规则修补。覆盖边界仍同上：本轮不声明整个优化器无错误，
也没有补齐混淆 CFG、堆内存分析或通用 MBA 化简能力。
