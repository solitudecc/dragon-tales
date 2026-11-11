# Remaining table-target proof diagnosis (baseline main 3bc5f2e)

本轮仅检查已归档 IR、源文件和 ELF；未修改代码/测试、未增加预算、未运行 VM 或新原生对拍。

结论：已知状态/索引恢复后，当前共同阻塞是 **Guest 表项内容没有证明来源**。
这些 LOAD 的保留是正确的保守结果。不能再把状态14/34当作未修复问题，也不能把
文件中的目标地址或 `.rodata` 标志直接视为调用者 Guest 内存的不可变事实。

## Evidence provenance

Source archive `/Users/c/Desktop/deob/validation-artifacts/guest-edge-worklist-real-20261111.tar.gz`,
SHA256 `8612e8ece6af95c519912beb84f45e1d9f5938773788fa7098f2eb9dfc83fef5`.
Only necessary IR, transformed sources and three small ELF files were extracted under this
report's directory. `manifest.json` binds every extracted source to its archive member/hash.
Diagnostics in `per-target-diagnosis.json`, `direct-store-inventory.json`,
`elf-table-evidence.json` and `table-layout-and-relocations.json` are static observations,
not claims of additional semantic executions.

IR line numbers below refer to:
`candidate-v1-enabled-full/oracle/<profile>/<level>/s0-product-O0/after-pipeline.ll`.

## Separate the compiler dispatcher, business switch and SBOX

| ELF | CFF source PC | CFF table [begin,end) | Business-switch PC / table [begin,end) | SBOX symbol |
|---|---|---|---|---|
| f-cff O0 | 0x102d8 | [69632,70040), 51×8 | 0x1067d / [70040,70088), 6×8 | [70096,70160) |
| f-cff O2 | 0x100b6 | [69696,70104), 51×8 | 0x10347 / [70104,70152), 6×8 | [70160,70224) |
| f-combo O0 | 0x10adc | [90120,90528), 51×8 | 0x13a87 / [90528,90576), 6×8 | [90608,90672) |

The table bases come from the actual target LOAD address expressions in the IR, not guessed
symbol names. The compiler tables have no symbols in these ELF symbol tables and no retained
relocation sections; these are already-linked ELF64 little-endian ET_EXEC files. File values
were read through the actual `.rodata` section address/file-offset mapping. SBOX has an actual
`.bss` object symbol and size64. `table-layout-and-relocations.json` records the headers.

O0 and O2 addresses must not be mixed: the previous CFF O2 index70096 belongs to its CFF table;
that ELF's SBOX begins at70160. It does not overlap the O0 SBOX at70096. Grouping all final
switches by their default Exit source PC yields f-cff O0 **21 CFF + 1 business**, f-cff O2
**17 CFF + 1 business**, and combo O0 **6 CFF + 1 business**. The existing `actual_loads`
metric filters by the CFF source PC; the old sixteen unique O0 indices did not include the
business switch. No correction of those sixteen-index results is needed.

## Known-index LOAD → target chains

f-cff O0 has20 known-index LOAD chains with16 unique indices; combo O0 has5 chains/5 indices.
All remain genuine `load i64` operations whose result is the target-switch selector.
The table lists one chain per unique index; JSON retains all25 chains including repeats.
The final column is a **file-byte observation**, not an admitted Guest content fact.

