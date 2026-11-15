# Accepted-view cleanup 输入与只读库存

本目录同时容纳独立 emitter / toy 和本说明涉及的 inventory / 准备脚本。inventory_v1/v2/v3 保留实际已执行源码与控制器原字节；原 0ffa emitter 未改动。prepare_input_seal.py 和 prepare_actual_plan.py 为实际执行的准备脚本，不运行 cleanup pass 或 Guest。文件来源与完整 SHA 在 inventory-source-provenance.json。

首轮 v1 使用 actual Guest base SSA、simple i64 LOAD、direct i8 GEP 和纯 binary/cast 常量地址规则，要求在 8192..8831 范围精确识别 73 个目标 read。编译成功，严格 count 门拒绝；该轮没有持久化实际计数，不能借后轮输出冒领。v2 只补计数日志、保留原 73 门与所有分类条件，实际记录 unused69 + literal-equality ordinary-fallback guard80 =149，constant_work702，再次拒绝。此规则未无推断覆盖其余四个目标；149 不是已证明的整个表 read 集。两次失败的源、输入 pin、stdout/stderr 和结果全部保留。

v3 使用单独 schema readonly-all-original-simple-i64-load-inventory-v1：完整 LLVM 枚举所选 view 的全部原 simple i64 LOAD，不按块名、地址或目标数量筛行，不做数值推断。实际为 459 条 integer LOAD、419 条 i64 / simple i64、零 non-simple i64；419 是完整结构超集，不是 419 个 TLOAD，更不替代原 core 的 table/cell 闭合证明。每行含零基 block/instruction ordinal、load 与 terminator 的原 render 及 SHA。保持 emitter 原有 512 库存上限，模块只读前后完全一致。

ACTUAL-B-PLAN 沿 unchanged emitter 的 lookups 容器传递这 419 条结构记录；报告字段 original_lookup_cutpoints 应按此库存 scope 解读，不能当作精确 TLOAD 删除计数。输入 seal 使用 exact-fileset-v1，source714701… 与 closed REPORTaebcd589… / STATUS.no_more_writes=true 逐内容绑定；FileSet 是来源记录，不替代证明。实际 source、producer、emitter0ffa、SDK d446、solver 和每个 ordinal/render 摘要都显式 pin。原 73 门没有放宽或改写，而是保留失败后启用范围明确的新只读库存。

这些脚本保留实验室当时的绝对路径和新文件拒绝覆盖行为，不是已经验证的 portable runner。不要在现有冻结根重放；重放需新 namespace 和新计划。原输入与三个版本的完整输出在独立双端证据包，actual cleanup 的六 pass 输出由另一个 owner 单独保存，本批不包含也不改写它。新封存操作不编译、不运行 pass/Guest，也不重新计数已有 native。

seal_evidence_v1.py 首次仅元数据收口读取失败结果时假设 RESULT.inventory 存在，触发 KeyError；原 per-command exit 已完整保存。seal_evidence_v2.py 只改读该既有 execution receipt，在核对原副本后完成收口；保留两份实际脚本和首次元数据失败说明，没有新目标调用、没有修改原输入/输出。
