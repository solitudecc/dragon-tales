# Guarded integer right slices

`guarded-bit-slices` is a separate opt-in LLVM pipeline component. After the
existing finite, correlated-boolean and partial-mask predictions fail, it can
predict result bit patterns for scalar integer `lshr` and `ashr` operations
whose known shift counts leave at most three bits. Logical shifts enumerate
zero-extended patterns; arithmetic shifts enumerate sign-extended patterns.
Constant sources use their exact shifted bit pattern, including count zero.
Finite mixed shift counts combine their retained predictions. An unknown count,
a visited count at least the operand width or exhausted work rejects the
candidate. Literal undef/poison sources and such immediate operands of a source
instruction are rejected; this is not a recursive poison analysis. A freeze may
supply an opaque defined bit-pattern source.

These are untrusted target predictions. An unknown source is an opaque atom,
not a recursively inferred register value, historical PHI fact or range proof.
Any outcomes too broad to enumerate still use the complete original fallback.
Original shifts, arithmetic flags, memory checks, actual target LOADs, STOREs
and side exits remain. The small switch checks that actual LOAD result; its
fallback consumes the same value through the complete original switch. No
table contents, call effects, memory immutability or definedness are assumed.

This component admits ordinary original seeds and the existing generated seeds
supported by `guarded-checked-corridors`. A generated block cannot be part of a
copied path. Blocks appended by either component carry the checked-continuation
marker and are excluded as future seeds. Copying, root-PHI initialization,
fresh loop-backedge inputs, liveout repair and rollback use the same checked
continuation core. Calls, volatile/atomic accesses and synchronization inside
the corridor remain unsupported.

Options are `max_candidates=64`, `max_rewrites=32`, `max_blocks=128` and
`max_instructions=16384`, with hard maxima 4096/4096/4096/262144. Statistics
retain the checked/partial-corridor counters, including loop contexts and actual
backedge slots. A candidate reports Incomplete; counts are not full dispatcher
coverage. Other profiles, the default LLVM pipeline and the public ABI are
unchanged.

A successful short suffix prediction does not stop this profile's bounded
reverse search. A candidate with an actual predicted switch case returning to
its first corridor block takes priority over straight candidates. The two
existing branch/switch candidate containers keep their previous caps: a loop
may replace the last retained straight candidate when a container is full.
The first loop for an already seen original seed/slot can replace its earlier
straight variant. Same-priority discovery order is stable; branch seeds precede
switch seeds within each priority. All extra scans consume the original shared
work budget, and any cap/search exhaustion remains Incomplete. Rewrite-time
original-edge checks still apply; no generated clone is rediscovered.

Validation results, frozen inputs and limits are recorded separately. This
contract alone does not claim complete real-program loop de-flattening.
