# Direct-exit entry-prefix repair: independent real validation

Baseline `756d64b`. The new direct-exit SSA repair passes the frozen four-ELF
oracle under both the original budget and a separately declared larger budget.
This extends the isolated entry prefix; it does not establish complete
loop deobfuscation or propagate facts across unknown calls.

## Frozen recipes and input domain

`entry_prefix_continuation_real.py` is an outer adapter around the unchanged
`guarded_entry_prefix_real.py` (SHA-256
`54db29bd0a691fbf00f539374e7201d2e23935ffe41d94f8c4d0ffa820fbbf7c`), which uses
unchanged `guarded_entry_real.py` (`812c7862616804db32a40ace8984da38634362017a64f172180d752443239b0e`).
The only recipe selection is the explicit entry-prefix instruction/block cap:

| Recipe | Prefix blocks/instructions | Purpose |
| --- | --- | --- |
| A | 32 / 2048 | Original-budget compatibility and combo O2 direct-exit repair |
| B | 128 / 16384 | Separately declared large-function profile: 4x blocks, 8x instructions |

Each module runs one explicit prefix pass with iterations1/pass-runs1, followed
by the same three rounds of eight legacy steps: mem2reg, instcombine, GVN,
guest-memory with edge_worklist1/run_mem2reg0, dispatch-specialize with
allow_plain_stores1, control-flow, DCE and the read-only checkpoint. All eight
modules actually recorded25 runs each. Legacy max-pass-runs32, the dispatch
condition cap256 and all other legacy options remain unchanged.

The SP-only checked entry facts, four ELF files,16 input states, C reference,
LLVM oracle, full-state comparator and144 semantic fault controls are unchanged.
Table facts still require runtime checks; no image immutability, no-alias,
callee-preservation or whole-loop assumption was introduced. Source modules
remain independent of guarded views; only the wrapper's original single direct
kernel call is rebound by the frozen adapter.

`PREDECLARED.json` SHA-256:
`9cb3e8ace498047971e00469e4b1765593d4cbb54395e8c3ac539f5b759d4a0f`.
It was frozen before either run, including separate A/B precision gates,
source hashes and unchanged native requirements. Candidate main runtime:

- `libdragon.so`: `34d1dd8d937590e9b6b66846fc8e51806254b507a309e68e6cd341a8e3f0f31f`
- `libdragon_c.so`: `953af6b077f946f1304a5cd90f49a957691b74a94729f38540516184218c1c84`

## Results

| Module | A prefix result | B prefix result |
| --- | --- | --- |
| f-cff O0 | complete,21 blocks/748 instructions | complete,21/748 |
| f-cff O2 | complete,22/961 | complete,22/961 |
| f-combo O0 | partial,32/2007, budget1 | complete,110/7892, budget0 |
| f-combo O2 | partial,22/2045, budget1 | complete,40/3071, budget0 |

These `complete` values describe the prefix pass only. Per recipe, the later
12 GuestMemorySSA runs remain incomplete with12 aggregate budget events; all12
dispatch-specialize runs remain incomplete with16 budget events. Those results
are preserved, not converted to success or conflated with prefix completion.

Both A and B independently passed C16 and LLVM16 complete-state comparisons,
plus16 register,16 Guest-memory and112 Exit-field fault controls (144 total).
Neither added exclusions, normalization or skips. All original16 input files,
four raw guarded source modules and16 full output byte streams match the prior
stage's preserved artifacts exactly. This saved-stream review is not counted
as additional execution. Source/Graph preservation and all runtime, fixture,
source, contract and harness pins passed before/after checks.

A elapsed108.25s; B96.36s. A began with651673600 bytes free and reached a minimum
363380736; B began577146880 and reached291500032. Both met the unchanged450MiB
start threshold and neither triggered the64MiB abort threshold. Groups ran
sequentially, with A archived and retired before B began.

## Precision and safety evidence

