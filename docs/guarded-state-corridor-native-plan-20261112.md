# F 独立原生验收预注册

独立源 `guarded_state_corridor_oracle.py`（fd3a6b296c5d7594e03ce168d78cdc2e28b7d1a826eeceea2195ac3994982f81）派生新六模式，不修改封存E模型或输入。实际公开 `guarded-state-corridors` 调用，driver初始SHA e6c28634477fef6a34a088f32acfe6195f6214906449965f6367ef7ac6b27197。

循环route带iteration/history/oldtarget真实PHI；left双重复slot和right单slot进入head，state PHI分别34/37；unknown第三入边携运行时LOAD值，不能误用其它入边常量。第二层index PHI经state+0/shl/add形成真实Guest表地址。循环0..3次重新选择route，fresh LOAD/freeze和旧history不同，必须保持外部/回入SSA与边界sideexit。

六模式为普通、select i32(0/50)+zext、LOAD前改表、LOAD后改表、路径内未知CALL、保留add nuw nsw state+0的原式（运行输入state0..63，故无overflow UB）。四个x边值、四循环上界、八目标初值、正常/不足Guest size，共1536输入。所有64表槽由独立初值模型初始化，提示仅4个文件行且经常与运行时实际值不同，最终实际LOAD guard/fullfallback必须保证等价。

normal一次与resumed连续5次max_rewrites1两组actualAPI输出。普通/写表/flagged应复制三个原slot；续做每次恰1，第三次完毕，第四五次稳定；unknown边保持原head。select/cast允许以有限集合预测最短terminal单copy，不强行要求不必要的整段复制。CALL路径完全不复制。bad manifest、1block预算、0rewrite预算不改IR。所有actualLOAD/fallback同SSA、original metadata/Module/Function/Argument/global身份必须保留，flagged原式不被剥除。

每构建before/normal/resumed × O0/O2共9216正常全状态观察，其中before3072，实际变换after5120，未改CALL after1024；4608业务返回与4608边界失败分开计数。完整73728字节arena、返回值和callcount不归一化、不跳过。七故障为无条件信任预测、错fastcase、LOAD后写表错误重读、丢STORE、错重复PHI、fresh替代old history、right37错绑为left34。每构建3584故障观察，应错数由独立模型实际差异确定；101376比较器控制调用正常全状态比较路径。50条完整gzip流/12800帧保存后重新逐帧审计。

main/noZ3分别执行，相同1536输入重复不冒充新增样本。此组不替代四真实ELF的full closure与loop observer，也不声称文件表只读。首次13份before/fault IR在VM均opt verify通过，未调整输入追candidate结果。任何失败保留原文；只允许明确基础设施修复，不降低门槛。
