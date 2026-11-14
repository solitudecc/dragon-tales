# VM 存储清理记录（2026-11-14）

三批均已完成，共删除552个具备精确恢复映射的旧叶子文件。最后一批完成后的空闲空间为
4123140096字节（3.840GiB），tmpfs空闲1486553088字节不变。该数是开始新样本测试前的
快照，后续测试输出及桌面服务可能改变全局空闲。没有删除源码、原始输入、当前运行库、
构建对象、目录或唯一归档，也没有暂停/清空桌面搜索服务。

每批先保留完整原件、验证两端备份，再检查使用者及受保护文件，最后仅删除获准的叶子文件。

## 第一批：七个旧 perf build-id ELF cache

已删除七个旧缓存 ELF：四个 backend、三个 C shim。源路径只在以下两个缓存根内，各条完整路径及精确恢复 member 记录在 `RESTORE.json` 和包内 `metadata/RESTORE-INDEX.json`：

- `/home/cc/.debug/home/cc/dragon-tales/build/lib/libdragon.so/<build-id>/elf`
- `/home/cc/.debug/home/cc/dragon-tales/build/c_shim/libdragon_c.so/<build-id>/elf`

这两个缓存根与原 dirty checkout `/home/cc/dragon-tales/` 是不同路径。

| 类型 | build-id | 原件字节数 |
|---|---|---:|
| backend | `462d740609bcbea1de946556e21ec08edf95f40f` | 144800632 |
| backend | `5f49f335b9990ded8fa7158f6bbbe232a8f134f1` | 144722064 |
| backend | `bd8403db26c88f0a81bd2559fd3eccc2045af4b3` | 144757288 |
| backend | `c92ffad2fbdbc5ce39dae873ee115afffb8dedeb` | 144817976 |
| C shim | `03dd38f08d8d259fdd7ccf4c06d0a1cc376bb51c` | 4227792 |
| C shim | `b4e6ab1a6b697cccbcd2450a7a76efa7d9c84f63` | 4227768 |
| C shim | `e93989bee965bd9b65006a56eb78b6b9867cb25b` | 4228776 |

七个均为普通文件、`nlink=1`；GNU ELF build-id 与各自目录名一致。它们的 SHA/build-id 均不同于当前正式 backend `2645b01d…` 和 `80dc8fe6…`。未来历史 perf/gdb 符号化如需要这些旧 build-id，应先精确恢复缓存叶子。

### 完整备份

先将七个原件复制到 Mac，逐件核对大小与 SHA；随后在 Mac 以单 worker、XZ preset3 生成最小完整包，并回传 VM。压缩包为 **127085564 字节**，包含 **10 个普通成员**：七个完整 ELF、Inventory、Restore index 和显式 Manifest。两端均流式遍历全部成员，核对文件集、每项大小与 SHA，并读取 XZ CRC 到 EOF；VM 未生成展开 tar，也未执行 ELF。

- Mac 包：`/Users/c/Desktop/deob/validation-artifacts/perf-buildid-cache-retirement-20261114-v1/dragon-perf-buildid-cache-seven-20261114-v1.tar.xz`
- VM 包：`/home/cc/vmc-runs/perf-buildid-cache-backup-20261114-v1/dragon-perf-buildid-cache-seven-20261114-v1.tar.xz`
- 包 SHA256：`51f857fd253b716a311a175a8a68d3bcb676c491df7e9a9f5ec94757f2445c15`
- Manifest SHA256：`efc3b5180b04089beba88e38e6e04167bef63ccb83eaef363c177dcf5c0666e8`
- Mac 完整验证收据 SHA256：`cb3f250d2dbb172f0a2ba35684f57a2d3651b9ba4f9789428bb08efa3122aa0f`
- VM 完整验证收据 SHA256：`7af3f42572faf28bf2d31f9e346674775cc1559fa85c9c6167671138f9b657b1`

七个 ELF 的逻辑总大小为 **591782296 字节**；旧档案的 metadata 索引未找到这七个 SHA 的可绑定 member，因此本批建立了新的完整备份。这个索引查询不等于证明所有历史压缩 payload 均不含相同字节。

### 使用者检查与保护范围

删除前逐文件复核 `lstat` 的 device、inode、`nlink`、ctime、mtime、大小及完整 SHA。原样复用已批准的分 UID 扫描函数，其 SHA256 为 `98539ee1e96fa0bdb6fe77d06a4ee9b43b0843b6242fc420a11391bc924ea5ea`；执行两次近时 `/proc` 检查，覆盖可读的 cmdline、environ、cwd、fd 及 maps，并按设备/inode 核查引用。两次扫描间隔约 1.623 ms，各有 86 个当前 UID 进程；候选引用为0，未知同 UID 不可读进程为0。

可见性限制保留在原始收据中。每次扫描有16条已批准安全/会话 daemon 的字段不可读记录，只允许固定身份的 systemd-user、sd-pam、ssh-agent，以及核验 root 父身份的 `sshd-session: cc@notty`；PID、UID、PPID、启动时间和命令行按既定规则核验。每次另有537条 foreign/root/system 字段拒读记录，这不是537个不同进程，也不表示全机所有进程均可读。任何新增未知同 UID gap 都会中止该批。

