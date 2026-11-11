# combo O0 entry-prefix budget diagnosis

This is the preserved pre-fix diagnosis. The implemented direct-exit repair
and its separate low/high-budget results are documented in
[entry-prefix-exit-values-20261112.md](entry-prefix-exit-values-20261112.md).

Baseline `756d64b`; production implementation `bb0c429`. This is a read-only
IR diagnosis plus four separate API budget probes, not a new semantic oracle
run or an amendment to the frozen 32/2048 recipe. No production code, comparator,
inputs, legacy three-round recipe, or default limits were changed.

## Actual probes

The unchanged raw guarded source is pinned in `PREDECLARED.json`. The driver
and both runtime libraries are pinned before and after all runs in that file
and `END.json`; the driver source is copied alongside it. Each invocation runs
only the production entry-prefix API, with the explicit budgets below. It does
not execute target native code. The original runtime may now be archived:
restore its pinned main libraries from the preceding production stage's
verified runtime parts before replaying, rather than silently using build/lib.

| blocks / instructions | result | copies | elapsed seconds |
| --- | --- | --- | --- |
| 32 / 2048 | partial, budget 1 | 32 / 2007 | 2.120 |
| 32 / 4096 | partial, budget 1 | 32 / 2007 | 2.074 |
| 64 / 2048 | partial, budget 1 | 32 / 2007 | 2.059 |
| 128 / 16384 | unchanged rejection, budget 0 | 0 / 0 | 2.169 |

The first three outputs are byte-identical (SHA-256
`d37d2a158716b36f8290ca5ec31e8636c74df84d5023c2d926dde7f384506386`).
The last returns driver exit 3 with `non-PHI or unsupported external live-out`;
`rejectedUnchanged`, verifier and ordinary/module/global/argument/metadata
identity checks pass. All failures and stderr are retained. Native oracle
comparisons: **0**; no claims of C/LLVM semantic validation belong to these probes.

## Where 32 / 2048 is spent

`combo-O0-selected.json` enumerates actual selected blocks. Fifteen copied
`memory.invalid*` terminal error leaves cost 82 instructions each: 1230 of2007
(61.3%). The remaining17 forward blocks cost777. Last copied forward block is
`memory.valid27`; `memory.valid29` costs171 and `memory.invalid30` costs82,
while only41 instructions remain and32 blocks are already selected. Increasing
only either cap therefore produces exactly the same result.

These leaves implement observable register and Exit state. Removing their
behavior is incorrect. A possible later generic optimization is to retain the
original terminal leaf when all its external operands dominate the seed and
it requires no fresh prefix value. Its edge and full error behavior would
remain. The separate textual collector model in
`combo-O0-terminal-reuse-model.json` estimates27 forward blocks/2026 instructions
under32/2048 even after reusing such leaves, stopping before `memory.valid49`.
This is not implemented, not a dominance proof, and not evidence of CFF gain.
It does not remove the need to address the live-out boundary below.

## Exact high-budget live-out and minimum repair

The read-only collector for128/16384 selects110 blocks/7891 instructions before
checking outside uses (`combo-O0-high-budget-liveouts.json`). Its sole offending
value is `%83`, defined in `bb_68024` at raw line180:

```llvm
%82 = load i64, ptr %RSP, align 4
%83 = sub i64 %82, 8
...
br i1 %89, label %memory.valid69, label %memory.invalid70
```

`memory.valid69` has exactly one original predecessor, `bb_68024`, and one
original incoming edge (raw line187, independently enumerated using exact
label tokens). The two outside uses, raw lines7880/7882, are:

```llvm
%5213 = getelementptr i8, ptr %2, i64 %83
store i64 68034, ptr %5213, align 1
store i64 %83, ptr %RSP, align 4
```

This exit contains the real call to `dragon_static_entry_x86_64_15f20` and
remains outside the copied prefix. Thus it fits a narrow generic repair:
introduce an exit PHI selecting original `%83` on the original predecessor and
the fresh copied `%83` on the copied predecessor, and replace these local
uses. Do not substitute original history for the fresh value, copy the call,
or manufacture facts surviving the call. Other predecessors, uses beyond the
immediate exit, unsupported types/EH, and historical values need separate
proofs or conservative rejection. This removes a concrete algorithmic
rejection; merely raising budgets does not.

A new128/16384 real recipe must be separately predeclared, explicitly disclose
its4x block/8x instruction allowance, and retain the old32/2048 compatibility
recipe. Whether this yields a new actual selector/direct target is a future
precision gate, not an outcome of these old-runtime probes.

## Loop dispatcher is a separate boundary

The saved formal final guarded combo O0 module still has:

- `memory.valid131`, final lines5246–5251: real `%1083` LOAD from the address
  rooted at RBP-168.
- `memory.valid133`, lines5312–5340: STORE that state at RBP-264, test state<=50;
  out-of-range goes through the original fallback path.
- `memory.valid135`, lines5405–5409: state*8+90120 and bounds check.
- `memory.valid137`, lines5470–5473: actual table LOAD `%1129` and switch.

An existing state50 copied incoming edge reaches `memory.valid129`, where a
PHI also retains the original unknown path (lines5159–5185). Original loop
reentries remain. A newly isolated entrance can specialize that entrance,
without proving every loop state or preserving table contents across calls
and possible aliasing writes. No whole-dispatcher removal is claimed.

## Why legacy reports remain incomplete

`legacy-budget-reports.json` preserves all24 relevant reports from the four
modules and three rounds. All12 dispatch calls end at `conditions_visited=257`:
`LLVMDispatchSpecialization.cpp` increments then returns incomplete once the
configured256-condition limit is exceeded. This primary reason is exact.
There are16 dispatch budget events total; the four additional events may be
expression or state-context caps and cannot be uniquely attributed from the
public counters alone.

All12 GuestMemorySSA calls report budget1. This counter coalesces several
checked-edge limits via shared `budget.reported`; it does not identify the
first cap and does not mean that all module work was stopped. The output does
not justify asserting that all12 hit the six-block cap, nor that they hit the
global rewrite limit. `findCorridor` checks its6-block/256-instruction cap before
finding whether a useful join exists, so long single-predecessor guard chains
are a concrete class of unsuccessful bounded searches. A future diagnostic
reason breakdown or bounded preclassification could distinguish those from
proof/clone/repair caps. No budget is raised or incomplete status suppressed
here, and this is not treated as the primary real-gain fix.

## Evidence and replay

`EVIDENCE-INDEX.json` pins raw/final IR and original aggregate report. The raw
input itself is archived with the API probes; full final IR remains in the
previous dual-host archive `entry-prefix-real-20261111.tar.gz`, SHA-256
`81216b4fa0a4d1ef6239504faae9264241f71a52498f79d710f2e9292d097730`.
`extract_evidence.py` reproduces exact block/line excerpts and the unique-exit
predecessor check from that stage's extracted result. `run_budget_probes.py`
records exact argv, runtime/source pins, elapsed times and stdout/stderr. Its
predeclared destination must be a fresh isolated namespace when replayed;
never overwrite the preserved run.
