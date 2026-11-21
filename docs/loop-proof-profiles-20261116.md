# 显式循环证明资源档

`loop-image-context` 的 `proof_profile` 接受精确字符串 `default` 或 `expanded`。
省略该选项保持原来的默认行为；未知字符串、大小写变体、空串、NUL 和非字符串拒绝。
该选项选择有界证明档位，不改变守卫、实际边覆盖、内存保持、未知调用屏障或事务发布义务。

| 资源 | default / 原 API | expanded |
| --- | ---: | ---: |
| OriginalIR PHI 投影节点 | 256 | 4096 |
| PHI 投影递归深度 | 32 | 128 |
| 单次 PHI 投影 work | 65536 | 1048576 |
| 局部 Guest 范围节点 | 128 | 4096 |
| 局部 Guest 范围深度 | 32 | 128 |
| 内存定义图 points | 128 | 1024 |
| 请求总 work 默认值 | 1000000 | 1000000 |
| 请求总 work 硬上限 | 1000000 | 16000000 |

单次投影还受剩余总 work 限制；消耗计入同一个请求，不重置全局账本。扩大总上限必须同时
显式选 `expanded` 和提供更大的 `max_work`。不提供 `max_work` 时仍为 100 万。
原 block/instruction/candidate 上限不变；Opaque shape 的原有局部限制也不因该档位改变。

Expanded 还允许第二次原STORE查询进一步分析按字节截断的低位连续1 mask：
mask宽度为8/16/32且小于原整数位宽。默认档仍只细化稀疏mask；两档内部的小低位mask
（例如7）仍优先直接取其界，避免追入无关的旧内存。两档均先算Coarse，只有合法但不够
证明不交的区间才进入Strong；独立初始化锚始终禁用祖先假设与Strong。任何失败仍回滚
整个未闭合数字缓存，不把mask或候选范围本身当内存保持证书。

JSON 工作流配置示例（以下为完整配置中的 `loop` 对象）：

```json
{"loop":{"proof_profile":"expanded","max_work":16000000}}
```

`scripts/deobfuscate.py` 严格校验后，将此字段交给 Python
`PassPipeline.add_loop_image_context(..., proof_profile='expanded', max_work=16000000)`。
省略字段时，规范化的配置及 Python helper 提交给 native 的选项集合保持原样。
普通 `pipeline.add('loop-image-context', ...)` 也支持同一字符串选项。

C++ 增加独立的 `LoopImageProofProfile` 参数 overload；`ImageTableContextRequest`、Stats 和
所有旧签名、字段布局及默认值未改。旧两参数 `runLoopImageTableContext` 永远走旧限制；
传 `Default` 也回到旧函数，不通过 Expanded。`LLVMLifter` 提供对应的显式桥接方法，非
StaticAnalysis 模式拒绝且不改 IR。直接 Module API 的未知 enum 值同样拒绝且 work 为零。
内部 PHI helper 用独立的四参数 overload，原三参数 helper 仍拒绝超出原硬上限的 options。

较大资源档不是闭合证书。预算不足仍返回 Incomplete/budget_exhausted，未证明的 STORE、
不完整定义和未知调用仍拒绝，原公开 Module 保持不变。实验样本曾在总 work100万、扩大
PHI 局部限制后越过旧瓶颈，但随后因 STORE 未证明而拒绝；不能称其已经去平坦化。

回归位于现有已注册 TU，无新增 CMake 入口：

- `[phi-low-profile]`：400节点完整转发图、超过32层的真实表达式、旧接口/显式Default
  输出及计数相同、无效枚举和上限、tiny预算回滚、裸 LOAD 仍拒绝。
- `[loop-proof-profile]`：Pipeline 档位校验、默认/Expanded各自总预算上限、模式及缺失
  view 的状态区分。此 TU 只用旧公开 API，可用于旧 runtime 的选项拒绝对照。
- `[image-loop-proof-profile]`：直接 API/lifter/Pipeline的默认输出与work一致，Expanded
  可执行完整基础证明，非法档位/旧上限/超限/tiny预算都保持 IR 与对象身份。
- `tests/python/test_loop_image_pipeline.py`：原默认编码不变、显式档位编码和 native
  配置拒绝、Expanded请求不会把未闭合状态改成 Success。

建议集中验证命令：

```text
dragon_tests '[phi-low-profile],[loop-proof-profile],[image-loop-proof-profile]'
dragon_tests '[phi-low-bits],[phi-low-forward],[loop-pipeline-options],[image-loop-context]'
python3 scripts/deobfuscate.py --self-test
DRAGON_C_LIB=<fixed-shim> python3 tests/python/test_loop_image_pipeline.py
```

准备阶段已本地执行纯配置 self-test：24项配置拒绝、6项未闭合清理授权拒绝、3项
CLI输出控制，Dragon imports/Guest runs均为零；Python三文件语法和 `git diff --check`
通过。C++构建、集中聚焦、双配置完整测试及真实样本执行由主控统一安排，本文不将
尚未运行的检查或旧语料数字计作本次结果。新增 C++ overload 无法由旧 SDK 直接链接；
旧 Pipeline 不识别新档位属于选项能力缺失，不是已证明的旧执行语义错误。
