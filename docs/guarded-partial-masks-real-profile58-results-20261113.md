# Profile 58: frozen M/V real-IR diagnostic

This diagnostic is complete, with a **failed final closure gate for combo O2**.
It ran each of four frozen L2-final IR inputs once through `guarded-partial-masks`
(M), then `guarded-state-values` (V). It did not rerun the preceding 56 steps,
perform native execution, or start a fresh full profile-58 replay. The candidate
full recipe remains D37 + base19 + M1 + V1, without post-V cleanup.

## Fixed execution and source bindings

The predeclared producer and adapter are
`guarded_partial_masks_state_values_real_probe.cpp` (SHA-256
`937eaac02d41748fff7f3a10feba32f7aa59c1ffacf64c56a525c1670d27ed21`) and
`guarded_partial_masks_state_values_real.py` (SHA-256
`e93aa1eba0193cf54900f776f93e1e9472fe8980e6bbf1c242b6cb8f4f2f74cb`).
The production runtime was built from base
`cdb4f6b2850e313326d1b4728ac67e8285e364d8` plus patch SHA-256
`2b0239bf8c01669632024b75ae98f5d67856f277b68e4ade7e239b0f090a047d`.
It is not represented as a build of a later documentation commit.

The main backend was
`guarded-partial-masks-20261113/runtime-main-v1/libdragon.so`, SHA-256
`953dc02461c74b0deee26b9d2f9099236f2643141cd635a1904d90f1cb21dc1b`.
The producer was linked only against that frozen backend. The preserved PRE,
source pins, runtime manifests, exact inputs/anchors and command logs identify
every actual input. The caps stayed 64 candidates, 32 rewrites, 128 blocks and
16384 instructions per pass, with 180 seconds per producer command.

The O0 V prediction remained state 2 at `memory.valid133`, target 85740.
The O2 predictions remained state 2 at `memory.valid192`, target 69064, and
state 34 at `memory.valid129`, target 67780. Both cff inputs had null V hints.
These are predictions checked against actual SSA and actual target LOADs, not
source range, heap, table-content or memory-preservation proofs.

## Results

| Input | M candidates / attempts / rewrites | M blocks / cost | M budget | V candidates / attempts / rewrites | V blocks / cost | M closure | V closure |
| --- | --- | --- | --- | --- | --- | --- | --- |
| f-cff O0 | 0 / 0 / 0 | 0 / 0 | 0 | unavailable | 0 / 0 | true | true |
| f-cff O2 | 0 / 0 / 0 | 0 / 0 | 0 | unavailable | 0 / 0 | true | true |
| f-combo O0 | 0 / 0 / 0 | 0 / 0 | 1 | 1 / 1 / 1 | 7 / 145 | false | true |
| f-combo O2 | 2 / 2 / 2 | 10 / 222 | 0 | 2 / 2 / 2 | 10 / 82 | false | false |

All four producer commands returned zero and their output IR verified. Combo
O0 M reported `state-corridor expression or search budget exhausted`; this does
not identify a specific node or work limit. No budget was increased and no pass
was retried. The M/V reports remain Incomplete for the changed combo cases.

The unchanged V4 auditors validated the actual staged frames: M used
`mask.before -> mask.after` with no V hints; V used the actual post-M
`value.before -> value.after` with the declared hints. Original D37 anchors
were retained throughout. M passed 24 structural fault controls, V passed 39.
Old scalar/target-only metrics and their recorded failures remain separate from
the set-aware metric. M/V snapshots and aliases retain exact byte bindings;
archives dereference hardlinks so every named payload is independently present.

An initial startup refusal at 501383168 free bytes was below the unchanged
512 MiB probe gate and executed zero cases. It remains as a failure artifact.
After independently approved storage retirement, the four declared commands
ran once under the original 64 MiB running floor. Earlier profile-64 combo
180-second failures and the condition-safe profile-57 T/T/T/F result were not
replaced or relabeled.

## O2 residual diagnosis

M really redirected the original `memory.valid99` and `memory.valid216` edges
into checked copies. It did not close the whole original central dispatcher.
The final IR SHA-256 is
`8b6a5eb05f36d807f8c6c90089d980cd47e691cf0dbd5a7908c76f84c3193b93`.
The remaining central block is `memory.valid49`. Its actual sole predecessor
is `bb_66038`; the first-entry route into that block comes through
`bb_66032.bb_66038_crit_edge` from `bb_66032`.

A second read-only traversal stops expansion at the first central block. It
finds 14 actual incoming sources to `bb_66032` before that first central entry:
`bb_66531`, `bb_67843`, `memory.valid57`, `memory.valid210`,
`memory.valid222`, `memory.valid240`, `memory.valid256`, `memory.valid260`,
`memory.valid270`, `memory.valid278`, `memory.valid298`,
`memory.valid49.transition5`, `memory.valid49.transition12` and
`memory.valid49.transition24`. The saved report includes each complete shortest
path, actual successor slot, and RCX/ECX PHI value. These are CFG witnesses;
they are not proofs that every path is feasible. For example, mutually related
Boolean values in the `memory.valid298` state expression may make its original
`ugt 50` direction infeasible; the frozen metric does not invent that proof.

The shortest residual path passes through `bb_67332`, whose state is
`select(R13 == 0, 42, 43)`. The existing small switch reads the actual Guest
table and allows targets 66032 and 67384, retaining the full same-read fallback.
The state value enters the original header RCX PHI unchanged. The ELF table
base is 73792: initial entries 42/43 at 74128/74136 contain 66032/67384. The
original C has case 43 and no case 42, consistent with a default repeated
dispatch for state 42. Two other small switches similarly include missing
states 19 and 12, whose initial table entries also point to 66032.

This file-table observation is diagnosis, not an immutability or arbitrary-Guest
content proof. A future continuation must preserve each actual table read,
bounds/error behavior, full fallback and loop PHIs. Redirecting to the state
chooser itself would repeat prefix effects and can change a chosen state after
the original chooser clears its register. No such transformation was applied
in this diagnostic.

## Evidence

The complete package is
`~/vmc-runs/guarded-partial-masks-state-values-real-20261113.tar.gz`, SHA-256
`799f891ace9888c01309ff12a635022425524261536d6e9bfc3355921ed1aa1d`:
197 payload files, 1084838507 logical bytes, 92020749 compressed bytes. Both
hosts independently verified the exact regular-member set, every length and
every SHA-256. The Mac copy and verification receipt are under
`/Users/c/Desktop/deob/validation-artifacts/guarded-partial-masks-state-values-real-20261113/`.

Within `probes-v1`, retain `FINAL-STATUS.json`, `STAGED-AUDIT-RESULT.json`,
`FINAL-O2-FIRST-CENTRAL-RESIDUAL-v1.json` (SHA-256
`c2c364fd73b959aa85645d57b094830cfd0620b87f7e074961ae97713aa499df`)
and `SOURCE-C-TABLE-42-43-DIAGNOSIS-v1.json` (SHA-256
`8cc66351d4a396f01247ce08ab30f7d647d6de4303bf57322a44bb68e7aab271`).
Native count is zero; fresh full replay count is zero. This milestone seals a
completed failed precision diagnostic and identifies the next component gap.
