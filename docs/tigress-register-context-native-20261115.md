# 入口寄存器快照：独立 native 验证计划

本计划针对显式 ImageTableContext 的完整入口 word guard 消费，不改变默认流水线。
验证源码为 `scripts/validation/tigress_register_context_native_{source.py,probe.cpp}` 与
`tigress_register_context_native_driver.py`，不修改产品或现有 C++ 回归。以下协议是执行前
冻结的计划，实际 v1 失败及 v2 通过结果见末尾；这些结果不扩大计划的输入域。

## 协议与固定域

完整物理 arena 为 66,112 字节：Guest65,536、独立 RegBytes512、Exit56、guest return8。
有意 alias 的 trial 把**实际** State.regs_bytes 指到 `Guest+0x8fd0`；RSI offset48与
`Guest[0x9000]`物理重叠。整个 arena 所有字节一律比较，包括此时不活动的独立512字节区。
不把Guest和RegBytes拆成不相交模型，不声明noalias，不归一化任何字段。源码ABI仍是
void的四descriptor函数；guest return 是实际RAX字节，由wrapper序列化到最后8字节。
全部7个Exit字段、所有寄存器／flags原字节与Guest均在比较范围内。

Host descriptor含实际ASLR指针，不能直接纳入跨进程期望。wrapper在调用后检查每个
State/Memory/Context字段与原实际地址及整数完全相同；任何变化直接失败并保留原始输出，
不是忽略该字段或把指针归一化。只在这个固定host映射下执行；没有主机地址泄漏到输出。

三个新source fixture：

- `snapshot`：入口读RSI旧SSA，完整guard等于0x8000。body真实写Guest[0x9000]=0x2000，
  输出仍经旧SSA写0x8000；新proof应closed/changed，旧API拒绝是缺精度，不是旧执行错误。
- `fresh-load`：相同写后再LOAD RSI。物理alias trial得到0x2000，真实覆盖table row0，
  下一轮原LOAD产生UnknownTarget。proof必须拒绝且IR byte-exact。
- `unknown-call`：旧SSA输出后实际external call更改row1并增加call counter，下一lookup
  UnknownTarget。proof必须拒绝且IR byte-exact，不根据C实现给未知调用授予白名单。

每个source运行相同8个冻结input，共24个不同module-input、8个不同initial arena。
数据包括0、1、全1、64位符号位、最大正数和交替位。覆盖alias/disjoint、SP guard miss、
output guard miss及其bounds error、table guard miss、逻辑GuestSize8191的early error。
所有路径有至多3次dispatch的实际counter，原target LOAD和UnknownTarget/error效果保留。
它不是新的Tigress随机样本、不是单指令或69样本campaign，也不声称解决真实Flatten。

## 阶段与门槛

1. stdlib emit完整C/Python/LLVM/input/expected/三个fault文件并冻结PLAN。
2. BEFORE_ONLY producer只解析、verify、保存IR，不含Image API引用。旧库提供LLVM符号；
   不额外链接共享LLVM。每个O0/O2分别运行C24+LLVM24=48个完整frames，必须全部匹配。
3. 每个正常frame逐25个明确offset翻转字节，完整比较器必须拒绝，故每次before应有
   1,200 comparator controls。两个before result和各自saved-only审计通过后，才允许
   `--prepare-after` 调用产品API。C/Python/LLVM比较与坏帧检查均不放宽。
4. after producer仅动态引用installed `runClosedImageTableContext`，不能把core.o定义放入exe。
   明确pin producer/runtime/sole Z3与LD路径，保完整LD_DEBUG。逐3source各调用API一次；
   `snapshot`必须成功，另外两例必须闭合拒绝、无budget不足且IR逐字节不变。ordinary
   definitions、Module/Function/Arguments身份不变；所选原target LOAD仍存在。
5. 每个O0/O2执行新after24frames+600controls，所有66,112字节必须相同；拒绝fixture仍执行
   原IR并比较，不能排除。两O-level重复不增加24个distinct module-input域。
6. 三个实际坏IR：snapshot错用fresh、fresh错用snapshot、跳过alias STORE。
   每个O0/O2全部24个fault observations均执行并保存；规定8个路径必须在指定完整word
   （Guest[0x2000]或Guest[0x9000]）检出。guardmiss导致mutant未生效的其余结果同样保留、
   单独计观察，不能把24称为24个指定检测。两host-O级合计48observations/16specified。
