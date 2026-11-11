# guarded-target-sets 独立原生验证（2026-11-12）

本组验证实现 `a654e15` 的实际公开 `PassPipeline.add("guarded-target-sets")`。它将同一次真实目标 LOAD 的小目标集分支接到原 case，未命中仍走同一 SSA 值的完整原 switch/default。预测提示不构成表内容证明，也不保证去掉整个平坦化循环。

独立 namespace 为 `~/vmc-runs/guarded-target-set-native-20261112/`。新模型在独立文件扩展已经封存的业务循环模型，未修改旧 C/D 的源、输入、参考、比较范围或故障控制。预注册见同目录 source 中的 `guarded-target-set-native-plan-20261112.md`。

## 语义域与真实 API 门槛

五模式覆盖普通业务循环、LOAD 前改表、LOAD 后改表、目标块外未知调用、目标块内未知调用。x 为 0/1/2^63/UINT64_MAX，循环上界为 0..3，目标初值为43/44/99/127/128/255/777/99999，Guest size 为65536或不足511，共1280个固定输入。每轮保留实际状态槽 LOAD/STORE/LOAD、fresh LOAD/freeze、旧 history/target 快照及重新进入原循环。

六候选集合为43/44/99/127/128/255；另用合法但过时的集合{777}作正向等价控制。777是原 switch 的中心case，99999为原default。LOAD前修改表必须实际读到新值，LOAD后修改表不能重新读取替换已经加载的selector。未知调用写寄存器、Guest、状态槽和表，记录调用次数；块外调用保持原处，块内调用不复制。

每构建15项实际 API/结构检查通过：正常集/过时集分别五模式，以及缺manifest、坏JSON、非case提示、17候选超限、1block预算。两集合各四模式实际改写，块内CALL模式完全不变；五拒绝控制IR完全不变。主与noZ3的15输出及15 parsed-before共30份IR逐字节相同。

default与43、44共用good目标。normal小switch真实保留6cases，完整fallback保留7cases/default，同一 `%loaded.transition` 仅定义一次。good PHI严格为3条原边+3条fallback+2条fast边；过时{777}集合为6条good PHI边。Module/Function/Arguments/global身份及原 `dragon.guarded-entry` 与独立sentinel均保留。

## 原生与故障控制

每构建独立运行before、六候选输出、过时提示输出的O0/O2版本，比较完整返回值、调用次数和73728字节arena，其中包含512字节寄存器、全64KiB Guest、七个Exit字段、描述符及其余缓冲。无跳过、排除或归一化。

| 范围（每构建） | 数量 | main | noZ3 |
|---|---:|---|---|
| 正常全状态观察 | 7680 | 全部一致 | 全部一致 |
| before观察 | 2560 | 全部一致 | 全部一致 |
| 实际变换后的观察 | 4096 | 全部一致 | 全部一致 |
| 未复制CALL模式的after观察 | 1024 | 全部一致 | 全部一致 |
| 故障IR观察 | 2560 | 全部符合独立故障模型 | 全部符合独立故障模型 |
| 其中指定应错 | 944 | 全部捕获 | 全部捕获 |
| 比较器故障控制 | 84480 | 全部捕获 | 全部捕获 |

每构建3840正常业务返回、3840边界失败路径；不能把7680都称为完整根正常返回。同一1280输入在不同输出、优化级、构建间重复，不算新增独立样本。

五种故障依次为无条件信任43、43错误跳至另一case、LOAD后STORE再错误重读、丢LOAD前STORE、重复边PHI值加一。每个优化级分别产生104/37/114/124/93个应错输入，共472；其余故障观察对该输入恰好等价，明确不计为捕获。比较器对返回值、调用次数、寄存器、Guest及七个Exit字段逐个改坏一位，实际调用正常比较函数确认拒绝。

保存审计逐一读取每构建40条gzip流、10240帧，重新对照同一冻结独立模型、检查无短流/尾随字节、重放全部故障与比较器控制、核验源/运行库前后SHA及ldd。此审计不算新增原生执行。两构建均完整通过上述保存审计。

## 旧接口兼容与证据边界

