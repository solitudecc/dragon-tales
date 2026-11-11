# Guarded transition 独立原生验证（2026-11-12）

实现里程碑 `4c66276`；VM 运行库是父任务冻结构建，本文不声称 VM checkout 的 HEAD 已是该提交。

本组只验实际 `cloneGuardedTransition` 与自动 `runGuardedTransitions` API 的变换等价性。候选 43/99 均是预测，实际 Guest LOAD 必须比较，未命中保留同一 SSA LOAD 的完整 switch/default。新增一个受检查的直达边不等于完整去平坦化；原 dispatcher 仍保留。

## 固定输入与参考

独立 LLVM producer/Python 模型生成 6 种形状：普通、LOAD 前覆盖表、LOAD 后覆盖表、side-exit 回原循环、seed 上未知调用、重复 seed successor slots。每种 128 输入，总 768 个：整数 0/1/最高位/全 1，循环上限 0..3，表值 43/44/99/777，size 65536/511。side-reentry 可以额外经过一次原循环；模型没有跳过该路径。真实 helper 修改表、fresh 数据和寄存器，callcount 也比较。

比较返回值、调用次数及完整 73,728 字节 arena，其中包括完整 64 KiB Guest、全部寄存器区、描述符、配置和七个 Exit 字段。描述符中的原生指针使用每次执行记录的实际地址；不归一化、不排除、不跳过。freeze 的输入在这些原生用例中均为已定义整数，不能把这组测试当作任意 poison/undef 的执行证明。

core 明确复制四块路径 head/check/write/dispatch，验证两个历史快照 PHI 与新执行 fresh LOAD/freeze 分离；controller 实际自动发现单块 dispatch 的入边，不能把其成功计为自动发现完整四块路径。两组分别对 before/hint43/hint99 用 O0/O2 编译；同输入重复执行不增加独特样本覆盖。错误、过期的合法 hint 是等价性正例。

八类错误 IR 控制：无条件信任 hint、比较命中后跳错目标、STORE 后重新读取 switch 值、删除 STORE、复用陈旧 fresh 值、把历史值替为 fresh、重复边 PHI 错值、Exit 字段错值。控制同样完整执行并先与相应独立错误模型比较；仅独立模型确实有差异的输入计入指定故障。返回值/callcount/regs/Guest/每个 Exit 字段翻位还必须被正常比较函数拒绝。

## 执行和保存流审计

主库与 no-Z3 使用同一个 driver 可执行文件、相同源/输入/模型/控制，只有显式 `LD_LIBRARY_PATH` 不同。driver 链接单一 libdragon，避免重复 LLVM shared registration。所有生成的 IR 都 verify，并检查 Module/Function/Args 身份、原属性及非选中 globals 不变。

每个库分别生成 24 个 positive IR，并检查 7 个拒绝：路径含 call、块预算不足、核心缺 case、缺 manifest、坏 manifest、32-bit layout、自动发现缺 case。拒绝保持序列化输入逐字节不变且不输出成功 IR。缺 manifest 不冒称 production error。

| 每个 runtime、每个 API 组 | 数量 |
|---|---:|
| before 完整状态观察（768 × 2） | 1536 |
| after 两提示完整状态观察（768 × 2 × 2） | 3072 |
| 正常完整状态匹配合计 | 4608 |
| 错误 IR observations | 2048 |
| 指定错误全部捕获 | 1248 |
| 比较器翻位控制全部捕获 | 50688 |
| 保存完整流 / 帧 | 52 / 6656 |

每个 runtime 两组总计 **3072 before + 6144 after = 9216** 正常完整状态匹配、4096 错误 observations、2496 指定错误、101376 比较器控制，104 流 / 13312 帧。9216 次比较中一半（4608）是 descriptor size 不足后的受检查失败退出，另一半（4608）是业务正常返回；不能称为 9216 次业务根成功返回。同一 768 输入在两 API、两个优化级和两构建重复，不计为新增独立样本。独立重读保存流不增加原生执行计数。主库与 no-Z3 的完整保存流审计均通过，另从每个保存帧实际 Exit.kind 统计了上述业务返回/失败退出数；24 个转换后 IR 和 24 个 parsed-before IR 在两构建间逐字节一致。结构审计还明确检查 guard 和 fallback 使用同一个 actual LOAD、STORE 前后顺序、原 switch/default、未知调用未复制、side-reentry 及重复 seed 未选边均保留。

## 冻结来源与限制

- producer/model: `8929beea4d9341baf8063dd2a35a5811e7e08cb992c8a9c733509c315fbe762e`
- actual API driver: `3c5de376e3d9ebb91ca494a0f89bd526468f2ec3f725de0df8b6d527b07ccab0`
- main backend: `e96ec64725e521e88e8104c4e77bc1103c39dc5593642afe4ce9d7ccce5971c8`
- no-Z3 backend: `bcf83d53ffe9f543b5d601a729b9fd16b534e10dc52c56da29b63b3db0ff4480`
- main shim: `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`
- no-Z3 shim: `061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`

这是 host DL64 LLVM 原生执行，不是新机器指令/IGNIL/Unicorn 全语料验证。只证明上述固定输入的等价性，不证明一般完整循环 CFF 已消除。root 的真实样本闭合验收另行报告。

## 复现

VM 根目录 `/home/cc/vmc-runs/guarded-transition-native-20261112`。`source/prepare.py` 记录实际 API 生成、ldd 和来源 pins；输入存 `before/`，产物分别 `main/`、`noz3/`。运行每个 API/优化等级：

```sh
python3 -B source/guarded_transition_oracle.py run before main/core-hint43 main/core-hint99 main/core-O0 --opt 0
python3 -B source/saved_audit.py /home/cc/vmc-runs/guarded-transition-native-20261112 main main-saved-audit.json
python3 -B source/shape_audit.py main main-shape-audit-final.json
```

换 `core`/`auto`、`O0`/`O2`、`main`/`noz3` 执行其余固定组；输出目录必须不存在，避免覆盖失败。运行库来自父任务 `guarded-loop-transitions-20261112/runtime-main-v2` 与 `runtime-noz3-v3`。本组不包含旧 API baseline；旧 API 不存在/单元旧失败证据由 root 单独保留。

完整归档 `guarded-transition-native-20261112.tar.gz`：**999 文件（998 payload + manifest）**，SHA-256 `27051380c9ecedcec06ab7e222ef636dad308f75c32bb1a03640a8690df6aa17`。VM 原展开与 tar、Mac tar 与完整展开均已独立精确校验 file-set/size/SHA。VM 位置 `/home/cc/vmc-runs/`；Mac 位置 `/Users/c/Desktop/deob/validation-artifacts/`，同名展开目录和两端 `*-archive-verification.json` 可直接检查。归档 README 为封包前报告，最终包 hash 以本文及外置核验收据为准。没有退役任何父任务运行库或原始数据。
