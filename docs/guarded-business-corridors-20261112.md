# 业务状态写入到分发器的有界路径恢复

基线 `0c0717f46c5b14b5144c3e61a3da368273eeb903`。新增显式
`guarded-transitions(business_corridors=1)`，默认行为、原模块预算、C/Python ABI
不变。未知CALL/同步/volatile/atomic等在路径搜索中保持边界，不能假设没有内存效果。

从实际原始 successor slot 向分发器恢复有限路径，普通读写、bounds、侧出口与原分发
全部保留。常量Guest STORE与静态表字节只选择**预测值**，不证明状态槽、alias或表
不变；错误提示、无关STORE和后续写入均由最终实际target LOAD的相等检查与同值完整
fallback兜底。跨交易重新取原block ordinal、逐槽重验，重复原边分别处理，预算未处理
槽可在后续调用续做，不永久封禁整个业务块。

路径32块/2048指令；搜索32768边、每terminal512个已访问块及累计1000000扫描单位；
原模块64候选/32改写/128块/16384指令不变。真正访问的提示指令全部计费，原块effect
准入缓存；扫描耗尽在第一笔交易前退出。细节见[设计与边界](guarded-business-corridors-design.md)。

## 当前验证

- 最终新增6cases/223 assertions；连同原guarded组件主/无Z3各16cases/620assertions。
- 全CTest主1081/1081（120.13秒）、无Z3 946/946（106.21秒）。
- 最终6case测试在旧e96/bcf库各6cases/55assertions、5cases/12assertions失败；
  新选项不认识时，测试明确运行旧default，以缺少实际改写/预算报告触发失败，
  不把旧语义称为错误。原5case来源和实际失败日志也保留。
- 中间v1已有5cases/210通过，但漏计反复扫描大seed的成本。最终压力测试在冻结
  d37中间库真实运行，1case/13assertions中5失败；新版本在交易前拒绝且IR不变。
  夹具为4096条纯指令seed、256个dynamic终点及1个本可提交的constant机会；不是
  仅用已有单路径长度上限冒充扫描预算回归。
- 首次独立旧测试编译缺Catch头路径、第一次中间测试命令的可执行路径错误127均原样
  保留。修正命令后才取得上述实际回归失败，不把基础设施错误算检出。

冻结运行库及source-v2的基线+patch保存在
`~/vmc-runs/guarded-business-corridors-20261112/`：

| 构建 | backend SHA256 | shim SHA256 |
|---|---|---|
| main-v2 | a9efefec601995481c6c3acceab55c6cf9a571e32b743c2f708af1b117269282 | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |
| noZ3-v2 | d3977cb6eab9a28ffe7c0f1ffc7de48d3a0b0962083fa15ab7aefb155ddd6659 | 061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0 |
| 中间v1（预算反例） | d37b918a45632c2e57c4a14949d7955f60a7509ae1eb00099be9a3a0abe19a4c | 953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84 |

独立原生主/无Z3各3072完整状态匹配：1536 before、1280实际变换after、256保守CALL
未变after；1536业务返回/1536已声明size失败出口。2048错误IR observations中的
1410指定错误及33792比较器控制全部检出；每库40保存流/5120帧重新审计不重计执行。
源、输入和模型始终未改；默认/禁用输出及parsed-before跨库逐字节一致。
见[独立原生报告](guarded-business-native-validation-20261112.md)。

## 真实结构收益与未完成部分

独立四IR探针按默认、显式64改写/256块、显式64改写/512块比较；候选64、指令16384
和原GuestSSA/dispatch预算不变。cffO0分别改写32/45/49条，复制120/250/290块，
指令1144/2808/3320；最后一组24terminal+25原STORE分支全部改接、预算0。此前13
是业务起点数，不能当STORE边总数。state41实际候选67105已生成同值LOAD guard。

O2仍24/3条原terminal；comboO0仅新增1业务成为5条，搜索截断仍incomplete，
即使已发现候选的remaining为0也不能说整函数完成。严格成功子图闭合检查仍四组
失败，原中央dispatcher可达，所有普通条件及自然业务switch边必须保留。

单独尾部 `mem2reg,instcombine,gvn,instcombine,dce` 探针（无SimplifyCFG）使cffO0
25个复制状态条件都成为字面量false，同时49个实际target guards及完整fallback
仍保留。IR还显式带着常量条件的两个CFG successor；下一组件只消费已证明常量
的分支，不能把未知bounds或targetguard猜成真。该探针原生次数为0，不混入上述
独立模型或真实完整B验证。

完整冻结B+本组件现已完成C16/LLVM16全状态匹配与144/144错误控制，16原输入
逐字节相同，Graph/source/pins不变；真正原生finalIR有49/24/5/3个guards，四组
严格闭合均为false。归档及详细审核另行提交。**此实现里程碑不是完整循环去平坦化
验收。**没有新增单指令/IGNIL/69输入campaign，也没有独立二进制输出功能。
