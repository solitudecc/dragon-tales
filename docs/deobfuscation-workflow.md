# 显式候选的去混淆工作流

`scripts/deobfuscate.py` 把现有公开 API 串成一次有界流程：读取 ELF/PE，按显式 hints
反汇编，使用 static decoded region 提升，创建带运行时字节检查的独立 view，执行完整
loop table/cell 证明，**仅在 closed 后**对该 view 做函数内控制流清理。
不运行 Guest，也不把一份成功 IR 当作 native 等价验证或完整自动去混淆证书。

## 调用

```sh
DRAGON_C_LIB=/absolute/build/c_shim/libdragon_c.so \
LD_LIBRARY_PATH=/absolute/build/lib:/absolute/z3/lib \
python3 scripts/deobfuscate.py --config job.json --out new-results

# 纯 Python 配置检查：不导入 Dragon、不打开 image、不运行分析
python3 scripts/deobfuscate.py --config job.json --out new-config-check \
  --validate-config-only
python3 scripts/deobfuscate.py --self-test
```

`--out` 必须不存在；拒绝已有文件、目录或符号链接。配置错误、loader / lift / guard /
proof / cleanup 错误以及 Incomplete 均保留已经生成的文件和 `report.json`，不自动删除或
改预算重试。退出码 0 表示本次请求完成，2 表示配置或运行错误，3 表示
Unavailable / Incomplete。config-only 的 0 仅表示结构与值域有效，不保证文件存在、
能提升或能闭合。正常分析需要平台对应的已构建库；不能复用其它 glibc / 架构的库。
Loader 支持某种格式或架构，不保证它满足当前 loop helper 的 canonical-entry / 布局前提。

## 配置

下例仅演示格式。地址、目标与字节必须替换成用户自己的候选，脚本不按样本函数名或固定
地址匹配，也不从 image bytes 自动填入候选。相对 `image` 路径按 JSON 文件所在目录解析。
地址接受 JSON 无符号整数或十进制 / `0x` 字符串；大地址建议写成字符串，避免外部 JSON
工具的浮点精度损失。`image_sha256` 可选，用于核对实际读入的完整二进制字节。

```json
{
  "schema": "dragon-deobfuscate-v1",
  "image": "input.elf",
  "function_entry": "0x4000",
  "entries": ["0x4000"],
  "extra_blocks": ["0x4010", "0x4020"],
  "strategy": "linear-sweep",
  "guard": {
    "context_id": "user_context",
    "provenance": "explicit caller-supplied entry-byte candidates",
    "registers": [
      {"offset": 32, "bytes_hex": "00f0000000000000"}
    ],
    "memory": [
      {"offset": "0x800", "bytes_hex": "104000000000000020400000000000001040000000000000"}
    ]
  },
  "table": {
    "offset": "0x800",
    "targets": ["0x4010", "0x4020", "0x4010"]
  },
  "region": {"max_blocks": 4096, "max_dispatch_cases": 65536},
  "loop": {"max_blocks": 4096, "max_instructions": 65536, "max_work": 1000000}
}
```

`targets` 是有序 table slots，重复值代表不同 slot，原序和重复值完整保留。它不是可排序的
目标集合。这里只支持 1..512 个 u64 slot、八字节步长，表范围不得回绕。配置中的
`guard.memory` 必须显式提供需要检查的完整表字节；脚本不会假定表不可变，也不会用
`table.targets` 替你生成内存事实。与真实程序不一致的候选由运行时 guard / 原始回退处理，
不能因候选出现在配置里就视为证明成立。

`guard.context_id` 只允许 `[A-Za-z0-9_.-]{1,64}`，provenance 为 1..4096 字节的非空 UTF-8。
`guard.registers` 的 offset 是寄存器字节空间偏移；`bytes_hex` 只允许 1、2、4、8 个字节，
按 little-endian 转为 `FiniteEntryState` 的 `(offset, width_bits, value)`。
例如上面的八字节候选是 0xf000；不是宿主寄存器名。Guest memory 用完整候选字节串，
共最多 256 行、4096 字节。冲突的重叠候选、空或奇数长度 hex、溢出范围和未知配置键会
被拒绝。Guest 候选按 native exclusive-end 校验，`offset=UINT64_MAX,size=1` 也拒绝；
最大相邻合法 byte 起点为 `UINT64_MAX-1`。同一空间内一致的重叠行不会被悄悄删除。寄存器与 Guest 两个空间不因此获得
noalias 承诺，原库仍检查实际 SSA 快照、后续 LOAD 和所有继续写入。

`strategy` 可选 `linear-sweep` 或 `recursive-traversal`。入口、额外 entries 和
`extra_blocks` 都进入 `AnalysisHints.entry_points`，帮助解码显式种子；Graph 仍须有实际
function start，额外块须实际解码。随后 `lift_static_region(...,
rebuild_decoded_blocks=True)` 在私有副本重建这些机器块，保留未解析目标的 Transfer / error
出口，不改 Graph 来虚构边。脚本不自动推断符号范围、不枚举缺失表目标、不搜索更多 blocks。
用户必须明确提供所需覆盖；缺少目标或无法证明时如实失败或 Incomplete。

## 顺序与产物

提升固定使用 `STATIC_ANALYSIS` 和 `FUNCTION_START`，只提升所选 `function_entry`；
其它 hint entries 是解码种子，不自动变成已提升 callee。创建独立 guarded view 前不运行
默认 optimize、mem2reg 或其它清理。记录实际 manifest 的 `view_symbol`，不猜生成的符号名。
该 view 原始 IR 先运行一次 `loop-image-context`；每次只有一个 pass run，不自动多轮重试。

只有步骤状态 Success、`available=complete=closed=1`、pass 和调度预算标志均未耗尽，才对
同一 symbol 执行一次 `function-control-flow`。后者是现有六步 SCCP / DFAJumpThreading /
JumpThreading / SimplifyCFG / InstCombine / DCE，不重新运行 loop proof，也不扩大默认预算。
原始 lifter 保持独立，普通 guard-miss 回退由 view 保留。

输出包含 `00-raw.ll`、`01-guarded.ll`、`02-loop.ll`、成功运行后的 `03-clean.ll`、
原输入 / 规范化配置、guard manifest、两份完整 pipeline 报告和总 `report.json`。
失败时只保留已经生成的产物，后续阶段不执行。遇到 `PipelineError` 时也会尽可能保存
该阶段的 `*-error.ll`，只作诊断，不能把它当成功产物继续执行。报告的 `phase` 区分配置、loader、反汇编、
lift、guard、proof 和 cleanup，保留原始 counters / message / status；`ok`、`changed` 或
`converged` 不能单独作为闭合门。

最后的 `proof-closed-cleanup-completed` 表示此条件 view 的证明和所选清理完成，不宣称自动
恢复了整个程序或已经通过执行对拍。实际收益仍应分别检查 remaining dispatcher、业务
SCC、正常 / guard-miss 路径和完整状态参考。新的生产步骤参数格式与 C / Python 调用见
[PassPipeline](pass-pipeline.md)。
