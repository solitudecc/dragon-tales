# H v2 路径STORE独立追加组

原H八模式、2048输入、模型SHA10d50a43不改。本组为另名mixed、mixed_table_after，两模式各256输入；使用相同四x边值、loop0..3、四target、四实际size界限，完整arena比较范围不变。

seed仅i32 STORE8到独立槽280，没有有限i64 Guest STORE。head新增path_state PHI：初始真实寄存器LOAD未知值，两个重复seed槽分别输入50。实际size>=296后才STORE path_state到A，再实际LOAD A→STORE B→LOAD B→table LOAD；所有原始bound/check/fallback保留。fresh config LOAD/freeze和历史target值仍每轮执行、独立携带。mixed_table_after在target LOAD后以未知寄存器LOAD写table，避免更近的有限STORE提示挡住本次要测的中途PHI STORE；该值由独立输入定为777或99999，并包含完整内存效果。

新driver manifest增加state50→合法候选127，仅为提示。旧guarded-state-corridors两模式应不变；H v2两原seed槽均应实际clone，正常2边、预算1续做[1,1,0]，未知初始边不改。实际small set仅127，同次LOAD完整fallback保留，shared good PHI原3槽+每复制3fallback槽。bad/zero/1block不变。

每构建before/after两模式O0/O2，共2048正常观察；512唯一输入，重复构建或优化级不算新样本。四独立故障为选中seed值50错误换8、未知初始边错误假定50、丢中间STORE、LOAD后改表错误重读，共2048故障观察，模型有实际差异者必须全部拒绝。正常11字段比较器损坏22528次；16条gzip流4096帧全审计。所有返回、calls、寄存器、64KiB Guest、7Exit、config与arena余字节都比较，不排除、不归一化。

本组用于H v2新增候选发现域，不把提示当内存或别名证明；原H v1的API结构证据单独保留，不把未执行原生算通过。
