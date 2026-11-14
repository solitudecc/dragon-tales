# ROCK5B 验证环境移交（2026-11-14）

用户明确要求后续工作使用新 ROCK5B。已完成源码移交、依赖准备、首次main构建和
1233/1233完整CTest，0skip；同时完成更复杂Flatten样本的decoded-region IR基线。
旧VM清理已经完成，源码/输入/证据均保留；为重建同版本参考，只读导出了其clean ucdev
源码，没有复制旧主机二进制或继续在旧主机运行新target。新样本的去平坦化和完整native
校准尚未完成，不能由环境与IR检查通过推导出来。

## 已核验主机信息

读取新机自身的 device tree、`uname`、`/etc/os-release`、`lscpu`、`/proc/meminfo` 和 `getconf`，得到：

| 项目 | 实测值 |
| --- | --- |
| SSH | `ubuntu@192.168.10.241` |
| 设备型号 | `Radxa ROCK 5B` |
| 系统 | Ubuntu 22.04.5 LTS（Jammy） |
| 内核 | `5.10.0-1012-rockchip`，aarch64 |
| CPU | 8 个在线逻辑 CPU，Cortex-A55 / Cortex-A76，Little Endian |
| 可见内存 | `MemTotal: 32536684 kB`（约 31.03 GiB） |
| Swap | `SwapTotal: 0 kB` |
| libc | `glibc 2.35` |

这只是环境快照，不是性能、稳定性或运行正确性测试。源码初次移交完成时，home 所在文件系统可用 `446148096000` B，`/dev/shm` 可用 `16658780160` B；后续安装和构建会改变余量。

完整快照为 `bit_oracle-HOST-SNAPSHOT-v1.json`，SHA256：
`e8f7b9dc95f906fc15307a2caa4c7c5922e4760f37659ea392c0fe523f1097dc`。

## 源码与路径

新仓库原先不存在，创建于 `/home/ubuntu/dragon-tales-rock5b`，未覆盖任何已有仓库。origin 固定为 `https://github.com/solitudecc/dragon-tales.git`；本次未向 fork 或 upstream 推送，也未添加 upstream remote。没有子模块或 Git object alternates。

| 用途 | 路径 |
| --- | --- |
| 拥有的源码仓库 | `/home/ubuntu/dragon-tales-rock5b` |
| main 构建位置 | `/home/ubuntu/dragon-tales-rock5b/build` |
| no-Z3 构建位置 | `/home/ubuntu/dragon-tales-rock5b/build-noz3` |
| 新 campaign 输出 | `/home/ubuntu/vmc-runs/<独立标签>` |
| 本次移交证据 | `/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/source-transfer` |
| Mac 对应证据 | `/Users/c/Desktop/deob/validation-artifacts/rock5b-bootstrap-20261114-v1` |

构建路径是移交时的布局设计；移交操作没有运行 CMake、编译、CTest 或 target。SSH/SCP 使用已认证的 ControlMaster `/tmp/dragon-rock5b-20261114.sock`，没有复制凭据、SSH 私钥或全局 Git 配置。证据文件使用 `bit_oracle-` 前缀，与其他环境准备记录分开。

## 两次 Git 移交证明

首次 bundle 只包含 main 可达历史，精确检出 `1e45a5bde333f81ee13e652d75a2283e86ea403a`：403 个可达提交、1014 个 tracked 文件，共 `11349678` B。Mac / ROCK5B 的 path、Git mode、blob ID、文件类型、大小和 SHA256 全集合一致；bundle verify 与新仓库 `git fsck --full` 通过，工作区干净。

- `bit_oracle-main-1e45a5b.bundle`：`4348525` B；SHA256 `ab1f3e6e42ef49403f1202863f15ee2f0d86ac743d4fc1ca45011024870efced`。
- 原始两端 tracked manifest：SHA256 `e18992128094afc4c06cc3dfa273fb24e22c0475f9a178ad5e1b038bda3edc6f`。
- `bit_oracle-SOURCE-DUAL-VERIFIED.json`：SHA256 `e92374525d6bd023ef7c607af22629019d017aa8da19c74f4288bb959ce4b7cd`。

