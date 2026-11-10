# 通用有限值集合与间接目标完备性

`ValueSetAccumulator.hpp` 提供一个独立的单调合并层，供有限值分析和间接分发器复用：

- `FiniteValueAccumulator` 对有限集合做排序、去重和并集；宽度冲突、Unknown 和超预算会永久转为 Unknown；
- `IndirectTargetAccumulator` 合并所有可达路径，并明确区分“候选集合已完备”和“目前只收集到一部分”；
- 不会为了满足上限截断集合，也不会在存在未知路径时发布部分 target 列表。

该层不按 Tigress、OLLVM 或函数名匹配。特殊 dispatcher 可以把现有有限值/内存分析结果逐路径送入，
再根据 `complete` 决定是否允许发布 CFG 边。它本身不改变 Graph，也不替代现有 `FiniteValues` 的预算和别名规则。