删除后以下保护集合与删除前一致：

- 未选中的 `.debug` 文件、目录及 symlink，共100项；包含系统 `.debug/usr`。
- 原 dirty checkout 完整身份集合37103项及被选源文件的内容哈希。
- 当前正式 runtime 集合43项、其他 RAM 对象身份88项、正式 source pins 14项。
- 完整备份包及其 metadata 的身份与 SHA。

仅七个 `elf` 叶子被 unlink。所有父目录、`.build-id` symlink、源文件、当前库、对象和用户最终 IR 均保留；没有运行新的分析 target 或测试。

### 空间核算与证书

空间按实际分配块核算，保留完整证明快照的占用也计入成本：

| 项目 | 字节 |
|---|---:|
| 七个源叶子释放的实际分配 | 591560704 |
| VM 完整备份包、metadata 与目录 | 127172608 |
| 退役证明目录 | 27619328 |
| 退役脚本与 Mac pre 输入 | 20480 |
| 本批确定净回收 | **436748288** |

`POST.json` 记录删除窗口内实际空闲空间增加 **577814528 字节**；该窗口增量与扣除全部先期备份、证明成本后的净回收是不同口径。其他批次随后带来的全局空闲变化不计入本批收益。

- VM 退役证据根：`/home/cc/vmc-runs/perf-buildid-cache-retirement-executed-20261114-v1/`
- Mac 完整证据副本：`/Users/c/Desktop/deob/validation-artifacts/perf-buildid-cache-retirement-20261114-v1/retirement-executed-v1/`
- `RETIREMENT.json` SHA256：`bfb6771b8f42cfbd43d13c3cd6068131143db898d6c21d4367365b67ebbd1f35`
- 七条 `RESTORE.json` SHA256：`853ef51d236810a668e24499258acc013c868c5bcc59ee20b7754509e5f28218`

### 精确恢复

1. 先验证任一完整包的上述 SHA256，再用 Manifest 验证完整成员集合、大小与 SHA。
2. 从 `RESTORE.json` 选择所需的单条 `source_path`／`archive_member` 映射；确认目标叶子缺失、原父目录仍在且未被 symlink 替换。
3. 只流式取出该条完整 member，在独立临时叶子核对原始大小、SHA 和 ELF build-id，再恢复至记录的原缓存路径，并按索引恢复 mode／mtime。
4. 重新记录恢复后的 device、inode、ctime 等身份。新 inode 不会等于已删除文件的旧身份，不能继续拿旧身份快照当作当前文件证明。

恢复目标仍是旧 perf cache；当前正式库继续使用自己的路径。恢复过程保留现有目录和 `.build-id` symlink，也不向 dirty checkout 写回旧 runtime。

## 第二批：526个旧生成产物

已按 proposal SHA256 `2a8d8f3c25d0a68180c7c258cc8556ab37aef4f955caf010698f11e905886b17`
清理526个具备逐成员双端完整归档绑定的旧生成文件：31个日志、478个历史中间IR、14个
native观测文件及3个命令日志。逻辑640887782字节，源叶子实际分配641921024字节。
删除窗口内空闲从780619776增至1422536704字节，增量641916928字节；这是窗口数，
不冒称已经扣除全部先期证据占用的净值，也不重计第一批收益。

两份现有完整归档保留在 VM `/home/cc/vmc-runs/` 及 Mac
`/Users/c/Desktop/deob/validation-artifacts/ordered-dispatch-components-20261114/`：

- `ordered-dispatch-historical-prototypes-and-campaigns-20261114-v1.tar.xz`，
  SHA256 `0141795851ba5915bf68267a6fe092128356c2041e5bda341c7422506e7d927a`。
- `ordered-dispatch-final-production-validation-and-source-20261114-v1.tar.xz`，
  SHA256 `ffefe433e229119e3e2fffbba91c739777763d22ee61d25ca594577162be7ae9`。

完整双端成员验证证书沿用 `b973676a094513f2ece56d2a5a7cabff9479ec8d55a4a50dc619511d50c3447c`，
清理前后又核验压缩包身份/SHA。两次近时批准的进程扫描无冲突或新增未知同UID可见性缺口；
每个叶子临删除前均重新检查完整身份、SHA和归档member绑定。目录、原manifest、源码、
原始输入、对象、当前2645/80dc及806d/689e运行库均保留。额外保留21个输入形态的派生IR，
以及用户最终IR `394faefad43ac21708d823cc09b0c8cec237048c45340ef4b0f8365b6c87872d`。

VM退役证据在 `/home/cc/vmc-runs/permanent-storage-retirement-20261114-v1/`；
Mac在 `/Users/c/Desktop/deob/validation-artifacts/permanent-storage-proposal-20261114-v1/retirement-executed-v1/`。
18个证据文件共5594576字节，已双端逐项核对文件集/大小/SHA：

