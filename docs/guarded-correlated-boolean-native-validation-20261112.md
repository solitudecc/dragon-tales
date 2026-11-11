# I 关联布尔预测：语义通过，原历史精度门槛未完成

本组**原 whole precision gate 仍为 false**。history预注册要求3个copy及max1续做`[1,1,1,0]`，实际default只有2个，续做为`[1,0,0,0]`，最终resumed IR也不等于normal。原脚本、失败退出码、所有轮次IR和模型均保留，未修改门槛。下面的正常语义对拍验证已产生的改写及保留路径正确，不将其等同于完整去平坦化或续做能力完成。

## 冻结模型和范围

原8模式source SHA `43846283d7b522d9ee5bae110507e365f52f3d14eaf5a89c6abff90f43defa4e`：shared、history、three、four、unknown_arm、freeze、table_before、table_after。2048唯一输入，四x边值、loop0..3、四实际target、四size边界。i1 LOAD读取明确0/1字节，seed以i8 STORE翻转，避免i1 STORE填充位不确定性。新旧执行的bool分别建模；真实flags/LOAD/freeze、内存修改、bounds和同次LOAD完整fallback保留。

另新增unknown-scenario IR，SHA `e03c41cbe6bfd55a29aa1c08a8fe5263ca3d3aafcb6c3319a34c0b6e8588ce38`。原unknown_arm只在select条件使用bool，旧枚举不访问该条件，不能单独证明scenario拒绝。补充在未知select外OR `(zext(c) XOR zext(c))`，运行语义与原unknown_arm相同，但确实迫使旧枚举访问bool，再由未知整数臂拒绝整体候选。它复用原256输入，不算新增独立样本；原8矩阵完全保留。追加前计划的旧SHA及文本按old-api pin另存于`failures/plan-before-supplement.md`，没有覆盖旧记录。

完整比较return、calls、73728字节arena，包括寄存器、64KiB Guest、七个Exit字段、descriptor/config及其余字节。host Guest地址是初始化输入，不是结果归一化。零排除、零跳过。main/no-Z3和O0/O2重复同输入不增加样本量。

## 旧失败和新精度边界

旧H2运行库上13行public API均可调用，正例IR通过verify且未改写；预注册shared改写门槛实际失败。这是已有builtin的精度回归证据，不是旧语义错误或API缺失。

I两构建actual API结果一致：shared/three/freeze/table-before/table-after各1个terminal copy；four与unknown_arm保持0；history候选3、实际尝试3/commit2/reject1，保留1个未处理原边，报Incomplete。partial一次1；resumed四次`[1,0,0,0]`。补充unknown-scenario保持不变，通过其独立拒绝门槛。

history第一次copy把initial_write改接head.transition，原head剩seed的两个重复槽；再复制一个seed槽后，第三copy将清空原PHI，既有core拒绝。max1第一次之后，旧c输入被SSA修复为step的`transition.live18`，经各case PHI汇合原c0与clone c0；后续严格枚举不再能把该历史汇合当一个确定bool。这两个限制分别记录，不能称预算续做已完成，也不因此声称旧wrong-native。

原prepare继续exit1；原strict saved-audit脚本保留。新`semantic_audit`只验证完整执行状态并输出原precision=false；独立`structure`审计按实际生成边检查真实selector、完整fallback、fresh i1 LOAD与重复PHI槽，同时单列原3copy/续做/最终IR门槛失败。它没有替换原门槛。

## 原生和保存流

main和no-Z3分别完整执行并逐帧重放保存流，以下数字为每一种构建；同输入重复构建不算新样本。

| 组 | 正常比较 | before / 实际改写后 / 未改 | 业务返回 / 边界失败 | 故障观察 / 指定全捕获 | 比较器控制 | 流 / 帧 |
|---|---:|---:|---:|---:|---:|---:|
| 原8模式 | 8192 | 4096 / 3072 / 1024 | 2528 / 5664 | 3584 / 1208 | 90112 | 46 / 11776 |
| unknown-scenario补充 | 1024 | 512 / 0 / 512 | 256 / 768 | 512 / 256 | 11264 | 6 / 1536 |
| 每构建合计 | 9216 | 4608 / 3072 / 1536 | 2784 / 6432 | 4096 / 1464 | 101376 | 52 / 13312 |

