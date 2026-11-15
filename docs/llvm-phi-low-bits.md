# Original-IR PHI low-bit projection

`lib/LLVMPhiLowBits.hpp` declares the private, opt-in
`replacePhiLowTruncs(Function &, explicitTruncs, options)` helper. It is not a
pipeline default or an installed API. This component neither runs LLVM scalar
optimizers nor treats a simplified shadow module as evidence about the input.

The caller supplies original `TruncInst` candidates. For each candidate, the
helper finds a narrower PHI in the wide root PHI's block and proves that its
bits equal the requested low projection on every actual incoming edge. Constant
comparison uses only the requested low bits. Unannotated truncation, zero/sign
extension with a sufficiently wide input, and complete one-arm PHI forwarding
may expose that relation. A forwarded instruction must dominate the forwarding
PHI itself; availability at a predecessor terminator alone does not establish
the same current observation. Duplicate successor slots remain separate edges;
all incoming slots must exist and duplicate predecessor values must agree.

The relation proof and the definedness proof are separate. Definedness admits
integer constants, the result of an exact `freeze` instruction, complete PHIs,
unannotated integer casts, and flag-free add/sub/mul/and/or/xor. Arguments,
LOADs, calls, bare undef/poison, shifts and poison-generating annotations are
refused. A freeze is a defined observation boundary; its operand is not
declared defined, and two different freezes of the same operand do not become
equal. Cyclic definition components require a fully checked PHI incoming from
outside that component. A literal increment operand is not an initial value.
The helper proves complete integer values before exporting a small PHI, not
only the low portion of an otherwise possibly poison wide value.

Each attempted PHI pair owns its gray/black relation and definition caches.
Gray entries are pending cycle dependencies. A late failed arm discards the
entire pair; no partial fact is reused by the next pair or candidate. All
explicit candidates must succeed before any use is changed. The whole call
shares caps of 128 candidates, 256 proof nodes, depth 32 and 65,536 work units;
callers may lower them. Failure or exhaustion publishes no witness or change.

The edit changes only previously snapshotted pure SSA uses of original Trunc
nodes to dominating proved small PHIs. Original Truncs stay present. Direct
effect, call, alloca and terminator uses are refused. The helper verifies the
function before and after, checks the original effect/terminator nodes,
operands and metadata identities, and restores every edited use on failure.
There is no function-body replacement, CFG rewrite, address fact, alias fact,
memory forwarding or range inference. Function, Argument, BasicBlock and
Instruction objects remain the original objects.

On complete success, `provedLowPhis` contains only the selected fully defined
small PHIs. It does not export wide-PHI ranges or a `noundef` assertion about
any source argument, LOAD or freeze operand. A contextual range consumer must
independently require an actual dominating branch edge on that exact stable
SSA observation, query PHI incoming values at their predecessor, and discard
its dominance/value caches after further changes to CFG or SSA.

The first source freeze is
`/tmp/dragon-phi-low-projection-source-20261115-v1/SOURCE.json`. The new test TU
has nine cases covering shared-step recurrences, 128/64 and signed/high-bit
relations, shared versus distinct freezes, unsupported/poison definitions,
single-PHI forwarding, duplicate slots, a late-failing pair, atomic candidate
and budget refusal, and effect operand preservation. At that source freeze no
compile, focused/full test or native execution has occurred. Existing code
without this new private helper cannot compile these API tests; that is API
acceptance, not evidence of old wrong execution. Independent C/Python/native
validation is prepared separately and is not counted by these structural
checks.
