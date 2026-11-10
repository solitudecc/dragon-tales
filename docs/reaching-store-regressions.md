# 同函数 reaching-store 回归

本组回归始于 `26e1fce`，测试提交不包含生产实现。它要求去平坦化能在
同一前驱块内，从 dispatcher 的普通 state LOAD 反向找到同地址、同宽的
最后一次 STORE，同时明确拒绝越过未知写入和同步边界。

`tests/lifters/TestLLVMReachingStore.cpp` 构建真正的同函数 CFF 循环：
Guest+0x100 中的 state 依次取 10、20、30，最终返回。first 块还保留
真实的条件早退；尾部计算和 LOAD 的值参与这个条件，不能当死代码删除。
它不读取 RIP，不用 entry/continuation selector，也不分析其他函数的实现。

正例包括相邻 STORE、尾部纯计算、普通 LOAD、显式 readonly/readnone
非 convergent CALL，以及未知 CALL 后重新写入 state。负例包括未知
CALL、参数指针写入、部分重叠写入、volatile LOAD/STORE、atomic
LOAD/STORE/RMW/cmpxchg、fence、convergent readonly CALL 和带 memory
clobber 的 sideeffect asm。首版扫描保守拒绝所有其他 STORE；这里没有
依据地址形式猜测不别名。每个负例仅保留 first→dispatcher，entry 和
second 的独立已证明边仍须恢复。

测试直接运行 `runProvableDeflattening()`，此前不运行 GVN、Mem2Reg 或
GuestMemorySSA，避免由其他组件先替换 state LOAD。pass 后逐条检查
first 中的原有非终结指令仍存在且顺序相同，检查对应边、统计和 LLVM
verifier，然后才做通用 CFG cleanup。基线只识别紧邻 terminator 的
STORE。实际旧代码对照为 3 cases / 256 assertions，其中 15 assertions
失败：五个间隔正例各失败三条；相邻 STORE 和 12 个屏障模式均通过。
这是协调者在旧实现上运行 tests-only 的原始结果，本组保留副本，不计为
新执行。原始日志为 `/tmp/local-state-reaching-old.log` 和
`/tmp/local-state-old-build.log`，副本位于下述产物目录的
`old-control-reused/`。

导出及原生验证：

```sh
DRAGON_REACHING_STORE_FIXTURE_DIR=/absolute/path/fixtures \
  ./dragon_tests '[reaching-store]'
python3 scripts/validation/reaching_store_diff.py --self-test
python3 scripts/validation/reaching_store_diff.py \
  --fixtures /absolute/path/fixtures --out /absolute/path/native
python3 scripts/validation/reaching_store_diff.py \
  --fixtures /absolute/path/fixtures --out /absolute/path/control-alias \
  --control stale-alias --expect-control
python3 scripts/validation/reaching_store_diff.py \
  --fixtures /absolute/path/fixtures --out /absolute/path/control-condition \
  --control skip-condition --expect-control
```

脚本对 18 个模式的 before/after IR 各以 LLVM O0/O2 编译，使用 12 个
边界输入，合计 864 次原生执行。优化时 helper 只有声明；之后才链接
独立 C helper。每次同独立 Python 字节模型比较返回值、调用计数、全部
512 字节寄存器及全部 64 KiB Guest 内存。保存原始命令、退出码、
stdout/stderr、IR 哈希和实际/期望快照。任何失败都不能计为成功。

两个显式错误控制分别忽略未知 alias STORE 和删除 live 条件，要求在
预先确定的输入上出现差异。`--self-test` 还对每个模式/输入修改一次
返回值、计数、寄存器、内存字段，并拒绝三个错误长度，共 867 个纯
比较器控制。没有排除、跳过或字段归一化。普通 LOAD 的输入槽显式
初始化为 `x ^ 0x0f0f0f0f`，使其条件值与 `x` 不同，避免错误地以
`x` 代替 LOAD 仍通过对照。

最终冻结库验证结果（2026-11-11）：

| 验证 | 实际结果 |
| --- | --- |
| C++ focused exporter | 3 cases / 294 assertions，全通过 |
| 比较器字段及长度故障控制 | 867，全通过 |
| before/after × LLVM O0/O2 | 72 modules / 864 次原生执行，全部 match |
| 忽略 alias STORE 的错误控制 | 24 次：4 different、20 match，逐输入符合预期 |
| 丢弃 live 条件的错误控制 | 24 次：10 different、14 match，逐输入符合预期 |

最终 C++ 带导出，比无导出运行增加 38 次成功快照写入检查，因此
294 与旧对照的 256 条断言之差不是减少检查。每次原生执行均比较
返回值 4 字节、调用计数 4 字节、完整 512 字节寄存器快照和完整
65536 字节 Guest 内存。两组错误控制只有预先确定的输入允许出现差异，
其余必须匹配。正常运行和错误控制均无排除、跳过、归一化。

完整 VM 产物根目录：
`/home/cc/vmc-runs/reaching-store-final-20261111/`。
最终运行位于 `final-validated/`，包含 `focused.log`、三个
`results.json`、全部命令/退出码/stdout/stderr、实际及期望快照、38 份
源 IR、冻结库和 SHA/ldd 记录。加强版验证脚本来自 `54b52a3`，测试源
来自 `19c9f55`；其 SHA 分别为
`04315c3a3545ca2dc99498368f5351997df4408ad30e45ff556c77ddbf7f6a36`
和 `340f7f6b24197ce2cd1e925a7da184b81f9afc7376fbb6869b5a7a898ca6c658`。

运行时从协调者冻结的
`/home/cc/vmc-runs/llvm-local-state-20261111/final-runtime/` 复制；
backend SHA256 为
`7c54c9f964555a20d2b4fded23219344977eddeced5cb00c2acbfdb6a23b8799`，
shim SHA256 为
`cb01b3cb7723d69e1902fc89e393c62729c01e3fdc25cfd94aeef5827e11e855`。
ldd 确认 exporter 和 shim 都绑定本组复制的同一个 backend。

额外保留清单核验逐行读取 912 对完整快照，重算每个字段的差异，核对
精确 case/variant/level/input 集合、38 份 IR、1254 个成功原始命令及
冻结库绑定；结果为 `final-validated/retained-artifact-audit.json`。
这次清单核验没有重新执行目标，也不增加语义覆盖数字。

这些是 LLVM 层面、顺序执行的语义对照，不是机器 lifter 或 Unicorn
验证。volatile、atomic、fence、convergent 和 asm 的并发/环境语义不由
顺序原生执行证明，其保守边界由 C++ 结构回归固定。本组只记录上述
focused/native 结果；全量 CTest 由协调者单独执行和记录。
