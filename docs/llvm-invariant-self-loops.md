# Isolating pure invariant self loops

`invariant-self-loops` is an opt-in LLVM pipeline component. It preserves a
nonterminating path while separating it from a shared state dispatcher. It does
not establish any predicate, table, heap, calling-convention or callee-effect
fact and is absent from the default pipeline.

The accepted block has only PHIs, one scalar `icmp ugt` of one of those PHIs
against a constant, and a conditional branch. Its true successor is itself and
its false successor is another block. Every PHI has exactly one self incoming,
whose value is that same PHI, and at least one external incoming edge. The
external edges may contain duplicate predecessor slots; their values and
multiplicity remain unchanged. The original input module must verify.

The component preserves the comparison, the first conditional branch and its
false destination. It changes only the true successor to an appended block
containing `br label %spin`. Each original PHI loses exactly its self incoming;
one-input PHIs are retained. On defined executions taking the true edge, all
loop values previously remained unchanged, and no effects occurred before the
next identical comparison. The new path therefore retains the same divergence.
The conditional branch still consumes its original value at its original point;
no freeze, `noundef`, assumption or `mustprogress` property is introduced.
LLVM specifies undefined behavior for branching on poison or undef. See the
[LLVM 21 branch semantics](https://releases.llvm.org/21.1.0/docs/LangRef.html#br-instruction).

The narrow admission rejects changing or cross-coupled self PHIs, an absent
external incoming edge, direct undef/poison incoming values, nonconstant compare
bounds, a different predicate or orientation, two self slots, and any other
instruction in the loop. That excludes fresh freezes, loads, stores, calls and
synchronization in the repeated block. External values are not recomputed by the
loop. Existing loop metadata, function `mustprogress`/`willreturn`, EH,
blockaddress and debug/local-metadata identities are conservatively unsupported.
The LLVM [progress contract](https://releases.llvm.org/21.1.0/docs/LangRef.html#function-attributes)
is not silently attached to the new spin.

Configuration defaults are `max_loops=64`, `max_work=1000000` and
`max_blocks=65536`. Hard caps are 65536, 1000000 and 65536 respectively. Work
covers function/block/instruction/operand scans, repeated PHI admission checks,
and a reservation for one whole-body trial copy per affected function plus its
new spin branches. `max_blocks` bounds the final original-plus-appended block
count. All configured budget checks finish before the first transaction.
Unsupported candidate loops are reported as rejected/Incomplete; other proved
candidates can still be isolated. A budget rejection leaves the whole input
unchanged. Transaction verification failure leaves that source function
unchanged and reports Incomplete.

The implementation reuses the repository's trial-function strategy: copy and
validate a function privately, remap trial arguments back to the original
arguments, then splice the verified body. Module, Function, Argument, global
function-pointer initializers, external callers, original attributes and named
module metadata retain their identities. Body pointers must not be retained
across a successful call. The new spin has the existing generated marker so
later corridor discovery does not treat it as a new business seed. Original
branch metadata remains; a second invocation finds no conditional self edge at
the isolated block.

Counters distinguish `candidate_loops`, `isolated_loops`,
`removed_self_incomings`, `added_blocks`, `rejected`, `budget_exhausted`,
`scanned_instructions`, and accounted `work`, plus available/complete/changed.
Isolated loops are not removed loops, and isolation is not full deflattening.
This component can precede a separately validated partial-prediction controller;
no such controller is introduced here.

## Validation contract

Four public-pipeline regression groups check retained first-condition/false-exit
structure and external identity, duplicate incoming slots, seventeen unsupported
or unsafe shapes, configured budgets and opt-in registration. The previous
runtime has no builtin, so old public-test failures are API-availability evidence
only. They do not demonstrate an old wrong-result bug. Before/after native
semantics must be validated separately.

An independent native oracle must include both returning and diverging inputs.
The reference model explicitly classifies divergence; it must not compile a C
empty infinite loop and assume that language-level forward-progress rules leave
it intact. Boundary inputs include 0, 49, 50, 51, the sign bit and all-ones,
multiple external PHI values, and both duplicate external slots.

For returning inputs, compare return values and all Guest/register/Exit bytes.
For a diverging input, run before/after functions in separate controlled child
processes with the same shared input arena. The parent must first establish
that execution actually reached the compiled pure loop: use ptrace PC sampling
and a pinned disassembly showing that the sampled region has no store, call or
return and closes on its own branch. Sample again after resuming and compare
complete shared-state snapshots to the independent model's pre-spin state.
Record exact PCs, instruction bytes, process state, signals and both snapshots,
then terminate the child explicitly. A timeout alone is neither a pass nor a
proof of nontermination. Finite observations corroborate the structural
induction argument; they are not a standalone proof of infinite execution.

Specified negative controls must force an unexpected return on a diverging
input, force a spin on a returning input, corrupt a STORE before the spin, and
corrupt the return value on the retained false exit. The harness must detect each designated
failure through return classification or full-state comparison. Poison/undef
cases are structural refusal tests, not native expected-value tests.


## Frozen K validation (2026-11-12)

Main and no-Z3 full suites passed **1119/1119** and **984/984**. The public
four-group old-runtime regression is an unavailable-builtin failure, not an
old semantic counterexample. The initial candidate also exposed a diagnostic
issue: `max_work=0` returned Unavailable before inspecting budget exhaustion;
the pipeline status mapping was corrected to report Incomplete. Its failed
record and the original test are retained by the root validation campaign.
The core transform was not changed by that status fix.

The independent native campaign is **clang-21 -O2 only**, as declared before
candidate execution. Its final source-v3 observations comprise 36 baseline,
36 main and 36 no-Z3 full-state matches (**108 normal**), plus 36 observations
for each of four faulty IR variants (**144 fault observations**). Exactly
**72 specified faulty observations** differ from the normal reference, and all
72 were detected. This is 252 records containing **378 full saved frames**,
not 378 independent inputs. There are 126 two-sample self-branch witnesses
across normal and control records. Skips, exclusions and normalization are zero.

The parent obtains the stopped child's actual PC and checks `0x14000000`
(AArch64 branch to its own address), within the exact loaded `oracle` symbol.
After resuming, the second sample must reach the same PC and both complete
65536-byte arena snapshots and return-header bytes must equal the independent
reference. A timeout or arbitrary stopped PC cannot pass. A separate saved-data
review checked each sampled offset against the exact saved native ELF's dynamic
symbol and instruction bytes, rather than trusting only the reported opcode.
It also recomputed every model/state comparison and all source/IR/runner pins.
This review added **zero native executions**.

The executed reference sources are copied without modification under
`scripts/validation/invariant_self_loop_reference/`; `SOURCE-v3.json` maps each
filename to its exact SHA-256. `oracle.py` is
`6af1994d196080bcd75b3ff035652dbee0cf534f06785170bebe57f6a353475e`,
and `observe.cpp` is
`d7db305769251cf6ad130dea502378bb2ee5068652c326081f45a35437564a8e`.
The original execution auditor and its failed relative-path predecessor are
preserved there as evidence. For repeatable read-only review, use
`invariant_self_loop_saved_audit.py ARCHIVE_ROOT --manifest MANIFEST-before.json`.
That validator loads the pinned reference through an explicit path and private
module identity; it never imports a possibly unrelated module named `oracle`,
creates pycache files in the archive, or reruns compiler/native commands.

Main backend `c05fb5ec022b35ce192b563ba48a06821ec56c33b0e72d5e076f94bd1faa99f0`
and no-Z3 backend
`c5ddfdec70a2ba292ba8c94b6bbc6f858a489ec5c28c8b5e937ef3809df6d51c`
were rebound through the actual API to the compiled candidate IR, byte for
byte. Both candidate IRs have SHA-256
`ade4fac98d9e4668bcb3b918132d0ca8294618932a94f43ef3feaff31e96d92a`.
No subsequent rebuild is substituted for these native runtime bindings.

Complete native evidence (including earlier attempts) is archived on both hosts:

- VM: `/home/cc/vmc-runs/invariant-self-loops-native-20261112.tar.gz`.
- Mac: `/Users/c/Desktop/deob/validation-artifacts/invariant-self-loops-native-20261112/invariant-self-loops-native-20261112.tar.gz`.
- Archive SHA-256: `f5a951c3546ba42301a7e0456fea4f4fcfa7b2b6cfb3e648b3d51327947ca844`.

The archive contains 977 payload files / 37,613,487 uncompressed bytes. Both
archive payloads and both expanded file sets were checked against the exact
manifest, including file count, size and SHA-256. Raw VM evidence remains present.
The initial `observe` relative executable-path failure, successful older v2
baseline/control runs, the old runner's weaker nonzero-symbol-size test, and the
saved-auditor relative/absolute-path assertion correction remain separate.
Only source-v3 runs count in the final totals above; v2 was not a semantic
failure and is not counted twice.

Whole real-program validation of a recipe containing K is **pending**. These
results establish the narrow transform and its divergence-preservation checks;
they do not establish complete real CFF removal, heap facts, callee preservation,
or observed fast-loop closure for the four real samples.
