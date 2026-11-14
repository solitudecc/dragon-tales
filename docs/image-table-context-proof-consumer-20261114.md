# 私有完整闭合证明消费者

本阶段默认模式仍为原010ded版本的 full target-switch fallback；新增三参数
私有 overload，第三参数 `consumeClosedCFG` 必须显式为true。没有修改公共
API、CMake、默认pipeline或旧归档。旧两参数符号调用false，不改变request
结构布局或默认预算。

## 实际状态单元

冻结 manual O2 的 `01-checked-context.ll` 中：

```llvm
%2847 = getelementptr i8, ptr %2, i64 51108
%2848 = load i32, ptr %2847, align 1
%2849 = add i32 %2848, 1640531527
%2850 = xor i32 %2849, 1779033703
```

`%2` 是本次调用入口读取的 Guest.data SSA，单元为四字节
`Guest[0xc7a4..0xc7a8)`，相当于已检查的初始SP0xc7f8减84。
`02-control-flow.ll` 对应LOAD为%2589，地址/宽度相同。

该单元只有两个同宽最近定义：memory.valid79初始化enc0=138502176；
memory.valid105写入实际%ECX.0。后者在每个继续分发的业务路径末尾，位于
工作数组写入之后。回溯先遇到它就终止，不能因为更早的数组写地址未知而
否定这个最新覆盖。

旧完整fallback使bb_4765多出image.table.fallback的slot4入边；沿bb_4765和
fallback形成没有新STORE的逆向灰环。将已证不可能的旧target-switch case
回边移除后，所有实际状态LOAD入边只到上述两个最近STORE。只读诊断的
virtual CFG不是删除许可；真正许可必须由下面的运行时guard和闭合证明给出。

## 联合归纳依据

1. 初始条件来自实际入口比较指令：整个表的48字节及SP均检查相等；失败
   调用原普通root。ELF权限、候选表内容和manifest都不提供immutability事实。
2. Base在任何第一次间接分发之前，原程序与候选图执行同一段代码。每条
   继续到首个lookup的STORE都已证明不碰表，bounds/error分支原样。真正的
   decoded SSA在memory.valid107经过`decoded > 5`的原false边后才到查表。
3. 假设第k次lookup前表仍与入口相同。原index bounds把此次下标限制在六项
   表域，原actual target LOAD因此只能选六个真实目标之一。
4. Step从每个真实目标检查所有分支，直到下一lookup或终止。所有可继续
   路径SP稳定、写入保存表，没有未证明调用。原bounds在下一lookup仍成立。
   因此第k+1次满足相同条件。退出路径的原输出写/调用没有被删除或推理为pure。

域证明只借原比较分支、整数表达式和完整PHI入边，不借新index switch的
case/default，不把out-of-domain当成终止路径跳过证明。state6在原bounds
分支去finish，没有被虚构为第七个表项。任一late alias、unknown call、
坏bounds、缺失入边或预算失败仍使整次操作只读拒绝。

## 新模式提交什么

仅完整闭合后，提交同一个已验证proof Module中的candidate函数。保留
同次实际Guest target LOAD及原UnknownTarget快照/出口；index switch的
default指向该原Unknown出口。它不再指向“加载任意目标后跳进任意机器块”
的旧target switch，因为归纳已证明这些额外case边永远不发生。

没有添加assume、unreachable或noalias。普通root、入口miss与其完整原始
分发仍保留。proof失败不提交任何部分CFG、常量或metadata证明。

后续先snapshot唯一原状态LOAD，调用独立私有`forwardGuestCellLoad`，沿全部
predecessor slots建立最近定义图并在exact STORE处截断，再运行现有
InstCombine/GVN、predicates和control-flow。Cell SSA只转交真实STORE值，
不枚举状态或解释未知调用结果。最终仍以中央分发退出业务可达SCC验收；
仅消除旧fallback或得到状态PHI不等于完成去平坦化。

当前为本地实现/回归准备，尚未获得本阶段VM编译、执行、native语义或
完整CTest结果。旧010ded实测失败配方及所有254个payload保持冻结。