root 将 `a7e2cb5e91e44485725eb1eea8c584133abd6e42` 合入并推送 main 后，另行授权增量移交。先确认新机仍逐文件等于原 1e45 manifest 且工作区干净，再从增量 bundle fetch 并执行 `--ff-only`，没有 reset 或覆盖未知内容。新机 main 和 origin/main 均前进到 a7e2；404 个可达提交、1018 个 tracked 文件，共 `11408416` B，Mac / ROCK5B 新 manifest 完全一致。四项新增仅为 progressive harness 和文档，core 未变化。

- `bit_oracle-incremental-1e45a5b-a7e2cb5.bundle`：`27293` B；SHA256 `db69bb1a0565040e9ec43446ea9fdeab1c11eebd63648653c9e234161eeac283`；唯一先决提交为 1e45。
- a7e2 两端 tracked manifest：SHA256 `7bc4057cdbf49c83101767a716c7087e94698ed58c26efe4a2a7b884a2ac4305`。
- `bit_oracle-INCREMENTAL-DUAL-VERIFIED.json`：SHA256 `e4d4f89d2b948d2a072b43d10cf4e4d1067bfeb4d3b346e4f5a4b84dabb97dac`。

1e45 的原 manifest、bundle 和证书全部保留，没有用 a7e2 结果改写旧证明。

## 运行库与待补结果

本次没有迁移旧 VM 的 `libdragon.so`、shim 或其他已编译运行库。两机同为 aarch64 并不能证明 glibc、C++ 或 LLVM ABI 兼容；ROCK5B 需要从已绑定源码重新构建。旧主机的测试与 native 计数不转记为新机结果。

## 依赖与首轮构建

依赖已通过实际探针；原系统 GCC11/LLVM11、glibc 与原 apt 源文件保留。仅使用本任务
独立源/索引配置安装49个新包、更新15个必要依赖，0删除；没有系统upgrade/autoremove。
来源为 Ubuntu 官方源、apt.llvm.org 及 Ubuntu Toolchain Developers 的 Jammy PPA，
签名、包版本、安装模拟/实况及官方 wheel SHA 均保存。PPA共享运行库版本与编译器版本
分开记录：GCC/G++14.3，`libstdc++6`等共享runtime为16系列；不能将其统称GCC14库。

| 组件 | 实际选择 |
|---|---|
| LLVM/Clang/LLD | 21.1.8；LLVM_DIR=`/usr/lib/llvm-21/lib/cmake/llvm` |
| C/C++ | `/usr/bin/gcc-14`、`/usr/bin/g++-14`，14.3.0；项目需要`std::format` |
| Python基础venv | `/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv`，Python3.10 |
| Z3 | 官方`z3-solver==4.13.3.0` wheel的同一份headers/shared库与Python模块 |
| C++ Unicorn | 官方stock2.1.4 wheel完整headers/so，显式owned pkg-config前缀 |
| 编译缓存 | ccache，owned `/home/ubuntu/.cache/dragon-ccache` |

初次stock环境`ENV.sh`与`CMAKE-DEPENDENCY-ARGS.json`在bootstrap根并保留原样；
**当前使用`ENV.reference-v2.sh`和`CMAKE-DEPENDENCY-ARGS.reference-v2.json`**，
仅Python测试解释器改为下述正确参考，原Z3/Unicorn C++库和其他参数不变。不要用系统旧Z3
代替。ready探针SHA `2f95b432e2208536762dd6ac5bd5c8c3efdd730c62283178e762a66bcedcc9c1`，
安装审计SHA `ad4d72cded318d0cb4c895e8497611b7bc595b7502bf4dbf778d497b85b7fe64`。
六个wheel全部验证下载SHA。未手工改全局`ld.so.conf`或注册wheel前缀；apt正常postinst
可能刷新系统`ld.so.cache`，不声称系统缓存从未变化。两个前置审计脚本错误完整保留：
v1误要求wheel不存在的可选`z3_version.h`，v2漏定义审计变量；v3按实际`z3_api.h`、
`/usr/include/c++/14/format`及native Z3_get_version检查通过，没有伪造header或改依赖期待。

