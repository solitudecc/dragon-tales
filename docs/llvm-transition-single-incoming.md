# 目标路径事务中的单入 PHI

该扩展允许消除路径首块中最后一个实际原入边对应的 PHI。它不允许空 PHI，也不为历史值填入 undef。只接受：PHI 恰有一项、incoming block 就是当前 seed；输入是 region 外且支配 seed terminator 的指令，或本函数参数、整数/空指针常量。region 内定义、self 历史依赖、显式 undef/poison 保持拒绝。

发现阶段和 core preflight 使用同一个内部规则。发现规则不修改 IR；每次 core transaction 仍以当前函数和当前边重新证明。preflight 收集待消除 PHI 并为替换 uses 计费，仅在临时函数内执行 RAUW/erase，随后沿既有算法复制路径、修复 side exits/live-outs、verify 并提交。预算不足或验证失败不会留下部分原函数修改。原 Module/Function/Argument 身份、原块数量和顺序保持，新增块仍只追加。

原指令预算仍计入被选掉的 PHI，属于保守复制成本而非净增长。单入替换的 uses 与既有快照/live-out uses 共用 8192 上限；没有增加原默认模块或路径预算。重复 successor 槽在第一轮仍按真实槽保留剩余 PHI incoming，最后一槽才触发此规则。未知副作用及返回原 seed 的边仍执行原代码，seed 中的 LOAD/freeze 在下一轮重新执行。

独立回归 `TestLLVMGuardedSingleIncoming.cpp` 使用旧版已存在的 core 与 pipeline API：测试外部 SSA 快照、Module/Function/Argument/metadata 身份、原块顺序、原 side exit 和回 seed、重复槽两次处理、undef/poison 与预算不变拒绝，以及 controller 单入门禁。未修改旧 core 回归或原 I history 严格三次复制门槛。构建、旧失败、新原生和完整测试结果由集成任务另行记录。