| ELF | State | Guest index | GEP → LOAD → switch lines | File target |
|---|---:|---:|---|---|
| f-cff O0 | 50 | 70032 | 9461 → 9462 → 9463 | 0x105c2 |
| f-cff O0 | 16 | 69760 | 9547 → 9548 → 9549 | 0x102da |
| f-cff O0 | 13 | 69736 | 9633 → 9634 → 9635 | 0x103c1 |
| f-cff O0 | 35 | 69912 | 9719 → 9720 → 9721 | 0x106c5 |
| f-cff O0 | 1 | 69640 | 9805 → 9806 → 9807 | 0x1045a |
| f-cff O0 | 5 | 69672 | 9977 → 9978 → 9979 | 0x104d6 |
| f-cff O0 | 12 | 69728 | 10063 → 10064 → 10065 | 0x104c3 |
| f-cff O0 | 2 | 69648 | 10149 → 10150 → 10151 | 0x1060b |
| f-cff O0 | 48 | 70016 | 10235 → 10236 → 10237 | 0x10339 |
| f-cff O0 | 39 | 69944 | 10321 → 10322 → 10323 | 0x1058f |
| f-cff O0 | 42 | 69968 | 10407 → 10408 → 10409 | 0x10514 |
| f-cff O0 | 43 | 69976 | 10541 → 10542 → 10543 | 0x10392 |
| f-cff O0 | 29 | 69864 | 10627 → 10628 → 10629 | 0x1037e |
| f-cff O0 | 37 | 69928 | 10713 → 10714 → 10715 | 0x1041f |
| f-cff O0 | 23 | 69816 | 10971 → 10972 → 10973 | 0x10574 |
| f-cff O0 | 9 | 69704 | 11143 → 11144 → 11145 | 0x104ad |
| f-combo O0 | 0 | 90120 | 21494 → 21495 → 21496 | 0x14296 |
| f-combo O0 | 32 | 90376 | 21616 → 21617 → 21618 | 0x13435 |
| f-combo O0 | 34 | 90392 | 21738 → 21739 → 21740 | 0x13ead |
| f-combo O0 | 14 | 90232 | 21860 → 21861 → 21862 | 0x154f0 |
| f-combo O0 | 22 | 90296 | 21982 → 21983 → 21984 | 0x14b01 |

For example, f-cff O0 L9461–9463 is `%2558 = gep i8 %2, 70032`, `%2559 = load i64 %2558`,
then `switch i64 %2559`. Here `%2` is loaded from caller `%memory` at L790. The bounds branch
at L9457 accepts the address when the prior size checks establish that the eight bytes fit.
No branch proves `%2559 == 67010`. Combo O0 L21860–21862 similarly reads index90232 and
switches on the result; L21738–21740 reads index90392. These are the now-recovered states14/34,
whose **address** proofs succeeded. Their **content** remains external.

## Which initialization STOREs exist?

`fixture-source/f-cff/transformed.c` L180–186 and combo L232–238 show that `megaInit` calls
only `SBOX_i$nit`. The whole LLVM module includes these functions, but that does not turn
caller memory at the kernel's entry into initialized immutable table contents:

- f-cff IR L565 defines megaInit; L641 calls SBOX init at0x10000. The SBOX function starts
  L11215; L11269–11270 write byte99 at70096. It contains64 fixed Guest writes70096..70159.
- combo IR L565 defines megaInit; L641 calls SBOX init at0x10470. It starts L25486;
  L25540–25541 write byte99 at90608. Its64 fixed writes cover90608..90671.
- An inventory of direct fixed Guest STOREs in the entire final f-cff module finds only
  those SBOX writes. Combo additionally writes globals90576/90592 (root L5411/L5562) and
  entropy90672 in another function. None initialize either dispatcher table range above.

This inventory is deliberately **not** a proof that no instruction can write a table byte.
Dynamic stack/output/heap addresses and unknown calls can alias Guest memory. In particular,
the f-cff state50 STORE at L9448 targets `%2552`, whose index `%2428` at L9191 is
`RSP.0 - 136`; it is not the compiler table index70032. On the entry path RSP comes from
caller registers, so no generic disjointness from that table follows. Future optimizations
must handle such possible writes, not infer immutability from the absence of fixed stores.

There is another real initialized array in the root: `ops[4]`, with assignments at f-cff C
L253/L371/L383/L401 and combo L343/L363/L408/L424. It is a **stack function-pointer array**,
not the compiler's flattened dispatcher table. Its STOREs cannot initialize `.rodata`
addresses69632/90120. Source `switch` lowering creates the latter tables at link time.

