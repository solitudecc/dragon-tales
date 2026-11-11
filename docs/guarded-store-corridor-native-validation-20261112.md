# H/H2 STORE 通路独立验证

本报告区分候选提示、真实执行和分析完成度。STORE值只预测表候选；没有证明STORE与状态槽别名，没有把ELF表字节设为不可变。实际LOAD、STORE、flags、size检查、旧入口、循环历史值、unknown CALL及同一次LOAD的完整fallback保留。`complete`只描述本次有界pass，不代表完整去平坦化。

## 冻结验证域

原H组8模式、2048唯一输入；H2另增mixed、mixed_table_after两模式、512唯一输入。原H模型SHA `10d50a43f6441f32c5df1da803df69edf4eb61c39d727a26868682b12e1d6b6c` 保持不变；追加模型SHA `906e8b87247643eb5b60557d089e462b3ef377bce684655e2b581d1f592f1a7a` 通过明确导入与SHA检查复用常量/初始内存工具，但使用独立顺序语义模型和故障规则。输入与范围详见两份native-plan。

每次实际执行比较return、callcount及73728字节完整arena：含寄存器、64KiB Guest、七个Exit字段、descriptor、caller-owned config与其余字节。guest host pointer是初始化输入，不是结果归一化。O0/O2、main/no-Z3重复同输入不计新增独立样本。普通返回与size检查失败分别统计。没有排除、跳过或缩小比较。

## 旧失败与结构结果

旧H v1库8d043b在新增两个场景上能正常调用builtin、LLVM verify和identity检查通过，但normal皆0 rewritten且IR未变化；预注册改写门槛真实失败，完整9个API行保留于`path-api-old-v1/`。这不是API缺失，也不是旧执行语义错误。更早H builtin不存在的旧G测试由root另行归档，不混入本组。

原H首次prepare的PHI解析器假设`%selected`是good块第一条PHI；实际合法输出先有修复历史值的PHI，因此该解析器错误拒绝。原失败脚本、日志、全部IR保留。修复仅在good块内按精确名字查找`%selected`，原边数、candidate、精度门槛均未改，原模型和输入未改。重新执行21行API全部通过，原生尚未执行时不计语义成功。

H2 main和no-Z3各21行原组和9行追加组API全部通过。原组normal七模式各2copies、CALL模式0；partial1，续做[1,1,0]。新增mixed两模式各2copies、partial1、续做[1,1,0]；未知初始边保留。真实小switch和同值fallback及重复边PHI均逐项检查；无效metadata、零预算、单块预算不改IR。

原H1→H2的65份保存IR严格字节相同，65个单字节损坏均被同一比较器拒绝。该项是结构兼容证据，不新增原生执行计数。

## 原生与保存流

main和no-Z3分别完成以下同源矩阵，且保存流逐帧重放全部通过。下表每行是每一种构建的数字，不把两构建重复输入算作额外样本。

| 组 | 正常完整状态比较 | before / 实际改写后 / 未改CALL | 业务返回 / 边界失败 | 故障观察 / 指定错误全捕获 | 比较器损坏控制 | 保存流 / 帧 |
|---|---:|---:|---:|---:|---:|---:|
| 原H八模式 | 8704 | 4096 / 4096 / 512 | 2696 / 6008 | 4096 / 1518 | 95744 | 50 / 12800 |
| H2追加两模式 | 2048 | 1024 / 1024 / 0 | 608 / 1440 | 2048 / 742 | 22528 | 16 / 4096 |
| 每构建合计 | 10752 | 5120 / 5120 / 512 | 3304 / 7448 | 6144 / 2260 | 118272 | 66 / 16896 |

所有正常比较零差异、全部指定错误被捕获、零跳过。故障变体先与独立故障模型一致，再由正常比较器拒绝指定应错输入；无实际差异的故障输入保留为观察，不冒称捕获。每个原H优化级八类指定错误分别47/34/75/101/86/192/96/128。保存重放不增加native执行计数。两种构建共132条完整gzip流、33792帧逐一核对文件集合、长度和内容。

两组main/no-Z3共94份API输出逐字节相同，94个损坏控制全部拒绝。全部source/model/driver/runtime执行前后SHA一致，ldd指向指定冻结库；运行仅切换LD_LIBRARY_PATH，没有重新编译或修改driver。

原H八故障：信任hint、错case、LOAD后改表重读、丢table STORE、错重复边PHI、fresh代替历史值、错误替代state LOAD、丢scratch STORE。追加四故障：seed选50错成8、未知初始值假设50、丢中间STORE、LOAD后改表错误重读。另对每个正常快照的return、calls、regs、Guest及七Exit字段逐一损坏，使用正常比较路径拒绝。

## 边界与证据

LLVM IR在Ubuntu AArch64主机编译执行；这是LLVM pass语义验证，不是新x86/A64机器指令、IGNIL或69输入语料活动。真实Tigress四ELF和循环observer由另一组执行，不能用本组synthetic成功代替真实闭合。

VM证据命名空间：`/home/cc/vmc-runs/guarded-store-corridor-native-20261112`。所有初始失败保留。归档完成后追加双端完整file-set/长度/SHA验证与恢复位置。

## 可复查运行方式

在命名空间下，分别用两个prepare脚本指定冻结runtime和独立label；原组run传`before LABEL/normal LABEL/partial LABEL/native-O0 --opt 0`，追加组run传`path-before LABEL-path/normal LABEL-path/partial LABEL-path/native-O0 --opt 0`，O2同理。追加runner保留partial参数以兼容CLI，但其原生阶段仅before/normal，不把API partial当额外语义比较。`guarded_store_corridor_saved_audit.py ROOT LABEL OUTPUT`复核原组，追加传`--path-store`；`guarded_store_corridor_ir_compat.py LEFT RIGHT OUTPUT`检查精确IR与故障拒绝。

冻结运行库由root单独归档，本包保存完整runtime pins/ldd，不重复打包大库：

| 构建 | backend SHA256 | shim SHA256 |
|---|---|---|
| main-v2 | 926a3abf391b9e1c5bc2d57237d3aa0c78d37d7d25b9f4124d5cd36e25973b03 | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |
| no-Z3-v2 | 5e004c40d57d5df3b39f7e3c2e1769605568d06e79ecf27e1bb5bdef33f4fefa | 061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0 |

包内README在生成archive hash前冻结；最终hash在仓库文档及包外两端receipt中追加，避免自引用hash。未删除任何本组原始证据。

最终独立包：`/Users/c/Desktop/deob/validation-artifacts/guarded-store-corridor-native-20261112.tar.gz`，VM同名位于`/home/cc/vmc-runs/`。SHA256：`64ab18793a776c9f146859b8115f994b79e8b0758578502fc9ba92f7c912a7c6`。1213 payload加manifest共1214文件，两端archive与完整展开逐文件集合、长度、SHA全部通过；相邻`-vm-archive-verification.json`/`-mac-archive-verification.json`保留核验记录。两端完整展开仍保留；如需恢复，在独立目录解开同名tar并运行包内archive verifier，不能只恢复报告而丢失原始流。