原单候选core/auto × hint43/99 × 六fixture共24份输出，已用旧D运行库、原driver和相同输入路径真实生成并保存。新E main/noZ3分别生成24份输出，每份严格等于D旧输出，并等于原先封存的基础单候选输出；这是字节兼容，未重计旧native次数。

旧public builtin不存在的回归由主验证记录：旧6cases/20assert中原单候选兼容case通过，新builtin相关5cases失败。不能把不存在的API称作旧运行库语义错误。新的独立native夹具首次生成/实际API检查/两构建执行均未修改冻结输入追绿。后置辅助binding审计初版误把baseline报告的 `original_sha` 字段当作candidate报告的 `old_sha`，保留KeyError收据；按两种真实schema修正后72份legacy记录严格复核通过，不涉及模型、输入或比较更改。

这里只覆盖host-compatible LLVM原生语义，不新增机器指令、IGNIL、69输入语料或跨架构执行覆盖。真实四ELF、实际fast SCC及业务循环observer由独立真实样本组另报，不能用本组结构成功替代完整去平坦化。

## 复现及归档

在Ubuntu LLVM21 VM，driver只链接libdragon，不另链接LLVM共享库：

```sh
c++ -std=c++20 -O0 -I/home/cc/dragon-tales-overlap/include \
  -I/usr/lib/llvm-21/include source/guarded_target_set_native_driver.cpp \
  -L../guarded-target-sets-20261112/runtime-main-v1 -ldragon \
  -Wl,-rpath,/home/cc/vmc-runs/guarded-target-sets-20261112/runtime-main-v1 -o driver
python3 -B source/guarded_target_set_oracle.py emit before
python3 -B source/guarded_target_set_prepare.py "$PWD" main ../guarded-target-sets-20261112/runtime-main-v1
python3 -B source/guarded_target_set_oracle.py run before main/normal main/wrong main/native-O0 --opt 0
python3 -B source/guarded_target_set_oracle.py run before main/normal main/wrong main/native-O2 --opt 2
python3 -B source/guarded_target_set_saved_audit.py "$PWD" main main-saved-audit.json
```

源不覆写已有输出；复现应使用新目录或完整恢复相同绝对路径。noZ3更换label/runtime，输入和driver保持完全相同。旧接口兼容复现依赖基础单候选driver与六输入，最终包保存这些小依赖及原路径说明。

main backend：`c8bb7646ab64504576de536009d577b346e38beaffe16cc8e8067e29ff4ed4a1`；noZ3 backend：`be9589ab36770bb856bcf27254d2338aee41ead3549363560104edd7a8669726`。shim分别为`953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`和`061eb0a1b16a69acfc518255d48a752cd6836e8b09d84409f914d8412c9e91d0`。完整运行库由主验证namespace归档，本独立包保留精确pins与ldd。

最终独立包为两端 `guarded-target-set-native-20261112.tar.gz`，Mac根目录 `/Users/c/Desktop/deob/validation-artifacts/`、VM根目录 `/home/cc/vmc-runs/`。相邻 `*-vm-archive-verification.json` 与 `*-mac-archive-verification.json` 保存压缩包SHA、精确文件数及tar/展开双重核验。原始输出未删除。包内本说明在计算包SHA前保存，最终包SHA另列仓库版说明。

旧接口依赖保存在 `legacy-dependencies/`。为保持ModuleID逐字节比较，复现需将其恢复到 `/home/cc/vmc-runs/guarded-transition-native-20261112/` 对应子路径，再运行 `guarded_target_set_legacy_compat.py`；默认链接旧库的位置由候选 `LD_LIBRARY_PATH` 显式覆盖。VM实际checkout为5cac879加E候选补丁，不宣称VM HEAD等于提交a654e15。

最终独立包 SHA-256：`0bb9f7cfc6b435085b26a6cbb6525f509cbe663dfb320261237759c49ce86a40`，831文件（830 payload + MANIFEST）。Mac/VM两端tar成员与展开目录的完整文件集合、长度和每文件SHA均核验通过；不是抽查或仅比压缩包哈希。