7. saved-only审计检查单XZ CRC/EOF无尾数据、每frame域/输入/模型/完整SHA、全部帧与计数。
   未完整通过before不能生成after，任何失败写入新namespace且保留原始命令stdout/stderr。

所有target必须由root原512MiB启动、64MiB运行、180s外层guard串行调度；每个子命令也180s。
本脚本不会自动build producer，不改库、不补跑旧campaign；prepare/执行/saved三类明确分计。

## CLI（路径及SHA由root正式冻结后填写）

```sh
python3 scripts/validation/tigress_register_context_native_source.py --emit /ABS/raw-v1
python3 scripts/validation/tigress_register_context_native_driver.py --prepare-before \
  --raw /ABS/raw-v1 --raw-plan-sha256 RAW_SHA --out /ABS/before-v1 \
  --producer /ABS/before-only-producer --producer-sha256 PRODUCER_SHA \
  --runtime /ABS/libdragon.so --runtime-sha256 RUNTIME_SHA \
  --solver /ABS/libz3.so.4.13 --solver-sha256 SOLVER_SHA \
  --library-path /ABS/runtime-dir /ABS/z3-dir
python3 scripts/validation/tigress_register_context_native_driver.py --native --phase before \
  --before /ABS/before-v1 --host-opt O0 --cc /ABS/gcc-14 --llc /ABS/llc \
  --out /ABS/calibration-O0
python3 scripts/validation/tigress_register_context_native_driver.py --audit /ABS/calibration-O0 \
  --before /ABS/before-v1 --out /ABS/calibration-O0-audit
```

O2必须另label完整执行和审计。`--prepare-after`需`--before`、两个`--calibrations`及两个
`--calibration-audits`、新producer/runtime的完整pins和LD路径。after/fault的`--native`同样
需两个校准与审计；after另需`--after`。故意fault不计normal matches，root另发明确GO。

依赖是现有 `dispatch_corridor_native.py` 的file pins、whole-byte controls和单XZ EOF工具，
原旧validator文件不改。新command adapter额外保留每条命令失败时的原始stdout/stderr。
独立C只用字节pack与有界结构化loop；Python独立使用bytearray和算术，不以LLVM输出为expected。
所有输入／expected都先冻结；后续修订必须另版本，不能修改旧校准来过门槛。

## v1实际结果：校准通过，首次after精度门槛失败

ROCK owned root与Mac镜像均为 `tigress-register-context-native-20261115-v1`。
实际SDK为`435fcf7fac5563cb9ddc1a02437f7c22f022e6420acd8c1ddb6a1ab8dc622b4d`，
仅映射已pin的wheel `libz3.so.4.13`（SHA `4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a`）。
BEFORE_ONLY executable的defined/undefined symbols均无Image产品API；6份正常/坏IR只解析验证，API调用0。

O0/O2各完成48/48完整状态匹配（每级C24、LLVM24）及1,200 comparator controls；两个before
saved审计均完整通过。合计96匹配／2,400controls，只有24个不同module-input、8个不同initial
arena。三个坏IR在每级实际执行24份观测，规定8份均在指定完整word检出；合计48observations、
16specified/16detected，两级saved审计均通过，不把其余非触发路径冒称指定检测。

随后真实SDK上的第一次`prepare-after`调用即在`snapshot`阳性门槛停止：
`closed=false/changed=false/budget_exhausted=false`，message为
`SP observation did not survive normalization`，work1,235／guardedBytes16。
原／后IR逐字节相同，Module/Function/Arguments与ordinary definitions身份保持，原target LOAD在。
该v1只有1次产品API调用，另两个negative after API尚未到达，after-native0，绝不算新库native成功。
没有改fixture、模型、输入、比较或预算，也没有重试；所有失败命令stdout/stderr完整保留。

