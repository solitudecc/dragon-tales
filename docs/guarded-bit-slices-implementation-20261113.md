# guarded-bit-slices implementation checkpoint (2026-11-13)

This checkpoint adds the opt-in component described in
[the contract](llvm-guarded-bit-slices.md). It extends untrusted target
prediction with bounded LShr/AShr result patterns and admits ordinary seeds to
the existing checked-continuation copier. Source operations, flags, actual
target reads, full same-read fallbacks, root-PHI initialization and fresh
backedges retain their behavior. The three core copier files are unchanged.
No public struct, C/Python ABI or default pipeline changes.

## Validation completed

| Build | Related focused suite | Full CTest |
|---|---:|---:|
| Main with Z3 | 97 cases / 7945 assertions | 1162/1162 |
| No Z3 | 97 cases / 7945 assertions | 1027/1027 |

The new file contains ten cases, covering 32/64/128-bit LSR/ASR top1..3,
constant and unknown sources, zero/width/above-width/negative/large counts,
mixed finite counts, freeze before/after shifts, retained flags and table
mutation order, calls, wrong hints, complete fallbacks, ordinary/generated
seeds, own-clone refusal, atomic budgets, duplicate backedges and old/fresh
history. The final test SHA256 is
`89bfd89f5c63d879afa69760ba93923731768da5e8418284a5484d36933f7106`.

The original seven case bodies are unchanged from the v1 test
`7b204c705ab107fc99f133b710a48fa66cc127d905fd5bd570c808895b7e7d53`.
That v1 source compiled and ran against origin/main c9cd781 and the previous
main/no-Z3 checked runtimes: each seven cases / seventeen assertions, seven
failed cases / ten failed assertions. The three appended v2 edge cases compiled
against exact c9 headers and ran on those same three old runtimes: each three
cases / fourteen assertions, three failed cases / five failed assertions.
All failures are rejection of the absent new builtin, including Catch nested
assertion translation; they are not old wrong native execution.

The first origin/main v1 postprocessor incorrectly expected one space in the
Catch summary. Its original successful compilation and failed test output are
retained; a whitespace-aware postprocessing-only receipt records the unchanged
seven-case result. No test execution or comparison criterion was changed.
An initial full-suite disk preflight refused 1069600768 free bytes before any
CTest command; the unchanged 1 GiB start gate was met before the actual run.

All 339 VM source/configuration pins match the local candidate byte for byte.
The actual VM checkout remains cdb4f6b with prior frozen changes and the new
patch. Reproduction is bound both to that complete VM diff and to local base
52018fcd62c0a306a0bc9751d4b6c57e9fbacd80 plus patch SHA256
`89fc9ab15ebdd9b9e8acea3a34adfc3ecf0e93b4610b80165980526dce403633`.
Source-pin manifest SHA256:
`4fbc19ca7f08e337ab97ea9525990406b9755c2063c95c7d18312f3a4b0edf8a`.

Frozen VM runtimes are under
`~/vmc-runs/guarded-bit-slices-20261113/runtime-{main,noz3}-v1/`.
Backend hashes are respectively
`c23bf0a5c092a5a94d3bb076f0207bb09d52970727130a68d12ad663c8ab8758`
and `58dcdd857935122229abe1fa9858081d9821bc49d06a0fa9a3764e98504df70a`.
Their manifests bind both backend and C shim, source pins and reproduction
patch. The C shims are byte-identical to the preceding checked runtimes.

## Outstanding validation at this commit

The independently frozen 5704-input before oracle is complete; see
[its separate report](guarded-bit-slices-independent-validation-20261113.md).
Candidate main/no-Z3 public, native and saved-stream validation is in progress
and is not counted here. Real profile60 is prepared and awaiting those native
gates; no Bit real candidate, target-native replay or full60 replay is claimed.
The last actual real closure result remains T/T/T/F. No all-four loop
de-flattening, new machine-instruction/Unicorn or whole-corpus campaign is
claimed. Final native/real results must be committed separately before main.
