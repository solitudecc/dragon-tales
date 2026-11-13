# Partial Boolean-mask implementation milestone

This milestone implements the opt-in `guarded-partial-masks` component. It
does not complete the current real loop-de-flattening campaign. Independent
native validation and the composed real M/V pipeline are still in progress;
the feature branch must remain separate from `main` until their required gates
pass. See [the component contract](llvm-guarded-partial-masks.md).

## Frozen implementation

The tested worktree is baseline `cdb4f6b2850e313326d1b4728ac67e8285e364d8`
plus patch SHA-256
`2b0239bf8c01669632024b75ae98f5d67856f277b68e4ade7e239b0f090a047d`.
`GuardedStateCorridors.cpp` is
`eee5537ba254cac3bfee7884eb5a90ffe6876fe4f9ca1de41ca6d7b09a1c17ac`.
The existing promotion core is unchanged, as is the corrected state-value
guard implementation. The original J/F/H controllers and default pipeline do
not enable this new retry.

Unknown Boolean bit patterns and a zero-mask outcome are untrusted target
predictions. Every original operation, flag, memory access and actual target
load still executes. The loaded target selects a known existing case or its
complete original fallback. No Boolean relation, state range, memory contents,
call effects or definedness fact is exported. Explicit invalid dependencies
and resource failures remain fatal to a query; an out-of-width finite shift
count also rejects the new retry when its left operand is ordinary unknown.

## Old-code regression and full tests

The final public-pipeline tests compile and run against both frozen corrected
pre-M runtimes (`ed2aa795` and `7e4c8448`). Each runs five cases /54 assertions:
the legacy-only case passes, and four cases fail five assertions because the
new builtin is absent. This is an API/precision regression, not evidence of
old wrong native execution. The legacy case exercises the same masked source
expression through J/F/H and checks unchanged IR and counters.

On each final M runtime, focused tests pass81 cases /3971 assertions. Full
CTest passes1146/1146 on the main build and1011/1011 without Z3. Builds and
tests run on the owned Ubuntu worktree with two jobs; no macOS build is claimed.
The initial production-only build and its later shift-policy correction remain
in the evidence. Only the final source is used for these test results.

Runtime backend SHA-256 values:

| Build | Backend SHA-256 |
|---|---|
| Main | `953dc02461c74b0deee26b9d2f9099236f2643141cd635a1904d90f1cb21dc1b` |
| No Z3 | `d8260b587c2d2b7aade948c3553dade92266f10db379fd20eeb7f42c4ea676dc` |

Compatibility checks execute the existing public drivers: J41, I245 and H94
IR/report products remain exact in each build; K's actual native IR remains
exact. Corrected state-values retains all41 IR products and13 reports, with41
damaged-byte controls detected. These are compatibility checks; historical
native observations are not rerun or added to this component's native counts.

## Sealed implementation evidence and remaining work

Archive SHA-256:
`27c86d470ca6d10c1e9ee7ed72d6ef937712c406caa2a0316e5fa0a1d015fae3`.
It contains1067 payloads, including tracked-source pins, the exact candidate
sources/patch, old-test failures, focused/full logs and compatibility checks.
Both hosts verify the exact member set, every length and every SHA. Runtimes
and real/native evidence have separate namespaces and are not counted as
payloads in this metadata archive.

VM: `~/vmc-runs/guarded-partial-masks-20261113/guarded-partial-masks-metadata-v1.tar.gz`.
Mac: `/Users/c/Desktop/deob/validation-artifacts/guarded-partial-masks-20261113/guarded-partial-masks-metadata-v1.tar.gz`.

At this implementation milestone, no fresh full58-step real replay has run.
The previous sound structural closure result remains three of four, and actual
repeated-loop observations exist for cff O0/O2 only. The upcoming staged M/V
diagnostic must use each pass's actual before/after frame, and full replay and
actual combination-loop observations remain required. No new machine-instruction,
Unicorn or full69-input corpus campaign, exclusions or relaxed comparisons are
claimed here.