首轮main源码为a7e2，RelWithDebInfo/Ninja，显式GCC14、Z3 ON、Unicorn ON、Python/cffi，
link pool=1。清洁环境和全部命令记录在
`/home/ubuntu/vmc-runs/rock5b-first-build-main-20261114-v1/`。

- CMake configure **22.0423秒**；确认Unicorn FOUND=2.1.4、指定wheel Z3、UBSan启用。
- `cmake --build ... -j4`，进程树只允许运行CPU4–7；342/342步骤成功，**663.6665秒**。
- 两秒采样的最高温度55.461°C，最低MemAvailable28882341888字节（26.90GiB）。
  这是采样值，不是连续物理峰值；没有OOM或资源守卫触发。
- tracked源文件未改；full no-Z3构建尚未在ROCK执行，不能引用旧VM计数冒充新结果。

本轮实际库：

- `build/lib/libdragon.so`，218113944字节，SHA
  `28ddd0d889f8ae6e28ad8a41abfbd69a811600e90f40db2e3e1ac4a12b1e48e6`。
- `build/c_shim/libdragon_c.so`，5511104字节，SHA
  `9d95c6402f624f277900746e3493f2e65039a8e51b93561fde66454e84583efa`。

## 首次完整CTest与参考边界

新机首次fresh注册1233项，全部执行、0skip；193.8117秒，1231通过、2失败，原始
`CTEST-RESULT.json`、JUnit、完整日志及输入/输出均保留在首轮build证据根。不能把
历史cached 1163计数当成本次注册数，也不能把本次两失败隐去。

