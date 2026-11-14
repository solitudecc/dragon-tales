# 私有 image-table checked-context 原型

此组件暂不注册公共 API、pipeline 或 CMake，不改变默认 Guest 执行合同。
`GuestMemoryV2.data` 仍是任意可写的字节数组，ELF `.rodata` 权限仅提供候选
表字节，不能证明它们在 Guest 中不可变。

接口为 `detail::runImageTableCheckedContext(Module&, ImageTableContextRequest)`。
输入选定既有独立 GuardedEntry 的 raw FunctionStart 符号、表 Guest offset 和
u64 目标数组。函数重认实际运行时检查：整64位初始 SP、全部表字节、同一
Guest base；检查失败仍调用原普通 root。metadata/符号名不认证这些值。

证明完全在独立 Module clone 中进行。先 mem2reg/instcombine，再识别一个
八字节绝对表索引。候选图只保留表映射的目标及原 Unknown 出口；它不是可
执行产品。只有下列联合归纳条件全部成立才允许消费该候选图的常量：

1. 实际 guard 成功到第一次表读取的所有路径都保存表，访问下标在表内。
2. 每个表目标到下一次表读取或终止出口的所有路径仍保存表，SP/标量事实
   在候选图中闭合；所有实际 PHI 入边和重复 CFG 边均计入。
3. 每次表访问的表内证明来自实际整数表达式和原有 bounds 分支；无法证明
   的表外 default 不能作为终止出口忽略，而是整次拒绝。
4. 所有能继续到表读取的 STORE 都证明其完整字节区间与表不相交；无法分类
   的写入、未知调用、atomic/volatile/fence 保守拒绝。不会回表的终止路径
   可以保留任意原始写入/调用，不从它们推导后继事实。

每个地址表达式最多32层/128节点，所有本组件扫描共用不超过1M work，模块/
函数大小另有硬上限。区间只做保守整数过近似，不把未知 PHI、LOAD 或选择
当常量；绕回、未知或预算耗尽均不得成为不重叠证据。

闭合后在另一份保留原 CFG 的 clone 中构造实际 index switch，default 保留
原 LOAD 和完整 target switch。只有被联合证明的、实际 guard 支配的标量
常量才能替换。校验通过后仅移交选定函数的 body，保留原 Module、Function、
Argument 身份及普通 root。失败不提交部分证明或 IR。

当前首先验证 manual O2 的六项表。Tigress flatten 的 output STORE 后仍回
分发，需要额外证明 spill-slot 恢复的输出指针/patch 下标，v1 不猜这些值。
后续先复用现有 control-flow 的 DFAJumpThreading/JumpThreading；中心分发
是否退出业务 SCC 是单独精度门槛，证明闭合或 counter 增加均不代替它。

计划回归包括：合法 base+循环 step、错误入口表/SP 检查、base/晚到 step
别名写、表外下标及其回表分支、未知调用/volatile/atomic、部分 PHI、整字节
范围重叠与整数回绕、预算失败保持完整原 IR、终止任意 output 写保留。
此为新增私有 API，旧代码不能编译该测试；不得称为旧 runtime 执行错误。

v1 实际最小正例因 LLVM21 InstCombine 产生嵌套 Guest i8 GEP 及
`zext(shl i32 index,3)` 而拒绝，源、编译、失败执行及纯打印 diagnostic
均保留。v2 在纯 clone 中把无 inbounds/nuw/nusw 的 Guest i8 GEP 链按
每个索引的符号扩展规则重建完整模 i64 offset；支持窄移位后 zext 时仍
必须证明整张表的下标域在原移位位宽内不溢出。原移位/flags、目标 LOAD
及全 default 保留。未更改测试期望、预算或表保存准入条件。

## 私有原型实际结果

v2 自建 offset 中的 `add 0,index` 又使识别保守拒绝；v3 只避免生成该
前缀。同一 tests.o 在 v1 为3 cases/202 assertions、1 case/6 assertions
失败；v3 为3 cases/311 assertions全过。这个 before 是新私有 API 的
精度回归，不是旧 origin/main 的错误执行。未注册公共构建/API/pipeline。

真实 B manual O2 使用精确 canonical raw SHA `0619b5e6…`、ELF
`f1065b49…`。冻结16个 wrapper 输入的 SP 均为0xc808；原 wrapper 的
`sub rsp,8` 和 CALL push8 后，kernel 初始 SP 是0xc7f8。未使用0xfff0。
existing GuardedEntry 检查该SP和完整48字节表，不约束输出指针。

v3 联合闭合证明保存55个回表 STORE，得到126个标量常量，work123241，
无本组件预算耗尽。只改 view，原 source 全 IR、普通 kernel/wrapper
逐函数打印均保持精确相同。所有 canonical/bound 阶段均通过 verifier。
bound wrapper 重定向仅在独立执行模块中发生。

既定两轮 control-flow/GuestSSA 配方仍保留一个 index switch、一个原
target switch和一个表 LOAD 在业务可达循环 SCC。两轮 GuestSSA 各为
0 forwarding、complete=false、budgetExhausted=1。完整结构门槛失败。
另存的 default predicates variant 实际消除了编码状态0/6的 select，
query64/fold1，但仍没有使中心分发退出业务 SCC。两个配方都保留失败，
不会把 table proof 或该谓词收益称为完整去平坦化。

验证 probe 的早期 C++ API 适配失败和 detached Function 恢复崩溃均已
保留。最终 probe 改为在完整独立 Module clone 中优化，并仅事务移交
view body。两个完整真实 probe 的峰值 RSS 约220MB，单次约1.4秒。
这些运行没有执行目标 native、Unicorn或新的 C oracle；还不是语义验收。
