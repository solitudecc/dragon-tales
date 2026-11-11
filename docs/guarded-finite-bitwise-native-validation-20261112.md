# G 有限bitwise候选：独立回归和原生验证

新增精度支持仍属于实际公开 `guarded-state-corridors`，不新增默认pass、ABI或指令语义。有限And/Or/Xor计算候选笛卡尔积，unknown AND常量最多三个置位可枚举最多八个子集；原运算、flags、实际LOAD与完整fallback继续执行。两个select的共同条件关联不在此处证明，候选可以包含额外组合。掩码不是静态跳转界，文件表内容也不是不变事实。

## 旧失败和专属单测

独立新文件 `tests/lifters/TestLLVMGuardedFiniteBitwise.cpp`（SHA360f6e57938a5a2c56b9bcc9aad0ba61f17ff8c93f77f4a448225c453f9cfcef），与生产文件独立编写。冻结F两运行库实际运行同测试，各4cases/77assert：3个positive cases各在 `changed=false` 失败，negative case通过。builtin当时已经存在，这是精度缺口，不是API缺失或旧原生错误。旧失败原文由主验证的 `guarded-finite-bitwise-20261112/old-tests-v1` 保存。

单测覆盖真实cffO2的 `and i32 unknown,1 → or disjoint 24 → zext nneg`；actual table bases90120/73792各PHI{0,50,unknown}和select{0,50}经shl3 OR disjoint；双方有限And/Or/Xor笛卡尔积、known34&1必须仅{0}、And3→Xor1→Or24、zero、八子集、sparse64高位、i128→i64截断、dense/full/动态mask及直接undef/poison拒绝。>3位unknown mask是未支持候选域，不冒称预算搜索已证明所有状态。

## 独立原生模型

在独立文件派生G的八模式，冻结F模型、输入与封包完全不改。native使用base512使完整64KiB Guest可执行；actual高base由单测和真实样本组覆盖。模式为masked i32、两层PHI、select/cast、And/Xor/Or组合、LOAD前改表、LOAD后改表、sparse64高位、dense15拒绝。

四个x边值0/1/2^63/UINT64_MAX、迭代上界0..3、八种初始表目标、正常/不足Guest size，共2048个固定输入。正常业务1..4轮，保留fresh LOAD/freeze、old history/target、state槽STORE/LOAD和真实unknown入边。sparse高位输入必须走原state失败出口；不能绕过bounds或把模截断后的候选地址当合法访问证明。

每构建11项实际API门槛：七模式真实gain，combo_phi复制left重复两个slot和right一个slot且unknown边仍进原head，其它正例各一copy；dense不变。小switch预测集合精确核对，完整fallback使用同一真实LOAD SSA；`or disjoint`/`zext nneg`及Module/Function/Arguments/global/metadata身份保留。bad manifest、1block和0rewrite预算保持IR不变。

每构建before/after × O0/O2共8192次完整观察：before4096，实际变换after3584，dense未变after512。比较返回值、callcount和完整73728字节arena，含512字节寄存器、全64KiB Guest、七个Exit字段及其余缓冲。无跳过、排除或归一化。3840为正常业务返回，4352为原边界失败，不能把8192都称为正常根返回。同一2048输入跨优化级/构建重复不当作新增样本。

七故障分别为无条件信任hint、错case、LOAD后写表错误重读、丢STORE、错PHI、fresh替代历史值、错选state。每构建3584故障观察；每优化级指定应错依次80/16/96/112/48/256/128，共736；两优化级1472个全部应由正常比较路径拒绝，其余恰好等价的输入不伪计捕获。每次正常观察分别破坏返回值、callcount、寄存器、Guest和七个Exit字段，共90112比较器控制。

main/noZ3各上述全部通过，各46条完整gzip流/11776帧保存审计通过。两构建11输出、11 parsed-before及11 round0共33份IR逐字节相同；没有修改冻结模型、输入、故障或精度门槛。保存审计重新逐帧执行独立参考与全部故障/比较器控制，检查流精确长度、源/运行库前后pins、ldd；不算新增原生执行。

## F兼容及盲区

用冻结F driver与相同绝对输入路径，在G两构建下重新执行全部15条F默认/预算续做/拒绝配方。各69份输出、parsed-before、逐轮IR与封存F文件逐字节相同，包括CALL、flags、unknown第三边。未修改F源码，也不重跑或重计F的旧9216观察；G新native没有新增CALL模式，不能假称它覆盖了新未知调用组合。

本组不新增机器指令、IGNIL、69输入语料或跨架构执行。只验证host-compatible LLVM变换；原始文件metadata是候选提示，不是Guest不变假设。四ELF完整闭合/loop observer由独立真实样本组另报，不能用本组gain、verify或流程complete代替去平坦化完成。

## 复现及运行库

独立namespace：`~/vmc-runs/guarded-finite-bitwise-native-20261112/`。

```sh
python3 -B source/guarded_finite_bitwise_oracle.py emit before
python3 -B source/guarded_finite_bitwise_prepare.py "$PWD" main ../guarded-finite-bitwise-20261112/runtime-main-v1
python3 -B source/guarded_finite_bitwise_oracle.py run before main/normal main/native-O0 --opt 0
python3 -B source/guarded_finite_bitwise_oracle.py run before main/normal main/native-O2 --opt 2
python3 -B source/guarded_finite_bitwise_saved_audit.py "$PWD" main main-saved-audit.json
```

noZ3只更换runtime/label，保持同一driver、源、输入和控制；输出不得覆写。driver只链接libdragon，不再链接另一份LLVM共享库。F兼容脚本读取原F namespace，使用候选 `LD_LIBRARY_PATH`，不加载已退役F库；F依赖可从双端完整包 `guarded-state-corridor-native-20261112.tar.gz`（SHA d6477abf0ba35e348b14102b5b686767b9cadc1dcffc506861521e48a2631ae1）恢复到同路径。

main backend `7b76a0a8df46fb41077133dc9569b3690b0b41b76f51081b41050e144ce58c9b`，NZ backend `6e398d4c351cc6e7acf4b1d2ff16938f2d5b3049dd3542810618f02efaaa1251`。shim分别953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84与061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0；本driver实际调用libdragon。完整运行库/生产source pins在主验证包，独立包保留原始pins和ldd。


最终独立包为两端 `guarded-finite-bitwise-native-20261112.tar.gz`，根目录分别 `/home/cc/vmc-runs/` 和 `/Users/c/Desktop/deob/validation-artifacts/`。相邻 `*-vm-archive-verification.json` 与 `*-mac-archive-verification.json` 记录包SHA及完整tar/展开精确file-set/size/hash核验。原始证据不删除。包内README在计算包SHA之前保存，仓库版说明另追加最终SHA。

最终独立包 SHA-256：`41dcbf73f540dcf1947e57b55ec9fe05135a58c0621acb6272b5928c3aad8244`。712文件（711 payload + MANIFEST），Mac/VM两端tar与展开目录完整文件集合、每文件长度及SHA全部核验通过，非抽查。
