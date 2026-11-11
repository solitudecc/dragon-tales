# Guest GEP 常量索引的宽度修补

基线为 `d1a8467`。本组件修正 `GuestMemorySSA.cpp` 中常量索引求值及单索引 GEP
fallback；不修改指令 lifter、Graph、调用语义、Guest ABI 或默认流水线。

## 实际错误

旧 fallback 将所有常量树按宿主 `uint64_t` 运算，再把结果零扩展为字节偏移。例如：

| 表达式及 base | 真正的 GEP 字节偏移 | 旧偏移 |
| --- | ---: | ---: |
| i8 `add 200,100`，base 0 | 44 | 300 |
| i8 `sub 1,200`，base 512 | 569 | 313 |
| i8 `mul 20,20`，base 512 | 400 | 912 |
| i8 `shl 1,7`，base 512 | 384 | 640 |
| 上一索引用于 i16 GEP，base 512 | 256 | 768 |

窄整数先按自身位宽作模运算；GEP 再将最终索引符号扩展或截断到 pointer-index 位宽，
最后乘以元素实际分配大小。先在真实地址和错误地址写不同值，再读计算地址，旧分析会
把错误地址的值作为确定结果转发。这些常见反例可让所有实际访问都落在 64 KiB 内，
不需要原生执行 poison 或越界移位来获得失败。

## 新证明边界

- 以 APInt 保留每个整数表达式的实际位宽，支持 Add/Sub/Mul、整数位操作、合法移位及
  ZExt/SExt/Trunc。最终 GEP 索引按 64 位符号扩展/截断，固定 type size 的乘法也按
  64 位模运算处理，避免宿主带符号溢出或丢失宽中间值。
- 有 poison 风险的操作标志、非法移位、显式 undef/poison、未知输入和不支持操作均
  保持未知；freeze 只能保留已经证明的确定常量，不能把 freeze(poison) 猜成普通运算值。
- 证明最多 128 位、64 层、256 节点。超出范围返回 Unknown，地址继续按精确动态 GEP
  身份追踪，不建立假常量。该局部求值器没有新增公共统计字段；Unknown 不等同于已经
  证明所有地址或完成去混淆。
- `AddressKey` 和字节事实按完整 64 位字节偏移描述。pointer size 或 pointer-index size
  不是 64 位时，不构造线性常量 key，仍可使用同一 GEP SSA 的精确身份。较窄索引只对
  指针低位回绕，不能用 full-width 偏移和 byte-offset 重建推断一般内存范围。
- 可伸缩类型的 `getKnownMinValue()` 只是最小大小，不是运行时分配大小。LLVM 自身
  不能给出常量偏移时，fallback 只接受固定 type size，不把 vscale-dependent 地址当常量。

本轮没有扩展任意 address-space cast、一般别名或整个指针表示模型。

## 回归证据

最终同源为 `tests/lifters/TestLLVMGuestAddressWidths.cpp` 的七个 case：模算术、typed GEP
符号扩展、宽中间值/转换、poison 标志/移位、预算、可伸缩类型、非原生 pointer/index
布局。DataLayout 布局和 poison/freeze 非确定性形状只做结构及 verifier 检查，不伪造
固定原生位值。

冻结旧 backend
`bf6322254b1239aeb65fb093fa41dcb82bc31bd087f33911f713f70d7ecd0003` 上实际为
**7 cases / 154 assertions，45 failures**。新完整 `GuestMemorySSA.cpp` 对象替换、其余
依赖仍链接该冻结库的快检为 **7 cases / 172 assertions 全通过**；所有模块 verifier
通过，没有跳过、字段排除或归一化。旧失败包括确定的错误常量、缺失的正确精度，以及
本应保留未知的证明边界，不应全部称为独立原生错误输出。

最终测试源 SHA-256：
`1e333ba8949210d3aafc43834d395931c11565efaef121d37f9551ec73f45529`。
本组独立生产源 SHA-256：
`f1caadfbcbf84c8107b84f4fd57950dd5dda56997ba70ee5ab800b5d951a1dcc`。
后者是单组件快检源，主代理合并其它同文件修补后应另记最终集成源及动态库哈希。

VM 证据根目录：`~/vmc-runs/guest-address-widths-20261111/`。
最终日志为 `old/pointer-final.log` 与 `quick/pointer-final.log`，对应源、编译日志和可执行文件
也保留。早期编译 API 修正、首次六例及加入 scalable 回归后的中间结果未覆盖，不能
代替最终同源七例。快检不代替主代理负责的集成构建、主/no-Z3 CTest、独立原生模型及
真实 ELF 重放。

完整证据包 `proof-evidence.tar.gz` 已在上述 VM 根目录和本地
`/Users/c/Desktop/deob/validation-artifacts/guest-address-widths-20261111/` 双端核验，
SHA-256 均为 `8adf8b43e5499230ae42a829778f51df231fe0ffaa930244f00fb5142727be90`。