只读定位显示，原私有SP slot在入口保存initialSP，prologue减64，后面两个回边不再写SP；
actual guard49152使lookup/step SP同为49088，状态cell49104，满足原闭合契约。其唯一实际算术
use为`stateoff=frame+16`，mem2reg后InstCombine可将`(initialSP-64)+16`合成`initialSP-48`并删除
unused frame，外部WeakTrackingVH不是实际use。内部trial未dump，因此具体第一null block未冒称
实际观测。root另准备private normalization anchor修补及新的同API旧失败回归，v2后结果另记；
原fixture/model与v1树不改，也不重跑／重计已经通过的before和fault。

v1完整318 regular／54,347,340B双端逐文件set/size/SHA核验通过，含所有原始C/IR/输入、native
stdout、frames、plans、命令和首次失败。其315payload为54,227,521B；另外FILESET/closure/cert：

- SUMMARY SHA `db2df12a18972d85ded7d70684601ad1366c7cfb663a215360980c7b30ecd99f`
- FILESET SHA `70210ea9ad1e1e2d79fd4b6c06ae902beffc2ba45aa426aed46bec08f2e323a4`
- SOURCE-CLOSURE SHA `45302e5db5db24613c6bbb623f5c5bab8e1c7fc2194be436980785703fe4db6d`
- DUAL-HOST-CERTIFICATE SHA `aa068ab5fd9867992134c4abbcf9b288890595c7b6855f4b559e6f76832a8f53`

435f完整SDK由root另行备份；本证据树只绑定实际库SHA，不把54MB native树称完整OS／依赖环境。

## v2实际after：固定域通过，原v1失败保留

新namespace为`/home/ubuntu/vmc-runs/tigress-register-context-native-20261115-v2`，Mac同名镜像。
实际main SDK SHA为`a8690acf6be36264fd628aa1ebdb8075605f452327609aa91fae63e4f35b1de3`。
独立producer只动态引用installed API，无core.o interposition；sole Z3仍为上述wheel SONAME。
编译在CPU0–3完成，API/native阶段经root释放后的CPU4–7执行，512MiB/64MiB/180s不变。

一次prepare共3次真实API调用：`snapshot`得到closed/changed=true，preservedStores8、work2511，
完整guardedBytes16，发布IR没有临时`llvm.fake.use`。`fresh-load`以
`STORE range may overwrite a protected table byte`拒绝，`unknown-call`以
`call has no table-preservation proof`拒绝；两个拒绝都IR byte-exact，work均2195且无budget不足。
三个模块各自仍保留1条原actual target LOAD与原UnknownTarget/error语义，ordinary definitions
和Module/Function/Arguments身份不变。

新after在O0/O2各完成24/24完整66,112字节匹配及600controls，合计48新normal matches、
1,200controls。两个独立saved审计的域、整帧SHA、单XZ CRC/EOF与统计均通过。包括fresh/unknown
原样拒绝的源程序实际执行，不能把它们排除，也不能把其native匹配称为额外优化收益。
模型、C/LLVM/raw、24个module-input、8个initial arena、比较方法及预算逐字节不变。

原v1的96 before匹配／2,400controls与48fault observations／16指定检测只作既验引用；v2没有
重跑、重计。完整v1文件集及原stat/ctime/SHA再次核实未变，首次after失败没有被撤回。v2是
这组synthetic观察边界的main库验证，不宣称No-Z3 native、真实Flatten闭合或新指令/corpus覆盖。
root的旧／新SDK回归与full CTest属于另一个SDK验证域，不并入上述native数字。

v2结果和发布source绑定保存在`SUMMARY-v2.json`、`EXTERNAL-REFERENCES-v2.json`；新文件集、
closure与双端完整核验在同root的`FILESET-v2.json`、`SOURCE-CLOSURE-v2.json`及
`DUAL-HOST-CERTIFICATE-v2.json`。runtime本体继续由root独立封存，本native树不冒充完整OS环境。

v2完整129regular/20,033,532B（126payload/19,984,642B与三个闭包记录）已双端核验；
SUMMARY SHA `10c88b83fcd3f5849b42f561b916e20b12a26f38dd955149829067d9db6b2dbd`，
FILESET SHA `ac817fe4c168f63837288648a20826c26380dcbadb9fe7d5134841d2c3a98ccf`，
双端证书 SHA `fa24ff873ab6aa49d136e9f322e98f2540107e7a0a1b4e5243c54ea92f67d77e`。