- `SEALED526-DUAL-VERIFIED.json` SHA256
  `848e005f6f484911a17b03b023290d91c09423599c733ddd7e93243bb414f0b4`。
- `POST.json` SHA256 `a92131aba11a0c6a039539f0c1470e55b34c3d9d973955d6b4ad4edb89f332da`。
- `RESTORE.json` SHA256 `302cd6eddfdec2938dd00bf0605ecabd91219896d55e4086af4843a784aacd61`。

历史重放必须先按该恢复清单选取准确归档member并验证原大小/SHA；不恢复整个旧命名空间，
也不将已退休旧inode的快照当作现存文件身份。没有重新执行旧native/C/Unicorn测试，
没有暂停或清空桌面搜索服务。

## 第三批：19个旧磁盘库

原候选池20个旧库与既有47份归档的原始成员及gzip成员解码后均没有精确SHA匹配，
没有据此直接删除。新选其中19个磁盘库，排除device39的
`/tmp/final-cross-isolation-20261111/libdragon.so`。源逻辑3380937642字节、实际分配
3372724224字节，全部nlink=1；它们属于Nov09–12的历史运行环境，当前及后续测试使用
2645/80dc正式库或新正式构建。历史复现需先按精确清单恢复该版本。

19个原件先复制至Mac、逐件核对大小/SHA及VM完整原身份；17份不同原始字节以显式source
别名映射去重打包，没有剥离、重编译或其他字节变换。完整包668934668字节，21个普通成员
包括17份完整库、3份metadata及Manifest。两端全部成员集合/大小/SHA完全相同；另完整
读取XZ单流CRC到EOF，拒绝尾随字节和第二压缩流。根代理额外核对19个唯一恢复路径恰等于
获准原路径集合、所有原stat/SHA一致、17个payload无遗漏或孤立，补足通用成员验证的路径覆盖层。

- Mac包：`/Users/c/Desktop/deob/validation-artifacts/old-runtime-retirement-20261114-v1/dragon-old-runtime-nineteen-20261114-v1.tar.xz`。
- VM包：`/home/cc/vmc-runs/old-runtime-retirement-backup-20261114-v1/dragon-old-runtime-nineteen-20261114-v1.tar.xz`。
- 包SHA256：`c02fe9c8349300e3eb660ef1ecd9c3944b4bc1275e014f69e7c88870de944b9f`。
- Manifest：`4dc0356e761d08b2e2e49efafe7a9255822077f6ebc7137eef6f985a5908866e`。
- Mac完整验证收据：`00bd7020ebdfc50a8b2335259bd6b404b6a8005bbafcff6cad9d5b149de4e6c3`。
- VM完整验证收据：`c06ff26e7ca4b630c3faeb04d893b8a5d0167ad5cc0bc712275dbee0c7421c86`。
- 独立路径覆盖收据：`cae200ad6866fedd8bfcfa4fae587f8b3ee9f59c55b790cfb86751b92329622c`。
- 精确删除proposal：`e7ae315b01db683dcc54dad2b7ff06347f407f83a4f1f77a9a7ea30e70e303a1`。

两次近时批准扫描没有引用冲突或新增未知同UID缺口；临删除逐叶复核完整身份、SHA和
17个member映射。仅unlink上述19叶子；所有未选sibling、目录、源/输入/对象、dev39旧库、
2645/80dc/806d/689e库、用户最终394fae IR与完整备份包在前后保持。扫描可见性边界与第一批
相同，不能声称root/system的所有字段均可读。没有执行target、重跑旧语义测试或新增其他删除。

VM证据位于 `/home/cc/vmc-runs/old-runtime-retirement-executed-20261114-v1/`，包含
`POST.json`、`RESTORE.json`、两次进程扫描及保护快照；Mac副本归入
`/Users/c/Desktop/deob/validation-artifacts/old-runtime-retirement-20261114-v1/`。
19个证据文件共248787字节，双端完整文件集/大小/SHA一致；最终证书
`EXACT19-DUAL-VERIFIED.json` SHA256
`8af0917199e27bf8cdc43136c59ae8de4b202a1c5a94eed65705cdd40f23f55a`。
`POST.json` SHA256 `447bb88425af3f54ead281231f787194b0504a656e700f8d2a71e9c89ce85752`；
`RESTORE.json` SHA256 `bc1eaf91608f3033396cc2b4bed559929702d6831efdc29cd77c49e0ed9560c4`。
删除窗口空闲从750419968增至4123140096字节，窗口增量3372720128字节；
先期完整备份包及证据自身成本不应忽略，不能把该窗口增量当作整个备份清理过程的净收益。

恢复先验证完整包和manifest，按退役 `RESTORE.json` 取所需source→member一项，只解出
该完整库，核原始大小/SHA，再恢复原mode/mtime。19原路径共用17个payload的关系保留在
包内 `metadata/RESTORE-INDEX.json` 中。不得据旧inode身份跳过恢复后的新身份登记，
不得将旧库恢复到当前正式库或dirty checkout路径。
