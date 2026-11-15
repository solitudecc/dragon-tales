# PhiLowBits 独立原生验证计划

下方保留执行前源模型与计划，实际结果单列在文末。
旧 Batch 的所有证据树保持关闭。本次没有把旧 expected 截断后当新 expected。

新源 `scripts/validation/phi_low_bits_native_source.py` 给出独立的 structured C、Python
模型和两个 LLVM 模块。沿用完整66112字节协议：Guest65536、Regs512、Exit56、Return8；
真实Execution.h descriptor和RegBytes→Guest0x8fd0物理别名仍保留。
随机填充使用新的 `phi-low-bits/u32/20261115` 域，固定8个frame；raw和RSI分别覆盖
0/1、全1、符号位、高32位及交替位。C明确用uint32_t初始化history并回绕step，
Python明确对初始值和每次step做M32掩码。输出、freshReg观察和计数器仍独立按u64语义。

两个 LLVM 模块在同一个header中保留parent64/child32 PHI；每个原始实际入边都完整列出，
其中seed0有两个到同一header的successor slots。

| 模块 | 原始关系 | 计划API验收 |
| --- | --- | --- |
| phi-low-shared-step | 两Phi共享同一Freeze初始化；parent incoming=zext(primaryStep32)，child incoming=同primaryStep32 | 1 trunc / 2 uses改写；CFG、内存、branch及Freeze节点保留 |
| phi-low-bad-child-step | 仅child incoming换成child+4；parent仍step3；原child不影响observable | 0改写，完整Module文本不变 |

primaryStep32是 `add i32 (trunc parent64),3`，无flags。初始化只读一次RSI64，先trunc32，
再由**同一个Freeze SSA**稳定定义两Phi的seed；没有新增noundef承诺，也不把两个Freeze合并。
候选 `parent.low` 仅用于primaryAdd和zextTrace；Guest1056写入zext后的值，候选trunc本身
不作为memory或terminator的直接operand。其他history trace、Guest STORE→freshReg LOAD、
大小/limit错误出口、u64计数回绕保持在原执行顺序。

验证顺序拟定如下，数字均是计划值：

1. 新C及before LLVM在O0/O2各比较32个完整arena（C16+LLVM16），先做保存流审计。
2. 只复制bad-child原始IR，将trace1056的zext operand从parent.low改成child，保持primary
   recurrence不变。每个O级别运行8观察：指定trial0/1/2/3/4/7的1056八字节必须不同；
   trial5/6首entry即limit/size错误，仍应完整匹配。总16观察/12指定检出/4未触发匹配。
3. 前两步全部校准后，安装的新helper由main/No-Z3 SDK各实际调用两次，不链接private实现对象。
   shared模块须1/2改写，bad模块须0且文本不变；检查实际SDK/solver映射与前后verify。
4. main及No-Z3 after各在O0/O2各比较16完整arena。每个正常比较增加1056控制，总28个
   不同字节的comparator corruption controls；原有27个offset没有删除。
5. 保存input/expected/actual整帧，独立重算Python expected、全部键域、SHA及单一XZ物理EOF。

总计划正常128匹配/3584controls，故障16观察/12指定/4未触发，8条保存流144frames。
API4次只发生在新before/fault校准以后，旧C/LLVM/UC计数均不重计。
计划冻结时尚无新SDK导出符号，后续使用固定SDK副本执行，见下方实际结果。

这检验的是纯OriginalIR的低位等价组件，不是新的机器指令语义验证或Tigress真实样本重跑。
bad模块的普通行为特意等同于shared模块；精度拒绝由原IR逐字节门验证，显式trace fault
则证明错误选择child会在定义良好的真实输入上变得可观察。Poison、Undef、不同Freeze、
未知Load/Call和不受支持flags/shift边界由结构回归覆盖，不执行未定义输入。

## 实际 before / fault

源模型c773保持不变。before-only client不包含PhiLowBits头文件或API调用，nm确认无新API
导入；只parse/verify/print原IR。其SDK按exact member从已认证Batch归档恢复：
archive SHA `b1475936fd7cd2bf87c96e3b83fd7255ca53ebd6bc2f1a66256490564972951d`，
member `build/lib/libdragon.so`，219,413,456字节，SDK SHA4a89。恢复原件及归档前后SHA均核验，
未读取会被覆盖的active build/lib。实际Z3.so.4.13路径、SHA、device/inode映射严格核对。

新C32+beforeLLVM32共64完整arena匹配，1792 controls全部检出。
故障16观察：12指定1056八字节差异全部检出，4个未触发观察完整匹配。
4条保存流完成全部键域、三帧、SHA、Python expected及物理单一XZ EOF复核。
12个外层守门全通过，0API、0Unicorn、0skip/排除/normalization。
before根为 `/home/ubuntu/vmc-runs/phi-low-bits-native-before-20261115-v1`，
178payload/257,413,775字节完整双端保存，包含恢复的SDK原件。
SUMMARY SHA `45745fc7add8c326905c8c917a9de69b41b6011c0191b0e8f8a551383a4c2e27`；
FILESET SHA `bc2cb054de1e8d4a031b946e18e8825a72d0464b11dcee34b70713b4f148a5e2`。

## 实际 installed helper after

使用独立固定SDK副本，未读取active build/lib：

- main SDK `9ee66d8343727cfbc0307b748d645cb57d71fcd70e067bdbe0e6ebbb7f3db29f`。
- No-Z3 SDK `7a87a061da0f5be810f00433527094d49e6f64c30103d329ee763b1ff56f8244`。
- header `4b5a23afb8d9ffc2e82c476a42981d5c5f55fae4917d508962d444591cc855e2`。

producer仅hpp声明与SDK import，没有链接实现CPP/o；nm核对producer UND→实际SDK T。
两库各实际调用2次：shared为1trunc/2uses/complete，直接检查两个原Use均变成child，
Trunc仍在且0use；bad为0/0/complete=false，原2Use、完整Module文本保持不变。
Function、Arguments、全部block/instruction对象身份，以及memory/terminator全部operands不变。
main实际Z3.so.4.13映射匹配；No-Z3 manifest solver=null且实际solver_maps为空。

各SDK在O0/O2各16完整arena匹配、448controls全检出，两个保存流审计通过。
两库parsed-before、after IR逐字节相等，完整stats相等，未做normalization。
after根分别为 `phi-low-bits-native-after-main-20261115-v1` 与
`phi-low-bits-native-after-noz3-20261115-v1`（均在 `/home/ubuntu/vmc-runs/`）。
main的102payload/37,906,823字节，SUMMARY
`decc03d2667ef806c5cf0098fb7868f69a1e38a58ebb7c63c477e8e9f7dfd5fe`，FILESET
`5f67825984191664f9316f5cfa9d318531675dba49f2fadf526e81d0fdf07b88`。
No-Z3的102payload/37,652,202字节，SUMMARY
`59b86d87b5f29516e541f8135f205e2450eba5fb218f34ec8e462b9d1c672f72`，FILESET
`0332a32419c99002e250bae82d184776b430500034dc2b15c97537aed26ac4ab`。

本轮新正常比较128全部匹配、3584controls全部检出；fault16观察/12指定检出/4未触发匹配。
8条保存流共144frames完整审计，4次installed API调用，Unicorn0。before/fault仅计一次，
未加上旧Batch的任何次数。这里没有单独去平坦化：该helper只改纯SSA uses，CFG和全部effects保留。
