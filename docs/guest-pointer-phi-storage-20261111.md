# 指针 PHI 阶段的验证数据归档

本阶段开始时，VM 根分区仅剩约 83 MiB，不能安全完成下一次链接。
先归档两个已冻结的旧验证目录，再继续构建；未清理源码、输入样本或当前构建。

| 原目录（相对 `~/vmc-runs/constant-flow-20261104/`） | 文件数 | 原始字节 | 归档字节 | 归档 SHA-256 |
|---|---:|---:|---:|---|
| `fixed-s0/f-mba` | 800 | 612079251 | 26935940 | `7f76783c19b566d1dde5691df136dca3c6cd50ac060da894f125240ee8060999` |
| `fixed-s1/f-mba` | 800 | 612079250 | 26936584 | `5abf472f5f724dabe3e517ddf833dac28eb8772e67aa7303b61b4fe36f3fb8b6` |

归档名称依次为 `fixed-s0-f-mba.tar.xz`、`fixed-s1-f-mba.tar.xz`。
VM 保存于 `~/vmc-runs/storage-cleanup-20261111-pointer-phi/`；Mac 保存于
`/Users/c/Desktop/deob/validation-artifacts/storage-cleanup-20261111-pointer-phi/`。
每包包含完整 `f-mba/` 文件树及 `MANIFEST.json`。两端独立逐项核验精确文件集合、大小和 SHA-256，
并在删除展开文件前再次核验 VM 源目录。原目录保留 `ARCHIVED.txt` 恢复说明，日志和失败数据均在归档内。
可用空间由约 83 MiB 增至约 1.2 GiB。

首次普通 gzip 归档约 581 MiB，无法在仅剩 83 MiB 的 VM 留下第二份；没有据此删除源文件。
后改用 128 MiB 字典的 xz 捕获重复快照，完成双端验证后才退役展开文件。
此归档工作不新增任何语义验证通过数。

在对应 `fixed-s0` 或 `fixed-s1` 目录恢复：

```sh
tar -xJf /path/to/fixed-s0-f-mba.tar.xz --wildcards 'f-mba/*'
```

完整核验报告为 Mac 目录下 `local-verified.json`、`vm-verified.json`、
`local-s1-verified.json`、`vm-s1-verified.json`；VM 同目录保留两个源文件 manifest。
