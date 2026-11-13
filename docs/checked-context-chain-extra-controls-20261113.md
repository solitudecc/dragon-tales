# 顺序链额外 whole-chain 负控（2026-11-13）

本轮只验证检查器，不执行 product pass 或 target-native。冻结输入为
`/dev/shm/checked-context-chain-audit-20261113-root-v3/source` 和 `plans`；
链源码 SHA256 为 `6b91cfe8fab9ce205dfbca398d94a8be2eb725dafb5a31c8963911a377a672bb`，
alpha 为 `3ab0ebf665c912913e4353a69a26568f61c8fe7b4e7cd225be3872c70d5c8381`。
原始 root-v3 source、plan 和 frame 未改，两项计数与主 selftest 的7项分开。

| 额外 whole-chain 负控 | 已通过的前置绑定/证明 | 实际拒绝 |
|---|---|---|
| combo O0：私有 v_after 的 `!5 = !{i64 67280}` 改为67281 | shared工具4ba0独立产生完整6字段JSON；anchor及5个state raw/JSON绑定全部exact | V4：`typed MD payload or string changed` |
| combo O2：实际59个central成员仅声明58个，遗漏 `transition.fallback` | anchor/5个state绑定、完整LLVM导入桥、C59和Bit60 copy proof均通过 | Bit60：`central coverage omission or duplicate` |

第一项完整私有LLVM模块 SHA256为
`5aae4e6659d44c00d222cbe970169abdb233bea950709bbc7986980aff158601`，
独立完整JSON为
`99cc9e9e5c909d5a93dd9ef770bb6523f51167ad836414779e70ded1145209e8`。
JSON恰与原frame逐字节相同，因此此项实际检查了链中outside metadata证书，
没有靠结构JSON差异提前拒绝。两项CLI分别30.895秒和74.070秒，均exit=1并在
指定阶段拒绝。运行显式使用正式main runtime `02d730ce...`；source、原始输入、
tool和runtime全部前后pins/stats相同。开始512MiB、运行64MiB、每CLI180秒门槛
保持，未读取owned build/bin或build/lib。

`checked_context_chain_extra_controls_v1.py` 是实际执行的 `run_controls.py`
逐字节副本，SHA256为
`42f42a92c6b686ebd5033673dc6557b26511f4a208ff4940cc814feca68850f8`。
这是固定历史workspace的审计驱动，要求独立namespace预先存在，且拒绝已有
inputs；源码中的固定路径和输入断言是证据的一部分，不会覆写已有结果。

独立小包已置于原namespace之外：

- VM：`/dev/shm/checked-chain-independent-whole-controls-20261113-v1.tar.gz`
- Mac：`/Users/c/Desktop/deob/validation-artifacts/checked-chain-independent-whole-controls-20261113-v1/checked-chain-independent-whole-controls-20261113-v1.tar.gz`
- SHA256：`fafe81977d1f5cb2172414aadc9c2bf8338b9ac95a4093f4687e5824b4f76754`
- 49个payload文件，1933865字节。

包内保留完整私有LLVM/完整JSON、private plan、原plan副本、9份冻结source、
所有命令/输出/报告和前后pins；未变化的原始frame使用协调者既有双端归档
引用，外部receipt为
`d1c5f6856dbc9a5413eff21027d0a561852f43e2e0009922cecc16ca1298e0e5`。
原始独立namespace和旧review证据均已冻结，之后不再改动。

本结果不能解释为产品成功、原生语义验证或完整CFF消除。V4仍仅使用既有
冻结guard/path/PHI检查及outside证书，不声称证明V阶段所有无关指令的语义；
完整ordinary-edge/value copy证明从C59和Bit60阶段开始。
