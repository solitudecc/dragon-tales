# 同一逐边证明内复用地址依赖检查

基线 `40f733e`。Guest checked-edge证明对翻译后的LOAD地址和已有STORE地址使用同一个
节点预算，但原来分别遍历依赖图。若两者引用同一条已执行的历史SSA图，重复遍历会先耗尽
256节点预算，尚未检查地址等价就拒绝本可证明的STORE→LOAD关系。

本次只把一个`proveStore`调用里的两份`IndexDependencies`合为一份。corridor、前驱和
IR在这两次检查间不变；fresh与historical集合仍分开。任一检查失败立即退出整个候选，
缓存不跨候选或调用存活。没有提高预算、跳过poison/flags检查或增加表内容假设。

## 已完成验证

- 最终3个用例可直接在旧运行库编译执行；主/无Z3各3 cases /46 assertions，
  2 cases /8 assertions失败。新两套各3/46全部通过。
- 主/无Z3完整CTest **1065/1065、930/930**。原有budget、fresh/history、loop检查未修改。
- 同一冻结真实B输入、同一单次GuestSSA配方，cff O0的转发0→14，combo O0为0→10。
  cff state43的副本中LOAD被实际STORE常量43替代，原LOAD及未知入边保留。
  所有真实table LOAD/switch仍保留，原Incomplete/budget事件仍报告。

## 验证边界与后续

这恢复的是状态内存传递，不是已经完成循环去平坦化。独立native与完整冻结B真实
语义重放、最终归档核验均已完成。实现提交 `02a71ef`。

代码实际在VM `40f733e` 加三份候选文件上构建；精确pin见
`~/vmc-runs/guest-index-proof-reuse-20261112/candidate-source-v1.json`。
历史快照归档的VM冗余副本已移出以释放3.3GiB；Mac完整原tar和逐流校验gzip均保留，
原校验日志/manifest仍在VM，恢复方式见`storage-cleanup-20261109/RESTORE-OFFLOADED-20261112.txt`。
这只改变证据存储，不增加任何语义验证结果。

## 最终独立与真实验证

两套运行库各1344次完整状态匹配，576次故障变体观察中320个指定错误全部检出，
14784个比较器控制通过；各40条压缩保存流/1920帧完整审计。三种新增正例实际发生
转发，四种负例保守保留。七份新IR在主/无Z3间逐字节相同。

九种旧history/fresh LOAD/freeze/loop夹具与历史已验证产物的before/after逐字节一致，
不重跑或重计旧6480次native。历史whole6门槛仍为false，single3/branch3仍为true。
第一次新native夹具的IR负哨兵字面量写错，before阶段即被独立模型抓住；v1全部保留。
v2仅修IR字面量为模型原定值，模型/输入/比较/控制不变，并重新跑同源old/new流程。
见[独立验证](guest-index-cache-independent-validation-20261112.md)。

冻结完整B配方新执行C16/LLVM16全状态匹配，144个指定寄存器/Guest/Exit错误全部检出。
原输入、脚本及精度门槛保持；后续12次GuestSSA/12次dispatch仍报告incomplete。
完整B额外恢复cff O0的9个实际常量表地址、combo O0的1个；这不等于表内容已证明。
见[真实验证](guest-index-proof-reuse-real-validation-20261112.md)。

所有归档由主agent另行核对精确文件集合及每个payload的SHA和长度，运行库gzip另核对
解压后原SHA。证据根为本地`/Users/c/Desktop/deob/validation-artifacts/`和VM`~/vmc-runs/`。

| 归档 | SHA256 |
|---|---|
| guest-index-proof-reuse-20261112/guest-index-proof-reuse-metadata-v1.tar.gz | d633df080ef3b3b1941f45bec998ecd041685b16d260cdb3b6d8dbc540ea5e86 |
| guest-index-cache-native-20261112.tar.gz（保留失败v1） | bd37719d0f2080a37ee46f493bda36435daad99cab48393e0596cc37255bc46a |
| guest-index-cache-native-v2-20261112.tar.gz | e144136fdffd9a02a1da17db0289a90dcf1028a60d06923d9678304e133a38b4 |
| guest-index-cache-real-20261112.tar.gz | 4b0510e3cc9c0af7afe3123ed283c8a1260cd2020ec6efa1bc22e4ac324e1028 |
| guest-index-proof-reuse-real-20261112/recipe-B.tar.gz | 8d26b6a6094fceae658ca5a5e81a727d45c9f5947e74e2caec7a7d7161f290ab |

未新增指令/Unicorn或全69输入语料库campaign。原业务switch、动态CFF、未知写入/调用
与回入路径仍保留；下一组件将用本次实际表读取后的运行时比较建立可验证快路，
不会把初始镜像字节当成永久内存事实。
