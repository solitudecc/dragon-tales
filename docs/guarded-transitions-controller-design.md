# 自动 guarded transition controller（实现与验收边界）

范围：仅将已有常量 Guest 地址的实际 LOAD→switch 上下文转换为带真实值检查的直达边。不是表内容常量折叠，也不是完整循环去平坦化。显式 builtin `guarded-transitions`，默认流水线不变。

入口使用现有 guarded-entry manifest 的 view_symbol 选择函数，memory rows 仅作为候选提示。解析限制256行/4096字节；拒绝坏十六进制、重叠、地址溢出。候选从地址对应的完整整数宽度字节解码，必须是当前 switch 中已有 case。只接受 little endian、AS0 pointer/index 均64位；拒绝不支持的宽度、非 simple LOAD、非同块 LOAD→switch、非canonical Guest base/byte GEP 常量地址。所有选择都要在当前 IR 重验。

无论提示正确、过时或恶意，核心必须执行真正 LOAD、比较其实际值与候选、相等才直达原 case；不相等使用同次 LOAD 值进入完整 switch/default。controller 不添加 assume、readonly 或 noalias，不删错误出口。manifest 的角色仅为启发式选择。

每个 switch 的每个真实 incoming successor slot 作为独立候选；冻结 block ordinal、seed successor slot、target ordinal，不持有跨事务 LLVM body 指针。core 成功后全部重新收集，重新验证边/LOAD/case。此方案依赖 core 明确承诺原 block 顺序前缀保持、新 block 只追加；否则需改为核心返回稳定映射，不能猜旧指针或名称。

一次调用只处理冻结原候选，成功或失败均不重排。新 clone/fallback 不加入本轮。新生成 block 的 terminator 可加专用 generated 附件，仅用于后续调用跳过生成 seed/target；伪造该附件只能抑制优化，不能创造证明。原 seed slot 改接后不再满足冻结边；重复 successor slots 按 slot 分开，不按 predecessor 指针合并。fallback 的 switch 消费别块 LOAD，也不满足同块候选形状。

全调用的候选数、rewrite 数、复制块/指令均有有限预算。每次 core 使用剩余额度，预算拒绝必须 incomplete；不提高既有 GuestSSA 或 dispatch 预算。候选发现也要有扫描量上界，避免巨大伪 manifest/函数造成无界工作。公开 options 默认值分别为 64 个候选、32 次改写、128 个复制块、16384 条复制指令；上限分别为 4096、4096、4096、262144。核心每次事务另限 4096 块和 65536 条指令。

计数应至少包含 available、complete、changed、candidate_edges、attempted_edges、rewritten_edges、unresolved_edges、rejected、budget_exhausted、cloned_blocks、cloned_instructions、original_switches_retained、fallback_switches_created。后两项分别核对最初所有 eligible target 和新增 fallback；绝不将 guard 比较次数统计为原 switch 消除。

回归设计：

- 正确候选匹配实际 LOAD，成功 case 直达；错误/过时候选仍走同次值 fallback，原 default/全部 case 保留。
- 无 manifest/非法 hint/不匹配地址/非原 case 不改 IR；恶意 generated metadata 只抑制优化。
- 共享 terminal 多入边、同 seed 重复 successor slots、PHI 外部 liveout，原路径和失败路径保留。
- 一次成功后旧 body 指针全部失效仍可处理第二个原候选；多个函数中只选 manifest 指定 view。
- 第二次调用不扩张原来新生成的 guard/fallback/自环；原未知 incoming 可保留。
- budget 正边界、0预算、真实独有候选超限保留 incomplete/原 IR；拒绝事务无副作用。
- table 值和 alias 写可变的原生对照：比较完整 regs/Guest/Exit，故意跳过比较或复用旧 LOAD 的坏 IR 必须被检出。

已落实：original_switches_retained 统计全部初始 eligible frozen target switches 中最终仍存在的 switch（包括预算0未改写者），不是全 module switch 数。core agent已确认并承诺原block前缀顺序保持，测试核对两次事务后的原前缀。无LLVM cleanup插入controller事务之间。