## Concrete counterexample to replacing the remaining LOAD with file bytes

At f-cff's state50 read, retain a valid descriptor with size at least70040 and the existing
bounds guard. Only change the eight bytes at Guest index70032:

1. Value67010 (file value0x105c2) selects its existing legal switch case.
2. Value66208 selects another existing legal case, `bb_66208`.
3. Value0 is absent from the switch and follows `jmp.unresolved` at L3285.

For the third case, L3300–3312 produce **Transfer/Branch**, sourcePC66264, nextPC equal to
the actual loaded value0, `next_pc_valid=1`, returnPC0/detail0. This is not a NeedTarget exit:
`Execution.h` L12–18 defines reason2=Transfer and kind1=Branch. The source root's known state50
on normal entry makes this an especially direct example: its entry state slot can be placed
away from the table, while the caller supplies either table cell. Nothing in the current ABI
requires the original file value.

Each JSON chain records an alternative legal case and value0/default as the same program-point
counterexample construction. For combo's table index90232 the file value is87280, while67440
is another legal case; value0 follows the default. Combo L6593–6605 records Transfer/Branch,
sourcePC68316 and the actual loaded nextPC. These are static IR counterexamples, not new
executed corpus comparisons or a claim that every copied path was separately executed.

The public memory descriptor (`include/dragon/lifters/llvm/Execution.h` L29–32) provides only
`data` and `size`. It contains no image-identity, table-byte or immutable-range promise.
The real oracle explicitly records `read_known_memory=False` at
`scripts/validation/guest_memory_checked_edges_real.py` L121. File contents and OS image
section flags therefore cannot be silently promoted to current Guest facts.

## Distinguish proof boundary from algorithm gaps

1. **Current known-index targets:** normal Unknown on the table LOAD is correct. Removing the
   switch unconditionally requires additional proved in-IR memory effects or a new explicit
   entry contract; increasing search depth or revisiting the queue cannot invent its value.
2. **Possible generic improvement:** a consumer may accept proved table contents from current
   dominating STOREs, byte reconstruction and joins, with correct clobber/call invalidation.
   This is useful for other same-function initialized-table patterns, but no such dispatcher
   initializer exists in these two roots, so this alone has no demonstrated real gain here.
3. **Smallest honest next component for these roots:** table-source diagnostics plus an explicit
   table-fact/entry-precondition interface, or guarded specialization that checks actual loaded
   bytes and retains the original fallback. The default must remain Unknown. An image reader
   can propose constants, but cannot establish immutability. A trusted immutable-region contract
   must state its duration, enforce or assume caller obligations explicitly, and account for all
   writes/unknown calls; an initial snapshot alone is insufficient after clobbers. These are
   design choices, not already-authorized assumptions about all Guest inputs.
4. The separate pure-GEP limitation in deflattening is an algorithmic extension opportunity,
   but allowing GEP instructions does not solve the missing content proof on these samples.
   The separately identified budget-report propagation bug changes diagnostic completeness,
   not this semantic boundary.

## Do not overgeneralize the targeted recipe

The actual frozen recipe is `mem2reg, instcombine, gvn, guest-memory,
 dispatch-specialize, control-flow, dce` plus a read-only checkpoint; see
`scripts/validation/bounded_memory_pipeline_real.py` L15–17. Although the intercepted legacy
request lists MBA, predicates and dispatch-deflatten at L18–20, **those passes are not in the
actual targeted run**. Thus `switches_rewritten=0` is the observed dispatch-specialize counter,
not a measured failure of all possible complete optimization pipelines. No new full pipeline
run was performed in this diagnostic stage. Other passes may simplify further state/index
expressions; they still cannot safely replace an unconstrained table LOAD with file bytes.