两失败为Python的`LLVM static Guest calls and boundaries`与`complete Guest CALL and RET`。
各自的x86_64/x86_32 `root_ret_max`、O0/O2各两trial只有SP不同：初始SP=0xc800，
机器字节`c2ffff`即`RET 65535`。Dragon给64位0x1c807、32位0x1c803；stock参考给
0xc807/0xc803，精确相差0x10000。RET imm16应无符号扩展；stock把0xffff当-1。
这与[Unicorn上游修复2bf1098](https://github.com/unicorn-engine/unicorn/commit/2bf1098e0defd95dd28553e78f853412e0507958)
的有符号到无符号读取修复相符。框架语义、检查和期待值没有修改。

旧VM的C++参考为发行包2.1.4-3，默认Python CTests则实际使用`/home/cc/ucdev/bin/python3`。
二者不能混称同一份stock库。已定位旧clean ucdev源码HEAD
`2a05c41831c678c22b2c6de471aae15b1cfc9ae3`，包含上述上游修复。

## 正确参考与完整复验

新机从上述原字节源码独立编译Python参考，owned根为
`/home/ubuntu/vmc-runs/rock5b-ucdev-reference-20261114-v1/`。源archive、1010个源文件
及原HEAD均核对，旧clean source不变。最初全架构构建和后续增量的两个170秒监控超时
完整保留；最终参考构建域明确为框架实际支持的x86/ARM/AArch64，不能称所有Unicorn ISA。
依赖构建的监控调整不改变任何solver、testcase或比较器预算；没有删除或跳过Dragon测试。
安装前一个辅助脚本误拒绝原有空`.gitkeep`目录的记录也保留，修正仅涉及安装路径处理。

- 正确参考解释器：`/home/ubuntu/vmc-runs/rock5b-ucdev-reference-20261114-v1/venv/bin/python`。
- 实际库：该venv的`lib/python3.10/site-packages/unicorn/lib/libunicorn.so.2`，
  SHA `404233d2b43651018a8c2e531eea7452ec4294acf8cea80f570d1635cb635b47`。
- API仍为2.1.4；archive构建的Python分发SCM标记是`0.1.dev50+g2a05c4183`，
  不能误标为PyPI stock release。来源HEAD与库SHA才是该参考身份。
- 新venv仅安装该UC binding，`.pth`复用原base的Z3/cffi；其路径、device/inode/SHA
  确认相同，没有复制第二份Z3。原stock基础venv与C++ UC前缀均保持。
- RET边界小probe：stock在32/64位合计6/10匹配、4个高位立即数失败；正确参考10/10。
  另有Rosetta x64五条独立参考5/5，不称32位新硬件执行，也不计入CFF完整状态campaign。
- `READY.json` SHA `13bcf93208151b824f57522df826cd49fb297ba8815d284be38fe19dded00351`。

root只重设CMake的`Python3_EXECUTABLE`，1.3171秒configure与0.0649秒无工作增量构建
均成功。**原五个库/可执行文件大小与SHA全部相同，tracked源完全未改**；原stock失败的
日志和结果保持。正确参考在实际CTest环境中也核验了加载路径。

第二次完整CTest位于
`/home/ubuntu/vmc-runs/rock5b-main-ucdev-reference-validation-20261114-v1/`：
**1233/1233通过、0失败、0skip、0disabled，229.3478秒**，仍CPU4–7、`ctest -j4`。
两次使用相同测试命令、守卫与检查；控制脚本派生只改变输出namespace，不改变比较规则。
原始CMakeCache/CTestTestfile/compile_commands在该目录另有`stock-initial-*`原副本。

当前环境文件SHA分别为：`ENV.reference-v2.sh`
`ab23c50c669ecb727affcd4ba245c2c2ea9bd0cd5e6706b9deeca5dd68ebafb1`；
`CMAKE-DEPENDENCY-ARGS.reference-v2.json`
`51b59035f4d2e57deac5e999974665cbbc43b25e1815ec3afe924c9bf50815d7`。

## 复杂样本的本机基线

原始输入只从精确归档成员选取：50个文件、4114804字节，含同一Tigress Flatten O2 ELF、
源和旧16份完整初始帧/参考帧；实际小包加manifest传输1206206字节。完整旧manifest仅作
历史provenance，没有替换`/home/cc`字符串后直接执行旧方案，也没有复制旧运行库。

可移植producer为`progressive_decoded_region_baseline.py`，source SHA
`f050a0daf96fb1d90fa3cf6daf758cd516a5a1442d29a13eabf66f55f80aa930`。
一次v3在反汇编前因严格Z3运行路径守卫拒绝，0analysis/pass，证据完整保留。错误在计划
将链接输入名`libz3.so`当实际映射名；该wheel两个regular文件同SHA但不同inode，实际
DSO的DT_NEEDED是`libz3.so.4.13`。没有放宽守卫；新v4只改计划这一精确路径，封存
SONAME与loader证据，其余输入、源、库、预算全部相同。

v4一次实际执行3.807秒，1次disassembly、2次region lift、1轮10pass；source Graph精确
不变，始末实际Dragon与Z3路径均吻合。按唯一正长度STT_FUNC extent及已解码leaders
私有重建区域，80表槽/75目标的实际LOAD驱动switch覆盖通过；没有把ELF表字节当Guest
不变事实、没有增加预算或执行三个checked C++组件。

**当前仍未去平坦化**：kernel IR从359BB/436slots变为360BB/577slots，中央switch仍在
208-block业务SCC中。前后IR分别与旧canonical/raw逐字节相等，是明确的兼容性而非新
语义验证。此基线新native/C/Unicorn reference调用数均0；上面完整CTest的运行另计，
旧C16/UC16/native参考也不因复制或复核而重计。

v4五个小root共30文件/2143298字节已双端逐项核验：新机own根为
`/home/ubuntu/vmc-runs/progressive-flatten-rock5b-preflight-20261114-v1/`，Mac对应
`/Users/c/Desktop/deob/validation-artifacts/progressive-flatten-rock5b-preflight-20261114-v1/`。
最终`V4-DUAL-VERIFIED.json` SHA
`697462829a4cbaba2ebe499f433c9c02bf2f7fa70070715ce66a58b0f87c0937`。
原v3零分析失败有独立封存，未被成功结果覆盖。实际循环witness及全部预算统计见
`scripts/validation/progressive_decoded_region_baseline.md`。No-Z3构建和新模块native校准
均尚未在ROCK执行。

## 基准产物备份

为防后续relink覆盖本轮唯一基准，另保存五个完整构建产物：上面的两个DSO以及
`dragon_tests`、`dragon_c_tests`和`disassembler`，总448418472字节。原文件、目录及
源码均未改/删；前后完整stat/ctime/SHA一致。这是指定构建产物备份，**不是完整OS或
全部依赖环境**。恢复仍需此前精确的源码/头文件与系统、Z3、Unicorn依赖记录，且只能
解到独立versioned namespace，不覆盖活动build。

- 两端根：新机`/home/ubuntu/vmc-runs/rock5b-main-baseline-products-20261114-v1/`；
  Mac`/Users/c/Desktop/deob/validation-artifacts/rock5b-main-baseline-products-20261114-v1/`。
- 包：`rock5b-main-baseline-five-products-20261114-v1.tar.gz`，167447987字节，
  SHA `fd9e548542557865e3fe8583a3c54fa1e2397a19994a3aac4a7ffd30cce23e79`。
- 精确8个regular成员：5个产物、BUILD-RESULT、RESTORE及manifest。两端所有成员集合、
  大小/SHA及单gzip CRC到EOF/无尾垃圾均通过，root另独立重哈希Mac压缩包一致。
- `DUAL-HOST-CERTIFICATE.json` SHA
  `82ec62cbf4b7a21742db47511be11b224fbb786bf1deab884832d1748cd0f75a`。

验证工具v1在Python3.10缺少`hashlib.file_digest`处停于前置检查，源/日志保留；v2仅改为
等价流式SHA兼容后完成两端全量验收，没有更改包、manifest或期望。没有新增target执行。

配置、构建、两次完整CTest、环境激活、原控制脚本与现有源/依赖/参考/样本证书引用另以
小证据完整镜像封存。两端根均为`rock5b-main-environment-evidence-20261114-v1`，分别在
新机`/home/ubuntu/vmc-runs/`与Mac`/Users/c/Desktop/deob/validation-artifacts/`下，
57个regular文件/3882644字节，完整文件集/大小/SHA相同；原40个结果/控制源的
stat/ctime/SHA保持。该操作没有再运行target，也没有删除或改写source/runtime。

- 小证据双端证书：`fa820d97f13c095eea5ff84822da6e4287cb660aaf0629b154b1a21b6f48982f`。
- FILESET：`c816bca57b496c2092bb7571de72c5a527a33f87b295b28c218ffd817736e4d3`。
- BUILD-RESULT：`d49983770ed9a23fd45a32f4e23e0598646cb88ea38feb556d36a43cb7ec83fd`。
- 首次stock CTEST-RESULT：`17a9b749daf760ead9e81b82f150d5e9e5a1f539240c984cf2a6560028b9e14e`。
- 正确参考CTEST-RESULT：`678153a7ba4df876c6e77799acb965e5610c444bc53aad0ca47a27b4c210847d`。

root独立核对上述关键结果SHA、1233项及0skip字段，并重哈希Mac五产品包。封存时新机
可用441605824512字节；后续Git文档提交receipt作为外部新记录，不改这些冻结证据。
