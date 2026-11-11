# Constant branches 独立验证（2026-11-12）

本组只验证已是 LLVM `ConstantInt` 的 br/switch 清理。动态 target LOAD、未知
调用、同值 full fallback 不因候选提示而被假定为常量。`complete` 只表示本次
冻结候选处理完，不表示完整去平坦化；实际真实样本闭合由主任务另报。

## 未改模型的兼容组

从已双端核验的 business 原生包逐manifest SHA复制六份实际 enabled IR，
复用原封不动的 `guarded_business_oracle.py`（SHA19bb40a…）、768输入、完整
状态比较和八类错误控制。公开 Pipeline 显式执行
`mem2reg,instcombine,gvn,instcombine,dce`，之后才接新 `constant-branches`。

旧 a9ef 库前置tail实际留下17个literal terminator：plain3、state_after4、
table_after3、table_before4、unrelated3、call_path0。新pass准确消费这17个，
并保持call_path的pre/post字节相同。旧/新前置tail输出也逐字节一致。该0候选
CALL模式的执行仅是兼容观察，不算新增折叠收益。

旧库新增pass的8次调用（六post、小shape的fold/zero）全部明确不可用，保持
输入，不隐式回退；这不是旧库错误语义。新库max_folds=0对小shape返回
incomplete且未修改输入。每次driver调用都verify模块，检查Module/Function/
Args身份、原属性和其他globals未变；driver只编译一次，用LD_LIBRARY_PATH
切冻结运行库，旧rpath可被覆盖。

## 新literal重复边形状

独立派生一个常量switch：选中目标有两条重复successor slot和两个相同PHI
incoming；后面还有恒真br。dead目标包含可观察Guest1536 STORE。
fold-only实际折叠1switch+1branch并删除1dead块，两个generated marker保留，
原动态LOAD switch和业务循环仍在。使用原plain的128输入子集，正常参考
仍为原模型，不改输入。两个真实错误控制分别跳到dead目标、删除业务live
STORE；独立故障预期仅在原完整状态模型上覆盖Guest1536/Guest128确切字节。

## 原生与保存流

main和no-Z3都已完成以下O0/O2矩阵及各自48流/6144帧全部保存审计。14份实际API输出和14份parsed-before（另16份control副本）在两构建间逐字节一致。冻结源、输入、模型与控制始终未修改。

| 每个构建 | 原业务模型兼容组 | 新literal形状组 |
|---|---:|---:|
| before观察 | 1536 | 256 |
| after观察 | 1536 | 256 |
| 完整状态匹配 | 3072 | 512 |
| 错误IR observations | 2048 | 512 |
| 指定错误全部捕获 | 1410 | 384 |
| 比较器控制全部捕获 | 33792 | 5632 |
| 完整保存流 / 帧 | 40 / 5120 | 8 / 1024 |

每构建合计3584正常完整状态观察，其中1792为业务返回、1792为受检查size
失败Exit，不能称为3584次业务根成功返回；另有2560错误观察、1794指定故障、
39424比较器控制，48流/6144帧。业务组after中256观察来自未变CALL模式，
另1280才是新pass有实际折叠的输入。新shape128输入属于原768输入子集；
跨形状、优化级和构建重复不增加独立输入数量。保存审计逐帧重比并读取
实际Exit.kind，不重计原生执行。

范围是返回值、callcount及完整73728字节arena（含完整64KiB Guest、寄存器、
描述符和七个Exit字段），没有跳过、排除或归一化。freeze输入在本原生矩阵
中有定义；unknown/undef/poison的严格准入另由核心结构测试覆盖。本组不是
新的机器指令/IGNIL/69样本campaign。

## 来源与复现

- frozen reference：`19bb40a10559a6ea3c385f60c590ad3c809a6e69e7fcd5e6d108c468c12888bd`
- public driver：`9cee43f198508cb500ee015ab86a95a318980615f855559850d31eb5ae040ea0`
- new shape/model wrapper：`b096f3133b3dcff1a8dd12fe83a3b8f5ae71e06ae568f568de476bb0b19920f6`
- main backend：`774c9fbb2b289fa95f42ce782424263ea67e9337c50bdfef2290d72bec993080`
- no-Z3 backend：`ed9442eef0b05c8e7debe3814e4e84e582ffb9f7d44ee11fc4762b02428723bf`
- main shim：`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`
- no-Z3 shim：`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`

VM namespace `/home/cc/vmc-runs/constant-branches-native-20261112`。
`frozen-source-bindings.json` 绑定上阶段逐payload来源，`baseline-main/` 保存旧
不可用结果和前置tail，`main/`、`noz3/` 分开保存实际API结果与原生完整流。

```sh
python3 -B source/constant_branches_prepare.py /home/cc/vmc-runs/constant-branches-native-20261112 main /home/cc/vmc-runs/constant-branches-20261112/runtime-main-v1
python3 -B frozen/guarded_business_oracle.py run main/pre main/post main/native-O0 --opt 0
python3 -B source/constant_branches_shape_oracle.py frozen/guarded_business_oracle.py run shape-before main/shape/fold.ll main/shape-native-O0 --opt 0
python3 -B source/constant_branches_saved_audit.py /home/cc/vmc-runs/constant-branches-native-20261112 main main-saved-audit.json
```

输出目录须不存在，避免覆盖失败；换O2/no-Z3执行其余固定组。父任务已在
双端gzip验证后退役旧C运行库raw；baseline文件仍保留，复现旧执行需先从
父任务归档恢复，不能把旧rpath当仍存在的文件。

静审对照 [LLVM21.1.8 Local.cpp](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/llvm/lib/Transforms/Utils/Local.cpp)：工具本身有unknown简化路径，因此调用方ConstantInt门槛必要。
[BasicBlockUtils.cpp](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/llvm/lib/Transforms/Utils/BasicBlockUtils.cpp) 的EliminateUnreachableBlocks只依入口CFG可达性删块，不另作一般terminator折叠。

完整独立包 `constant-branches-native-20261112.tar.gz`：625文件（624 payload + manifest），SHA-256 `468db785613171ba86e0fad8d0e751cf3eb66862966acf40b147a3d4ca648423`。VM原展开及tar、Mac tar及完整展开均已exact file-set/size/SHA核验；VM根 `/home/cc/vmc-runs/`，Mac根 `/Users/c/Desktop/deob/validation-artifacts/`，同名展开目录与外置核验收据可复查。封包README为封包前报告，最终SHA以本文及外置收据为准。所有原始结果与控制均保留，没有删除任何原生流。
