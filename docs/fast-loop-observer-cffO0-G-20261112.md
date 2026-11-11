# f-cff O0：G 最终 IR 的实际快循环观察

G 正式45步输出已经完成原C16/LLVM16与144控制，本观察在其后独立执行。
只观察 f-cff O0 的四个原输入；不是四个样本整体完成，也没有额外优化或新增 Guest LOAD。
被编译的 G 最终原始 IR SHA256：
`34859b006ec19bde9945493ccfd5307985d7861880845a98c44d27886f02d84d`。
后续若输出字节改变必须重新观察，不能把本结果移植到不同 IR。

## 最终结果

四个正常观察的完整输出均逐字节匹配 G 原生对照流：512字节寄存器、完整1MiB Guest、
七个 Exit 字段、返回/调用计数/预算等原格式无删减。每个输入都满足：
root_entry=1、entry_admission=1；entry_fallback、target_failure、business_failure、
全部50个完整中央分发 switch 的 visit 总数均为0。

四个输入均在已独立证明闭合的快 SCC 内重复执行同一个实际 guard，不是把不同 guard
各一次的计数相加。例 site41 `memory.valid13.state.context1099.transition` 在输入
0/1/2/3分别执行51/63/30/53次；每输入实际重复的guard站点数分别22/22/32/26。
因此这四个运行不仅入口检查成功，而且确实经过循环，未走中央分发或预测失败回退。
这是 guarded view 内单次调用的证据，普通 root/递归子函数不在计数作用域，不声称
整个程序/任意输入/未知改表域都没有 dispatcher。

## 工具与反向控制

使用已独立自检的 LLVM-only instrumenter v2，SHA256
`be962c5e31ddee2e00bbb6535a49abc1f84af4ba13c1fd4eeba2703ae413aa49`。
它只在原 terminator 前读取已有分支条件或实际 switch selector，写入独立 host counters，
不新增 Guest 读、不改CFG、不在插桩后运行优化。沿用原 driver.c，新增独立 C 析构函数
将 counters 写入另一个文件，原程序输出格式和比较器不变；使用原 llc-21 -O0。
所有输入、源码、工具、相关 LLVM 动态库和命令工具的前后 pins 保持。

最终另执行四个 double-count 变体：完整程序状态仍匹配，计数全部准确翻倍，
因 root_entry/admission 不再等于1而四个全部被拒绝。omit-central 控制只在生成阶段
删除首个中央计数点，要求 emitted IDs 精确等于全 IDs 减首个中央点，且
`omitted_sites` 精确为该点；此结构故障检出，不冒称在零中央访问域检测到动态差异。
已有工具自检另有12个实际中央非零的漏计故障检出。

第一版 runner 错把固定工具的“漏首个中央点”合同理解为“漏所有中央点”，在最后
结构断言失败；其四个正常+四个double完整状态流、异常和源全部保留。修正版仅修正
这一期望到冻结工具的精确合同，未改变入口1、同guard重复、fallback0、central0、
完整状态门槛。最终只计第二次运行的4正常+4控制；初次执行不增加独立输入/样本数。

首预注册 SHA256：`6c9eaba30f7b638930299288455b047f5094e2bbc22dd103ecfbce2c124b04bf`。
修正后预注册：`5481b8c465959b74808a80b2b07274a5a446a8a32ba78a41af544607b6fb0534`。
修正版根目录 `~/vmc-runs/fast-loop-observer-20261112/G-cff-O0-v2/`；
`run/SUMMARY.json` 保存全部site计数与门槛，`commands.json` 保存实际编译/执行命令。

完整独立包为 `fast-loop-observer-20261112/G-cff-O0-evidence.tar.gz`，Mac 位于
`/Users/c/Desktop/deob/validation-artifacts/` 同名目录。包含首失败和修正运行的完整文件、
两版冻结源、输入/预期/实际全流、PLAN、family来源、工具引用及逐payload manifest。
工具本身及96状态/36计数故障自检另包 `tool-v2.tar.gz`，SHA256
`e06915a04ba992cf91e7dee4b99b7f6d254be59aa5424ae56619186d109ca283`。
