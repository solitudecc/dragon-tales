# J 部分候选与未知CALL：独立原生预注册

四模式plain、table_before、table_after、call_inside；全新standalone LLVM IR producer、顺序byte-array参考模型及独立C helper。原I/H矩阵和模型均不修改。输入的unknown state基础值0/1/49/50/51/UINT64_MAX，target为43/777/default99999，condition0/1，loop上界0/2/3，size519/919/920/65536，regs/Guest独立或regs=Guest+32重叠，共3456基础输入。另增加63/64/2^63边界子矩阵：target43、condition0/1、limit0/3、size519/1023/1024/65536、两种布局，每模式96，共384；总3840唯一输入，每模式960。不为新增边界删除基础输入。

CALL在分析IR中只有声明；实际原生执行链接独立C参考实现。helper每次增加host callcount，按真实顺序写regs0/8/16/32/40/48/56、Guest80、table[50]及合法unknown-state槽。CALL后真实reload state、condition及side，再select(condition,50,postcallUnknown)。regs=Guest+32使Guest80与regs48真正别名；读取顺序与所有clobber均按共享arena顺序建模，不通过noalias/ABI假定保留任何寄存器。入口显式缓存的x/loop limit是原IR真实SSA值，不能混称callee保存寄存器。

未知值50可自然命中预测，0/1/49/51及表被改写时可进入其他既有case或default，63/64/2^63/allones覆盖guard边界。state<64和实际表地址+8<=size在LOAD前执行。native背后始终有完整64KiB Guest allocation；helper接受raw pointer而非size，允许在逻辑descriptor size之外写实际已分配内存，不能假定unknown CALL尊重该逻辑范围。size519、919、920及1023/1024逐一测试真实LOAD边界，所有CALL副作用即使随后bounds失败也比较。

table_before/after在实际target LOAD前/后写777或99999。call_inside额外在terminal块调用helper，必须保持原块且不复制；每轮真实callcount和完整clobber仍比较。循环原来有重复seed slots和旧loaded PHI，所有原路径保留。核心只守卫同一次actualLOAD，错误hint时完整原switch/default回退；不重读表，不把已知select臂替换整个表达式。

Public API预注册：旧guarded-state-corridors四模式保持不变；new partial三正例各1copy，call_inside0。normal hint table[50]=43，另wrong-hint=777在plain上也原生执行。每次发生partial commit必须`partial_candidate/attempted/rewritten_edges=1`、unresolved0、complete0/statusIncomplete；resumed应[1,0,0]且final IR等于normal，不能把后续无新候选成功误称原值集完备。bad/zero/单块预算不修改IR。检查同SSA fullfallback、七case/default、shared good重复PHI实际槽数、CALL及postcall LOAD仍在。

每构建O0/O2 before/normal四模式，另plain wrong-hint，共17280正常全状态观察：before7680、实际改写normal5760+wrong1920=7680、未改call_inside1920。原输入3840，wrong-hint及重复优化/构建不算新样本。5故障：跳CALL、用precall state、强制known50、afterSTORE错误重读、删除fallback case777；共9600故障观察，实际应错由独立故障模型差异确定且必须全部拒绝。每正常帧11字段损坏，190080 comparator controls。28条gzip流26880帧完整保存。

比较return、callcount与73728字节完整arena，含两种映射下完整regs、64KiB Guest、七Exit、descriptor/config及其余字节，不归一化、不排除。CALL的C函数原型为构造合法LLVM fixture所给；框架看不到helper body，本组验证保留未知效果及正确后缀预测，不声称自动恢复任意二进制函数原型、解析任意syscall或完整去平坦化。业务返回与边界失败单列，真实ELF/observer另组。
