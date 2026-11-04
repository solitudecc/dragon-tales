# 第 5 步：导入名称与间接分发验证

分支 `fix/llvm-import-dispatch`，基线 `dde7871`（第 4 步）。本步沿用批准的四指针
V2 入口和 Guest 地址注册表，没有重命名函数或改变 IGNIL CALL/RET、C/Python 接口。

## 实现

加载器将 `LoadedImage::importSlots()` 复制进 Graph 现有 pimpl；Graph 大小和公开方法
不变。移动时跟随 Graph；合并遇到同槽不同名称就永久丢弃该槽的标识，不因后续合并复活。

内部小型传播只接受同一个基本块内可证明的常量地址、精确宽度的导入槽 LOAD、COPY 及
支持的常量运算。寄存器别名写会失效；CALL、OPAQUE、局部 LABEL 清空证据；跨 CFG join
不继承证据。直接目标只追踪最多 8 个无 CALL/STORE/条件分支/未知指令的 JMP 跳板，
循环或超预算不命名。因此支持可证明的 PE IAT、ELF GOT/PLT、导入 thunk 以及槽位先
加载到寄存器的调用；不能证明时宁可缺少名称。

2026-11-04 修正（[外部调用审计](external-call-audit.md)）：只有机器 call 指令处的 IGNIL
CALL、机器 jump（非 call）指令处的 IGNIL JMP 才是转移；块的落空 JMP（`lea`、`nop` 或
`bl __stack_chk_fail` 之后）不是。跳板除临时值外只能写 PC（x86 RIP 相对寻址）和
AArch64 IP0/IP1（X16/X17）；`mov w4, #-1; b compress2@plt` 这类先做计算再尾跳的块不是跳板。
旧规则在真实 Lua/zlib/sqlite/C++ 上给出过错误或多余的名称。

名称写为模块的 `!dragon.imports` 诊断元数据，逐项为：

```
!{!"dragon_v2_<arch>_<entry>", i64 <site>, i64 <slot>, !"<loader import name>"}
```

它只表明目标值来自命名槽位，不证明运行时该槽仍绑定原函数，不提供外部语义，不添加
`readonly/readnone` 等效果属性，不生成 `@puts`/`@GetTickCount` 的宿主调用。所有间接
CALL/JMP 仍从 Guest 寄存器/内存求实际地址，通过同一注册表分发；已完成 CALL 缺模型时
保留一次性压栈/LR 效果。目标 0 仍是已知地址，缺实现报告 NeedTarget。

`check_one.py` 增加每个模块实际输出的 import-reference 数；函数验证仍全遍历、每组
128 个函数，没有用名称统计代替 IR verify 或执行对拍。

## 失败证据和当前数字

旧 `dde7871` 对新增 PE/ELF/A64 名称回归：4 测试中 3 失败，88 断言中 14 失败。
未命名/覆盖寄存器反例原先通过；正例缺少元数据和名称。例如原始失败：

```
TestLLVMImports.cpp: REQUIRE(metadata) / CHECK(metadata)
with expansion: nullptr
```

修复后扩展 i386 ELF、真实槽位/名称字段检查、循环跳板和 Graph 冲突合并：
6 测试、151 断言全部通过。没有通过声明一个有宿主 ABI 的外部函数来满足名称断言。

| 验证 | 结果 |
|---|---|
| 完整三架构执行 | 53 夹具 × 8 × O0/O2 = 848 match，0 different/error/skip |
| 正常返回/边界 | Return 704、NeedTarget 112、Unsupported 32，逐项比较退出记录 |
| 覆盖 | 61712 寄存器字段、55574528 内存字节；48 个未定义 AF 字段；0 Legacy 归一 |
| CTest（Z3） | 466/466，0 失败/预期失败 |
| CTest（无 Z3） | 317/317，0 失败/预期失败 |

新增 13 个执行夹具覆盖普通寄存器调用、x64 RIP+槽/i386 绝对槽、经内存跳板转移、
间接尾转移、槽值 0 和 BLR XZR。间接执行在第 4 步已具备，本步不把旧版也通过的用例
冒充新增语义修复：最初选测 7 夹具共 28 次，有 24 match、4 reference_error；4 次来自
手写 A64 测试编码错误，已改用项目汇编器产物并重跑 4/4 match，最终全矩阵 848/848。
原始错误输出为 `Unhandled CPU exception (UC_ERR_EXCEPTION)`，单独保留，不计通过。
新框架的错误检测能力另由第 4 步的 72 个旧版语义失败和 12 个内存负对照证明。

## 原始产物与范围

VM 私有副本 `~/dt-agent-all/validation-results/step5-*` 保存旧版 C++ 失败、构建/CTest、
Native IR/优化 IR/驱动/输入输出及 `step5-guests/results.json` 的源码和工具版本信息。
Ubuntu aarch64、LLVM 21.1.8、GCC 15.2、Unicorn dev 2a05c41（包 2.1.4）。

名称证据由加载器到 LLVM 的 C++ 集成回归验证；Native 三架构夹具验证相同机器形式的
执行，不把原始字节 Graph 谎称为 PE/ELF 全映像执行。最终第 6 步继续覆盖完整流水线。

本步没有运行两套全集、真实基本块全集和跳转表全集，最终第 6 步会对当前实现运行。
尚不识别依赖跨块推理、未知索引/动态重定位/复杂跳板才能确认的名称；这些仍有保守
运行时分发，不会因缺少名称而生成 UB。未提供系统外部函数实现、ABI 推断执行、OS、
PAC/CET、精确故障或自修改代码模型。未 lift 数量本步尚未重新统计，不冒用先前数据。