所有正常比较零差异，指定错误全部捕获，零跳过。两构建共104条流、26624帧保存数据完整复查。原7类故障每个优化级分别捕获128/72/112/36/78/94/84，补充每级128。所有故障先与独立故障模型核对，再经正常比较路径拒绝指定应错输入；没有实际差异的故障输入仍作为观察保留。保存重放不增加native执行数。

原七故障为拆开同一条件、fresh误用historical、丢第三bool位、强行信任hint、LOAD后改表重读、丢tableSTORE、错共享目标PHI。补充故障把应为零的额外项改成1。每个正常快照还逐一损坏return/calls/regs/Guest/七Exit字段，由同一全状态比较器拒绝。

main/no-Z3的42份原组IR及3份补充IR逐字节相同，45个IR故障比较控制通过。用两套冻结H/H2 driver在每个I构建重放30配方，94份旧输出各自字节相同，94损坏控制各通过；没有重跑或重计H/H2原生。

## 运行和证据

`guarded_correlated_boolean_prepare.py ROOT LABEL RUNTIME`严格门槛继续如实失败；`guarded_correlated_unknown_prepare.py`处理补充组。两个oracle `run`都传before、normal、partial占位目录、output与`--opt 0/2`；原生阶段仅before/normal，partial/resumed在本组只有API结构证据。`guarded_correlated_boolean_semantic_audit.py`复核已保存帧，补充加`--supplement`；`guarded_correlated_boolean_structure.py`输出独立shape结果与未满足的原门槛。`guarded_correlated_boolean_compat.py`在新namespace输出旧H/H2对照，绝不写封存旧目录。

依赖的冻结H工具`guarded_store_corridor_oracle.py` SHA `10d50a43f6441f32c5df1da803df69edf4eb61c39d727a26868682b12e1d6b6c`和IR比较工具SHA `daf5854fcfc15c294b6db62c0a2094c4cfaa62afb0d9cc2c3480d692bce4a61e`已随H交付，本组包内也完整保存。

| 构建 | backend SHA256 | shim SHA256 |
|---|---|---|
| main | e09aebc9ec241a5e22bb162d04958e5bda66aab15b73b9c47fbcd956e286a934 | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |
| no-Z3 | 53bd43c99bd32f9b71c6ea07a9ae46c219dcec4ce6b033a2ae0470e2e0ae937d | 061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0 |

VM namespace `/home/cc/vmc-runs/guarded-correlated-boolean-native-20261112`。这是LLVM pass在AArch64宿主上的独立语义验证，不是新机器指令/IGNIL/69输入活动；真实ELF及loop observer另组，不以本组copy计数充当真实闭合证据。包内README在生成包SHA前冻结，最终SHA写仓库文档与包外双端receipt，避免自引用。


封包汇总工具最初在打印末尾误用了循环中的旧报告字典，导致`KeyError: frames`；此前全部审计和summary文件已写出。旧工具和日志保留于failures，第二版仅更正打印字典，模型、输入、比较、precision门槛均未改变。原13行旧API及模型/输入源码绑定另独立核验，追加前plan按旧pin恢复核对；已经退役的H2运行库使用root的完整归档与原ldd/pins证据，不假称raw仍存在。

最终独立包 `/Users/c/Desktop/deob/validation-artifacts/guarded-correlated-boolean-native-20261112.tar.gz`（VM同名位于`/home/cc/vmc-runs/`），SHA256 `205fe9ff8078778aedc6cd67f77d92293087a3480a30754e8a90425856456fd6`。1077 payload加manifest共1078文件，两端tar与完整展开的精确file-set、长度、SHA全部通过；相邻`-vm-archive-verification.json`与`-mac-archive-verification.json`保留结果。原始展开、严格失败和全部快照均保留，未退役本组证据。需要恢复时解开完整同名tar，在独立目录用包内archive verifier重验；不能只拷报告而丢原始流。
