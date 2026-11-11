# F：选中状态上下文的独立原生验证

本组调用实际公开 `guarded-state-corridors`，在独立脚本派生新的SSA状态夹具；以前E的模型、输入、门槛和封包完全不改。预测来自显式metadata中的四个表行，运行时完整64槽表内容通常不同；只有真实目标LOAD后的小switch可走预测路径，未命中必须用同次LOAD的完整原switch/default。

## 固定输入和复制边界

六模式：普通两层PHI、select i32(0/50)+zext、LOAD前改表、LOAD后改表、路径内未知CALL、带 `nuw nsw` 的原始state+0。普通输入的route由 `(x+iteration)&3` 选择，left重复两个successor slot与right一个slot进入head；head state PHI分别34/37，unknown第三入边来自运行时寄存器LOAD；第二层index PHI再参与地址计算。未知入边不能借用left/right常量。

route另有iteration/history/oldtarget循环PHI，fresh LOAD/freeze每次重新执行。迭代上界0..3对应最多3次重新进入route，正常业务路径实际执行1..4轮。x为0/1/2^63/UINT64_MAX，八个初始目标包含六种普通目标、原central case777与原default99999；Guest size为65536或不足511，共1536个固定输入。所有state在0..63内，flagged state+0无溢出，不执行人为poison/UB输入。

一次默认调用与连续五次 `max_rewrites=1` 分别产生独立输出。普通/两种表写/flagged各复制三个原slot；续做顺序严格为 `[1,1,1,0,0]`，最后两轮逐字节稳定。unknown边仍进原head。select/cast有限集合只需一个copy，续做为 `[1,0,0,0,0]`，不要求无益的整段复制。CALL路径完全不复制。坏manifest、1block预算、0rewrite预算保持原IR。

每构建15项API/结构门槛包括：每个小switch和完整fallback共享同一真实LOAD SSA，root selected PHI按实际slot绑定；原 `nuw nsw` flags保留；Module/Function/Arguments/global身份与原metadata/sentinel不变。预算续做每轮都实际verify，并保存逐轮IR。

## 原生覆盖及故障控制

使用host-compatible LLVM原生执行，不新增机器指令、IGNIL或69输入语料覆盖。比较返回值、callcount及完整73728字节arena（512字节寄存器、全64KiB Guest、七个Exit字段、描述符及其余缓冲），不跳过、不归一化。

每构建before/default/resumed × O0/O2共9216正常观察：before3072，实际变换after5120，未改CALL after1024。正常业务返回与checked-size失败分别4608；不可把全部观察称为完整根正常返回。相同1536输入跨三路、优化级、构建重复不当作新增样本。

七故障IR分别为无条件信任预测、错fastcase、LOAD后STORE错误重读、丢LOAD前STORE、错重复边PHI、fresh代替历史值、right37错绑为left34。每构建3584故障观察，指定应错数由独立模型实际差异决定。每次正常观察对返回值/callcount/寄存器/Guest/七个Exit字段逐一改坏，调用相同正常比较函数确认拒绝，共101376比较器控制。

main与noZ3各完成9216正常全状态比较、3584故障观察中的1732个指定故障、101376比较器控制，全部通过。每优化级七故障指定应错分别116/32/120/124/90/256/128，共866；其余故障观察在该输入上与原式恰好等价，不伪计捕获。两构建保存审计各完整50流/12800帧同样通过。noZ3实际15项API通过，69份输出/parsed-before/逐轮IR与main逐字节相同；两构建全部原生和保存审计完成，无失败或源修正。

首次13份before/fault IR在VM verify通过；没有因candidate结果修改冻结模型、输入或门槛。旧库上的7组公共回归由主验证实际记录为新builtin不存在，不称旧库语义错误。完整证据保留在独立namespace `~/vmc-runs/guarded-state-corridor-native-20261112/`。

## 复现与范围

```sh
python3 -B source/guarded_state_corridor_oracle.py emit before
python3 -B source/guarded_state_corridor_prepare.py "$PWD" main ../guarded-state-corridors-20261112/runtime-main-v1
python3 -B source/guarded_state_corridor_oracle.py run before main/normal main/resumed main/native-O0 --opt 0
python3 -B source/guarded_state_corridor_oracle.py run before main/normal main/resumed main/native-O2 --opt 2
python3 -B source/guarded_state_corridor_saved_audit.py "$PWD" main main-saved-audit.json
```

driver沿用稳定公开头文件，只链接libdragon，API调用显式指定对应冻结运行库的 `LD_LIBRARY_PATH`。源脚本拒绝覆盖已有输出。noZ3使用相同driver、源、输入和控制，仅更换runtime/label。保存审计逐帧复验50条gzip流、12800帧，验证精确流长度、全部模型/故障/比较器控制、源/运行库前后pins及ldd；不算新增原生执行。

此组不证明四真实ELF全部去平坦化，也不把只复制首个上下文、`complete`或IR verify当作完整循环闭合。真实闭合和loop observer由另一独立组另报。未知freeze/poison及无法枚举状态的结构拒绝由聚焦单测覆盖，不能冒称本组原生执行了这些未定义输入。


运行库为 `~/vmc-runs/guarded-state-corridors-20261112/runtime-{main,noz3}-v1/`。main backend SHA `5d1247db22c263a32114134c7b31187d54de273088159d124175db48121049f0`；noZ3 SHA `02f6accb1aa79ed1f4685d067b1af85f5688764113efc0dd2987a44c2b44ed4c`。shim分别`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`和`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`。本driver实际调用libdragon；完整运行库由主验证包封存，本小包保留原始pins/ldd。


最终独立包为两端 `guarded-state-corridor-native-20261112.tar.gz`：VM根目录 `/home/cc/vmc-runs/`，Mac根目录 `/Users/c/Desktop/deob/validation-artifacts/`。相邻 `*-vm-archive-verification.json` 与 `*-mac-archive-verification.json` 保存包SHA、文件数及完整tar/展开精确file-set/size/hash结果。原始证据不删除。包内说明在计算包SHA前保存，仓库说明另追加最终包SHA。

最终独立包 SHA-256：`d6477abf0ba35e348b14102b5b686767b9cadc1dcffc506861521e48a2631ae1`。589文件（588 payload + MANIFEST），Mac/VM两端tar成员与展开目录完整文件集合、每文件长度及SHA全部核验通过，非抽查。
