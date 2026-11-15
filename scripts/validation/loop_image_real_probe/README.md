# 实际 Image 探针与声明导入检查器

本目录保存已执行的 C++ 探针、独立 checker controls 和控制器原字节；不是新增产品 pass。`EXECUTED-SOURCE.json` 将每个文件绑定到 ROCK 上实际执行根。

`source/probe.cpp` 用公开 `runLoopImageTableContext`，不包含 Image 实现。API 返回后先写独立 pending stats；模块验证、身份与普通函数全文、global/named metadata、原 fallback 等全部后置门通过后另写 eligible 记录。只有 B 的 closed/changed、无预算耗尽与完整门通过才能进入后续 native 计划；A 的原拒绝不影响 B eligibility。FileSet 只记录来源，不替代证明。

新函数集合门只在成功 closed/changed 后允许 `llvm.fshl.i32`：精确 `i32(i32,i32,i32)`、LLVM 21 canonical intrinsic 属性、无函数体、非空用户，且每个用户都是所选 view 内直接 `CallInst`。来源模块禁止任何新增函数，普通函数对象与 Arguments、普通函数完整文本、globals 和 named metadata 图仍严格比较。

14 个 LLVM-valid checker 控制全部通过：合法 fshl 导入阳性；旧 exact-set 门对同阳性拒绝；非成功闭合、普通 extern、新 definition、unused/wrong-width intrinsic、错误属性、非 view 用户、ordinary/global/named metadata 改坏和 source 模块新增均拒绝。控制器先完成这些控制，再执行两次 installed API；没有 Guest native。

历史失败原样保留：旧 af0a 探针对真实 B 的合法新增声明错误地触发精确函数集合门；v1 checker 编译因 LLVM 21 `Intrinsic::getAttributes` 需要第三个 FunctionType 参数而失败，0 controls/0 API。v2 唯一编译修正补该精确类型，未放宽接受规则。旧失败不是旧产品错误执行。

`controller.py` 是已执行的 artifact-specific 原文件，保留当时绝对输入、SDK 和输出路径及 180s/512MiB/64MiB 守门；它拒绝覆盖现有输出。不能在已有冻结根重放。需要另建 namespace、先固定计划和依赖，再使用新控制器调度；本目录不冒称新的 portable runner 已经测试。源码可独立编译 `identity_controls.cpp` 做检查器控制；完整 probe 的两个参数是已冻结 input-plan JSON 与必须不存在的输出目录。编译只链接实际 `libdragon`，不额外链接共享 LLVM。

主/No-Z3 同 physical producer 仅切显式 LD，B 均 work985771、closed/changed=true、无预算耗尽，完整 after SHA `714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`。同 physical producer 切旧 ef35 后 B 在原 1M 上限正常拒绝、IR 字节不变；CLI ok 不代表精度门通过，更不代表旧错误执行。完整语义 native 验证另行计划，不能把此 IR 证据当作 native pass。
