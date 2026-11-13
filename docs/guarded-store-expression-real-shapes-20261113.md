# Guarded STORE-expression diagnosis (read-only, 2026-11-13)

This note inspects frozen B outputs only. No new producer, pass or native run is
included. The local snapshots are under /tmp/dragon-practical-dispatch-diagnosis.
SSA names below belong to the kernel function dragon_static_entry_x86_64_1010.

## Actual target/state shapes

Manual C-O2 raw:
- target: memory.valid109, raw.ll:4082–4085:
  %1530 = getelementptr i8, ptr %2, i64 %107
  %1531 = load i64, ptr %1530, align 1
  switch i64 %1531, label %jmp.unresolved [...]
- address: bb_4787, raw.ll:510–511:
  %106 = shl nuw nsw i64 %1501, 3
  %107 = or disjoint i64 %106, 8192
- decoding: memory.valid107, raw.ll:4012–4014:
  %1499 = add i32 %99, 1640531527
  %1500 = xor i32 %1499, 1779033703
  %1501 = zext i32 %1500 to i64
- raw state %99 is a PHI in bb_4765 (line473):
  [138502176,memory.valid91], [%ECX.0,memory.valid105],
  [%99,memory.valid109]. It is not a Guest LOAD.
- original unoptimized state is a real i32 LOAD: lines9347–9348,
  %5694 = getelementptr i8, ptr %2, i64 %186
  %5695 = load i32, ptr %5694, align 1.
  %186 is current guest RSP+68 (lines882–883); in raw, the state cell is
  %1181 = GEP Guest,%1169, %1169 = %RSP.0-84 (lines3117/3165).
- actual state writes in raw: initialization store i32 138502176 at3166;
  memory.valid105 stores i32 %ECX.0 to exactly %1181 at3944.
  This exact operand observation does not license a general alias assumption.

Flatten C-O2 raw:
- target: memory.valid57, raw.ll:2553–2556,
  %806 = getelementptr i8, ptr %2, i64 %82
  %807 = load i64, ptr %806, align 1
  switch i64 %807, label %jmp.unresolved [...]
- address: bb_4390 lines484–485:
  %81 = shl nuw nsw i64 %state.edge.live404, 3
  %82 = or disjoint i64 %81, 8192.
- state.edge.live404 (line460) selects [48,bb_4384.state.edge] or
  [%RCX.1,bb_4384]. RCX.1 (line425) has 74 incoming arms, including
  constants, finite expressions and self/history edges. The state is register
  SSA, not a Guest state LOAD/STORE. Register snapshot writes to ptr %4 on exits
  are not interchangeable with a Guest state write.

Both raw target forms meet indexOf's same-block simple-i64-LOAD/switch shape.
storeTableShape instead requires a direct simple i64 state LOAD multiplied by8;
it rejects both these raw DAGs. Merely accepting i32 state width is insufficient.
In addition, B has no dragon.guarded-entry metadata. A new independent
dragon.guard-predictions manifest must not be presented as checked-entry facts.

## Three concrete incoming contexts

1. Manual dynamic business choice, source case1 -> case2/case3:
   memory.valid145 computes %1984 = and i32 %1955,1 and
   %1985 = add i32 %1984,138502173 (raw lines5218 etc).
   Real edges: memory.valid145 -> bb_4752 -> memory.valid105 ->
   bb_4765 -> memory.valid107 -> bb_4787 -> memory.valid109.
   Selected ECX.0 incoming is %1985; actual STORE is i32 %ECX.0,%1181.
   Modular decode yields {3,2}; table byte suggestions map indices2/3
   to target4815/4920 (addresses0x2010/0x2018).
   A copied selected corridor may guard on the actual loaded target and exit
   directly to those original business blocks. It must execute the actual
   STORE, decode, LOAD, bounds paths, effects and flag computations first.

2. Manual constant continuation to inner-count checking:
   memory.valid127 -> bb_4752 -> memory.valid105 -> bb_4765 ->
   memory.valid107 -> bb_4787 -> memory.valid109.
   ECX.0's selected incoming is138502175 = enc(1); actual STORE still at3944.
   Decode gives1; prediction bytes at0x2008 suggest target4988.
   memory.valid125 is a useful precision negative: select enc(0)/enc(6).
   The decode>5 side exit avoids the actual target LOAD. An enumerator union
   {0,6} with no valid table6 hint may honestly refuse the whole candidate;
   that is a precision limitation, not justification to erase the side exit.

3. Flatten constant source-state continuation:
   bb_4483 -> bb_4384 -> bb_4384.bb_4390_crit_edge -> bb_4390 ->
   memory.valid57.
   RCX.1 selects77 from bb_4483; edge-live404 selects that arm. Index0x2268
   gives a target suggestion4462. Source case78 sets c1 then next=77;
   target bb_4462 is the following case77 context (sets c2 then next=76).
   No Guest state STORE exists here. The old selected-PHI enumerator can be
   considered; a LOAD-atom-only STORE predictor must not invent a state LOAD.

Table suggestions were read from frozen ELF file sections, not Guest memory
facts: manual ELF SHA f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe;
flatten ELF SHA bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b.
No read_known_memory premise follows. Every predicted target must still be an
existing switch case and match the actual single target observation at runtime.

## Minimal change and acceptance boundary

The minimal LOAD-atom scheme is worthwhile for the original i32 encoded shape
after mem2reg and before GVN, and for independent32/64-bit fixtures. It is not
a guaranteed new gain on these already-GVN B raw frames. On raw, use only
selected-path PHI evaluation already supported; do not invent a fresh SSA atom
as a proved state fact. The clone core already performs same-LOAD actual-target
guards, retains original switch fallback and repairs edge/live-out values.

Refusals/controls must cover explicit undef/poison, volatile/atomic LOADs,
out-of-width shift counts, call barriers inside copied paths, unsupported
live-outs, path/clone budgets, duplicate successor PHIs and fresh/history
distinction. Wrong STORE address or width, partial overlap, an overwritten
state, wrong prediction bytes, changed table entries, and arbitrary Guest
aliasing must never become truth. They may lose precision or take fallback.
A target LOAD followed by a STORE to its table entry must use the original
observation even on fallback; reloading is a detectable fault. Prefix calls and
side-error/bounds branches retain their complete effects and Exit snapshots.

Acceptance has three independent claims:
- Rewrite/copy counters: construction only, never de-flattening by themselves.
- Conditional business precision: identify an original business seed edge,
  prove the copied suffix retains its actual target LOAD, then show a guarded
  direct edge to the intended business case without returning to the central
  dispatcher PHI/switch on that fast arm. Preserve and test fallback explicitly.
- Execution equivalence: complete return/register/flag/Guest/Exit or standalone
  fixture arena comparison against an independent model, including miss/error
  cases and named fault controls. This does not imply all inputs were covered,
  the entire dispatcher disappeared, or manual analysis time was saved.
