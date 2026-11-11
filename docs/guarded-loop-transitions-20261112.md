# 实际目标校验与业务边恢复：实施里程碑

基线 `1778d5ab248c5e2bf0dd434bb2ff3c5276107a72`。本次新增显式
`guarded-transitions` pass；默认六步流水线、Graph、机器指令语义和 C/Python ABI 不变。
**独立 native 与完整真实样本重放仍在进行，不是完整循环去平坦化的最终验收。**

## 实现

内部 `cloneGuardedTransition` 接收当前 IR 的实际边和有限路径，在原 Module 内的临时函数上
复制路径并修复 PHI/历史快照/跨块 live-out。检查成功才替换原函数体，保留 Function、Arguments、
全局对象身份。原块顺序保持为前缀，新块追加；调用者必须在每次事务后重新取得 body 指针。
失败丢弃临时函数。路径中的普通读写和失败出口按原顺序保留，未知 CALL、同步、volatile/atomic、
EH/debug/address-taken 等不支持形状拒绝。

终点必须为同块 simple 整数 LOAD 驱动的 switch。复制后的**实际 LOAD**与候选 case 比较：
相等直接去原 case，失败用**同一个 SSA 值**执行完整原 switch/default。候选只影响优化方向，
错误候选不是语义前提；不添加表不可变、noalias、readonly 或 assume。

模块 controller 从现有 guarded-entry manifest 读取有限候选字节，仅接常量 Guest 地址上下文。
它冻结原始 successor slots，每次事务后按原块 ordinal 重取并重验；generated 附件只用于抑制
重复扩张。显式预算、拒绝及未处理边都有统计，`complete` 仅描述本次候选集合，不能代表全函数
去平坦化完成。详见 [controller 设计](guarded-transitions-controller-design.md)。

## 当前验证

- 有/无 Z3：新增定向测试各 **10 cases / 397 assertions**，覆盖别名 STORE、重复 PHI 入边、
  原区域回入、历史 LOAD/freeze、身份保持、拒绝回滚、事务后重取及预算。
- 最终全量 CTest **1075/1075**（128.70 秒）、无 Z3 **940/940**（109.22 秒）。
- 最终 Python 接口测试在旧 b6a910 库正常失败于精确 builtin 集合断言；新库通过。新内部 API
  在旧库不存在，最终显式 driver 链接旧库真实报 `undefined reference`，不是旧语义错误。
- 首次生产构建的 WeakTrackingVH 类型错误已修正；首次全量测试仅 Python 精确 builtin 清单
  未包含新增 pass（1074/1075），更新精确清单并增加默认禁用/预算/Unavailable 检查后重跑全量。
- 首次旧 driver 的非限定 `find` 触发 LLVM ADL 编译错误，改名 `findBlock` 后才取得上述旧 API
  缺失证据。第一次错误保留，不能充当旧 API 回归证据。

冻结生产库：

| 构建 | backend SHA256 | C shim SHA256 |
|---|---|---|
| main-v2 | e96ec64725e521e88e8104c4e77bc1103c39dc5593642afe4ce9d7ccce5971c8 | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |
| noZ3-v3 | bcf83d53ffe9f543b5d601a729b9fd16b534e10dc52c56da29b63b3db0ff4480 | 061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0 |

实际构建是基线加保存的候选 patch。VM 证据根
`~/vmc-runs/guarded-loop-transitions-20261112/`，保留 source-v1/v2/v3、old-api/old-api-v2
原始日志、退出码及 source pins。后续归档与原生数字另行提交。

## 真实结构探针与未完成部分

冻结 full-B `f-cff O0` 输入 SHA
`dab36d3d945342c283ecfac097fa84a6ba47de5f0e01e5e030914979586343ed`：
state43 最小 terminal-only 请求成功增加 2 块/4 指令，保留全部 66 cases/default。
实际 Guest[69976] 与 66450 比较，成功去原业务 case，失败 switch 使用同次 LOAD。
独立 `mem2reg,instcombine,gvn,simplifycfg,verify` 后仍保留这条 guard。
这是一次实际 IR 变换与 verify，native 数字为 0，原中央 dispatcher 仍在。

首轮四模块 controller 结构诊断分别改写 15/24/1/3 条边；之后 control-flow 清理会把
f-cff O2 的 24 个 guards 合回 switch，不能把中间数量当最终收益。另有 f-cff O0 的
9 个末轮复制槽 LOAD 经标准清理才产生常量表地址。正在独立验证“先完成清理，再恢复直连”
的调度，并用同一冻结输入和完整状态比较器做端到端重放。

后续验收必须覆盖全部正常业务转移与循环闭合、保留原业务 switch/未知目标/错误回退，
并分开报告快路径与回退。没有新指令/Unicorn campaign 或 69 输入全语料重跑；opt-in
LLVM 层的独立模型、错误控制和真实执行验证尚未完成，不能用本次 CTest 代替。
