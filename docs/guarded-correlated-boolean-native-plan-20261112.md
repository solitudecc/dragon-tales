# I 关联布尔值独立原生预注册

另建八模式shared、history、three、four、unknown_arm、freeze、table_before、table_after。复用固定H源10d50a43的IR形状及初始内存工具，不编辑H源或其模型；本组单独顺序byte-array模型，输入仍四x边界值、loop0..3、四target及四实际size，共2048唯一输入。

真实条件来自caller-owned config独立字节的simple i1 LOAD。输入字节只为0/1，循环seed使用i8 STORE翻转，避免i1 STORE填充位不确定；3/4个条件初始位分别由x低位、n高位、target序号低位、x高位提供，固定矩阵涵盖全部8/16个布尔组合。source实际LOAD/freeze、flags、边界、state A→B复制、fresh历史值、所有副作用保留。

shared实际表达式select(c,0,8)|zext(c)仅有1/8；history使用上一轮c作select、当前c作zext，可有0/1/8/9，初始unknown入口也保留。三个独立bool枚举0..7；四bool超过key预算保持未改，unknown_arm任一scenario有非i1 runtime LOAD也整候选拒绝。freeze模式明确保留源freeze。table前/后实际写入777/99999覆盖合法中央case与full-default，不把任何hint视为内容证明。

预注册精度：shared/three/freeze/table_before/table_after各1个terminal复制；history须真实包含head/fresh i1 LOAD并复制initial和两个重复seed槽共3边，partial1、max1续做[1,1,1,0]，不是把仅terminal复制冒充历史值覆盖。four/unknown_arm0改写；bad/zero/单块预算保持不变。每个small set使用同次targetLOAD且保留fullfallback，重复case/default PHI按实际边数检查。任何未达门槛如实失败，不调整模型或缩减输入。

每构建O0/O2、before/normal八模式共8192正常全状态观察；实际改写after3072，未改four/unknown_arm1024，before4096。7故障：错误拆开共享条件、fresh误用historical、丢第三bool位、无条件信任提示、LOAD后STORE重读、丢tableSTORE、错共享目标PHI；共3584故障观察，独立故障模型实际不同的输入必须全部被正常比较器拒绝。每正常观察11字段损坏，90112比较器controls。46条gzip流11776帧全部保存重放。本组2048输入；构建/优化级重复不增样本。

比较完整return、calls、73728字节arena：寄存器、64KiB Guest、7Exit、descriptor、config和其余字节。零排除或归一化，size失败与业务返回分别统计。shared条件相关性只是改善hint，仍不证明表不可变或完整去平坦化。真实四ELF/observer另组。

## 另加真正触发retry的未知整数臂

原unknown_arm仅在select条件使用c；旧枚举不会访问该条件，因此其拒绝不单独证明Boolean scenario机制。原8模式完整冻结保留。另文件guarded_correlated_unknown_scenario.py在相同未知select结果外OR `(zext(c) XOR zext(c))`：新运算恒零，不改变正常语义，但旧枚举会访问i1；retry某个完整赋值仍遇未知整数臂，必须整候选拒绝。该独立IR与同一unknown_arm参考语义模型对拍，复用原256输入，不算新增独立输入。before/after O0/O2共1024正常，另错误把零项改1控制512次观察；11264比较器控制，6流1536帧。追加不得替代原8模式或改变其门槛。