Both recipes retain the earlier strict f-cff O0 gate: the checked first-entry
actual target LOAD/switch simplifies to a bounds-checked direct edge to original
`bb_67010`. Its real guard equality LOAD, memory-error Exit and original dynamic
reentry switch remain. Four deliberately wrong final-IR controls are rejected
per recipe. Guard-check LOADs and copied switches are not counted as removed
selectors.

A recovers the previously rejected combo O2 prefix using exactly three direct
exit PHIs:

| Original definition | Definition block | Original exit | Replaced local uses |
| --- | --- | --- | --- |
| `%126` | `bb_65869` | `memory.valid19` | address GEP and RSP STORE |
| `%6740` | `memory.valid33` | `memory.valid35` | address GEP |
| `%6741` | `memory.valid33` | `memory.valid35` | STORE value |

Each exit had exactly one original predecessor/edge. The checker verifies
original versus fresh cloned incoming values, the corresponding cloned
definition and exact original exit body apart from these substitutions. The
old A saved IR fails the same three-bridge gate with0 bridges. The other three
A modules' prefix and final IR (six files) remain byte-identical to the previous
production stage.

B recovers the combo O0 high-budget prefix which the old implementation rejected
without mutation. Its110 copied blocks contain7891 original instructions plus
one bridge instruction, charged as7892. The bridge is:

```llvm
%entry.prefix.live = phi i64 [ %83, %bb_68024 ],
                            [ %200076, %bb_68024.entry.prefix ]
```

Original `%83 = sub i64 %82, 8` and fresh `%200076 = sub i64 %200075, 8` feed the
original `memory.valid69` GEP and RSP STORE. The exit's remaining instructions,
including its real call, remain exactly present. The new values do not reuse
original execution history or imply that memory facts survive the call.
The separate prior old-runtime128/16384 probe returned `non-PHI or unsupported
external live-out`, unchanged; this evidence is not represented as an old
native-semantic failure.

The new bridge checker passes its independent small positive fixture and
rejects five specified faulty variants. On each actual A/B gain fixture it also
rejects five faults: stale incoming value, wrong predecessor, invalid bridge
form, stale local use and dropped local use. These structural controls are
reported separately from the144 native semantic controls. No new instruction
or full corpus campaign, no no-Z3 real-native matrix, and no whole-dispatcher
removal is claimed here.

## Artifacts and retention

Mac directory:
`/Users/c/Desktop/deob/validation-artifacts/entry-prefix-continuation-real-20261112/`.
VM directory: `~/vmc-runs/entry-prefix-continuation-real-20261112/`.

| Archive | SHA-256 | Payload |
| --- | --- | --- |
| `recipe-A.tar.gz` | `dd85cb3efbbbd51aaac8b9c28d8837bbf87f400584e5b488ac72c2a0ba4a39e5` | 344 files,285343173 bytes |
| `recipe-B.tar.gz` | `107a7879475e07c2129b1b102bb972d45ccb8d1964e35eef351bcac41ca608cf` | 344 files,283191141 bytes |

Each archive contains its manifest as a345th file, full source snapshots,
predeclared inputs, actual commands/environment, pin checks, all raw/prefix/round/
final IR, native inputs/outputs, control reports and precision audits. Both
hosts verified the exact file set and every payload size/SHA. The candidate
subdirectories were then freshly matched against the corresponding archive
before retirement; outer reports and source freezes remain. Dual-host
`recipe-A-ARCHIVED.json`/`recipe-B-ARCHIVED.json` receipts preserve exact retired
file pins and restoration commands. Do not treat retired candidate paths as
currently expanded evidence.

The earlier budget probes, including the old high-budget rejection, remain in
`entry-prefix-budget-diagnosis-20261112.tar.gz` with SHA-256
`14b5ec8a6d908f5f6efbd233c3cfe7d3dc818f6fcdf85d2ad387782c0232a3e5`.
Those27 files were verified on both hosts before their duplicate expanded
payload was retired. Previous fixed-recipe failures and prior archives were
not rewritten.
