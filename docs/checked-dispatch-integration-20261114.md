# Checked dispatch 三组件接入与验证

状态：正式 C++ 接入进行中，尚未完成本阶段完整回归或合并。接口与合同见
[组件说明](llvm-checked-dispatch-components.md)。这是显式 custom-pass 能力，不改变默认
六步优化，也不把验证脚本中的自动候选枚举注册为默认 pass。

## 已冻结的原型结果

本阶段使用本地 `ida-review-20261114/manual-O2.elf`，SHA256
`f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe`。
它是手写普通 CFF 样本，不是另一个 Tigress Flatten 样本，也不是组合保护的完成证据。
原始 LLVM SHA256
`0619b5e6d0d22b90a4ef4351929cfa9f1b61b3590c2bd81cfa32118dba7c2eed`。

显式 recipe 创建带真实 SP/表字节检查的独立分析 view，再证明表内容在相关执行路径上
保持、把最近状态 STORE 提升为 SSA、穿过 PHI 分配 decoder，最后保序处理原始回流边。
ordinary root 和 guard miss 的原执行继续保留。目标不是生成独立去混淆二进制。

保序组件处理8/8原始候选边，拒绝0、预算耗尽0；48副本块、853复制指令、165历史
snapshots、182修复 PHI、145797修复 cells、work159112。核心调用紧后及最终实际业务
SCC 的循环分发 switch 都为0；最终循环 table read 也为0。19个 checkpoint 全部验证
合法性并检查源/ordinary function 不变、原 Function/Argument 对象身份保持。
前9个阶段的18份 canonical/bound IR 与旧 recipe 逐字节一致，无归一化。

真实 nominal 对拍先执行本次 raw 的 before LLVM32次，再执行处理后 LLVM32次：同16组
输入，各自 O0/O2，64份结果的全部66136字节均与原16份 Unicorn snapshots 一致。
比较范围包括76个寄存器字段、65536字节 Guest 和8字节 Exit。192个比较器错误对照
全部检出。新 C/Unicorn 调用为0；旧结果只作冻结参考，不重复计算为新验证。
这组结果不覆盖 guard-miss、任意 GuestSize 或任意黑盒保护。

独立合成对拍覆盖历史回流、二值状态、STORE 后 alias/状态读取拒绝，以及错误路径：
64个 module-input、16份初始 arena；before C128+LLVM128、after LLVM128均通过，
另外64次实际错误变体全部检出，4992个快照比较器控制通过。6条完整保存流共448帧均
审计到 EOF。完整 arena 为66112字节，与真实样本的66136字节合同分开。

## 原始失败与限制

所有原型失败保持原始记录。v1 的历史 PHI 值证明、临时函数声明与元数据事务缺陷分别
记录；v1 原源码未成功编译，不能称其发生了 native wrong result。仅机械编译兼容的
v1 和保留旧值模型的合法事务 control 分别测到拒绝/错误证明，不能混为旧正式库缺陷。
重复边测试最初查询被 LLVM 清理的常量 PHI，诊断失败没有删除；最终测试保留常量返回
检查，并另加非恒定重复 successor slot 检查。宏测试配置14例/596断言通过；正式库不
导出强制提交失败接口，默认配置的13例另行验证。

本改动不涉及机器指令语义；没有重跑单指令 Unicorn、IGNIL 全语料或69输入历史 campaign。
已有 frozen reference 的桥接、原型 helper-object 和正式 shared-library 运行必须分别
统计。结构门槛不替代语义对拍，输入对拍也不等于任意程序的形式化证明。

## 正式库验证

接入复核发现 Image 原型的事务边界缺陷，随后单独修复，不能再把此次接入全部称为机械
include 迁移。旧核心 `066cb38f…` 的同一新增测试实跑1例/74断言，8失败：成功调用会
凭空留下空 `llvm.dbg.cu`，fastcc/musttail 输入还因回滚承载函数的默认 C calling
convention 在提交阶段被拒绝。这不是已证实的 target native 错误结果。

另外记录了两个不同崩溃：新增测试首版在旧实现意外发布后继续解引用已过期句柄，是测试
自身错误；修正为先严格 REQUIRE 拒绝再检查句柄后，独立 foreign BlockAddress section
仍在旧实现 `BlockAddress::get → Mapper::mapBlockAddress → CloneModule` 崩溃，已有单
section GDB 记录。两份原始失败都保留，不互相替代。

新核心 `b52d25ff…` 在 clone 前拒绝本地 metadata 身份与嵌套 foreign BlockAddress，
保留 named-metadata 对象/operands，临时承载继承原 calling convention 和 attributes，
在删除原 body 前完成全部验证。表内容证明和业务改写不变。独立宏测试7例/939断言
通过，含原4例、同 API 新2例和两阶段强制提交回滚1例。正式库不定义故障注入宏，默认
Image6、Decoder9、Corridor13，共28例须在新 shared library 上另跑。

正式源码已冻结并通过 GCC 原 RelWithDebInfo 参数的6次编译、2次串行链接；428个实际输入
pins 前后相同。保留原 Cell/C4/PassPipeline/predicate 对象和94个 LLVM static archives，
各库只追加3个新实现对象，不加载另一个 shared LLVM。构建 PLAN `267a3b66…`，BUILD
`b32a98ce…`。实际库分别为：

| backend | libdragon.so SHA256 |
|---|---|
| main | `2645b01d50cd9336a25ff4aadea87abe6ccc64ab615b2dca2063136bc7e610f8` |
| no-Z3 | `80dc8fe64694ecbd836cf28fdaf05ad82770423f9e347055e22a9ca469c36b6c` |

正式客户端每库28例/1701断言全部通过，SDK正链和运行也通过。客户端仅3个 test.o 或
smoke.o，没有实现对象；6个正常 API 都为 executable UND → 实际库 T，故障注入 hook
不导出，loader 记录锁定对应新库。旧实际库806d负链精确缺少这6个新 API；这是新增
接口不可用，不是旧 target runtime wrong。focus SUMMARY `37b015d8…`。

提交前检查另发现 Corridor 测试文件末尾4个空行。只删除这些空行，测试 token 与断言
完全相同；独立重新编译该 test.o，复用另2个 test.o，两库再次各28/1701通过。
最终测试源 SHA `d0e83b3c…`，source-clean SUMMARY `aa835a20…`；原 BUILD 的14份源码
快照保留原样，9个实现/头与2份 CMake 均仍逐字节匹配，不能把测试的空白差异隐去。

历史缓存完整发现列表保持命令、断言、超时与测试数量；只替换44个 C shim 环境路径，
显式 LD 指向新库。main1163/1163通过（88.42秒）；no-Z31028/1028通过（76.49秒）。
另外补跑不在缓存发现列表中的现有客户端：predicate main12/962、no-Z3 1/5；C4各16/3493；
Cell各14/261，全部通过且 loader 绑定新库。该补充计数与历史 CTest、28新用例分开。

正式 native/real producer 均仅旧 producer.o 加新库重链接，正常 API 为 UND → 实际库 T。
两个 backend 的合成4次 API 调用都与冻结原型的全部 parsed-before/output IR 字节、统计
和固定报告字段一致。重型原生回放、真实新库 recipe/边界组和最终双端档案尚待收口；
原型的成功不代替这些正式运行。
