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
语义重放正在完成，最终记录另行提交；合并前完成全部验证和证据核验。

代码实际在VM `40f733e` 加三份候选文件上构建；精确pin见
`~/vmc-runs/guest-index-proof-reuse-20261112/candidate-source-v1.json`。
历史快照归档的VM冗余副本已移出以释放3.3GiB；Mac完整原tar和逐流校验gzip均保留，
原校验日志/manifest仍在VM，恢复方式见`storage-cleanup-20261109/RESTORE-OFFLOADED-20261112.txt`。
这只改变证据存储，不增加任何语义验证结果。
