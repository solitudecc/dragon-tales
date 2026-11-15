# Exact-observation numeric-cache transactions: independent validation

These are compiled C++ internal proof tests using an exact source inclusion of
Image `2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8`.
They execute the real D resolver and transaction destructors, rather than a
copy of their algorithm. They are not execution of the installed Image API,
Guest-native/C/Unicorn comparisons, or proof of complete real-sample CFF.

The complete original 125675-byte Image TU is saved. A narrow overlay adds a
test-only friend, a macro-controlled callback after actual successful cache
publication, and distinct Image public names. Removing only those additions
recovers the complete original source byte-for-byte. The original loadRange,
strict independentLoadRange and scalar visit hashes are respectively
`dc2959e0089d06b308d568fdfb3bfa8fe26bbf630d39b31bd517a6e2c954e6e3`,
`1cf53c314caf0eaabccab877a867d1cf0ed083c3d064b91f9fd7885b19d9879e`
and `49ba0a6cadb634a3190c6fcd5ce330247f6d9ddbf7065e34792a940847c36576`.
Production Image and its public/private ABI were not edited for access.

Each executable is one source-inclusion TU plus the frozen b348 SDK. No helper
object, product object or separate static LLVM library is linked. The included
Image has distinct defined symbols; scalar helpers are undefined imports
resolved from
`/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1/lib/libdragon.so`,
SHA `b3483229d516183c0a323319fa4d7b6c407fa1c33f1fae0b7ca6c79560431291`.
Saved loader traces bind uniquely to that SDK and the original wheel
libz3.so.4.13. This is a private implementation test, not installed-SDK native
validation. The separate metadata readelf command returned zero while emitting
unsupported DWARF-form warnings; its U/T symbol checks passed. Runtime traces
contain no sanitizer error. Libraries remain external frozen references.

| Version/mode | Actual outcome | Scope |
|---|---|---|
| v1 preparation | compile exit 1, zero unit queries | Three new wrapper return expressions required an explicit llvm::json::Object constructor |
| v2 good | exit 0, 6 scenarios / 40 assertions pass | Actual D body and internal state |
| v2 wrong parent rollback | exit 1, 6 of 40 checks fail | Deliberately omit only failed parent loads.swap |
| v2 publication hook off | exit 0, 5 scenarios / 26 assertions pass | Same common queries, no artificial budget injection |

The v1 failure is preserved completely. V2 changes only the three wrapper
constructors; fixtures, independent expectations, comparisons and D body are
unchanged. Common queries with the publication macro off have identical
results, Work, nodes and definition points. Hook observations cost no proof
Work and do not alter ordinary queries.

Both failing-parent cases first populate a real, nonempty independent cache
entry with value 5. A nested child really closes at its own complete STORE of
2 and publishes once while the parent is active. A later parent STORE alias
obligation fails. Direct snapshots check the entire cache key/range map,
trial stack, memoryActive, scalar active, independentAnchors and closed-base
pointer against their entry state. The successful conditional child must not
escape. Already spent nodes, points and Work remain spent. A later independent
query of that child succeeds on the same LoopRanges object.

The budget variant sets limit to the actual stats.work, 113, immediately after
the real child publication. It never subtracts one or creates an unsigned
underflow. The parent's next spend fails at that same 113; cleanup completes
without further Work. For the later diagnostic query the harness restores
only the original 1M ceiling, not spent counters or the exhaustion flag. The
ordinary alias failure ends at Work 118. Node/point debts remain 9/2 and 6/2,
respectively. The rollback mutant actually leaks the child in both cases and
is caught by exact-state and explicit child-cache checks, not merely by a
changed summary or unchanged public IR.

Other cases reject an unmasked L+1 anchor cycle and an earlier observation
without a seed, while accepting a complete later full overwrite of 7. The
AnchorPhase test deliberately constructs a hostile provisional-parent context
and a polluted conditional cache using the actual transaction class. The real
child resolver must discard that cache for independent anchors, reject the
earlier uninitialized observation, and restore both scopes. This artificial
setup is a phase-isolation unit, not a claim that its parent was independently
proved. All queried modules retain exact printed IR and pass verification.

V2's combined unanchored-arm test also has a missing-entry-definition route.
Its rejection alone does not isolate the final all-node anchor-coverage check.
A separate supplemental source therefore uses a valid Function whose entry
only reaches a full STORE of 7 and the query. An otherwise unreachable orphan
self-SCC is also an actual predecessor of that query. Four topology checks
establish this exact shape; four ordinary-refusal/state/IR checks then run the
real D resolver. Good passes all 8 checks at Work 38. A second source mutant
removes only the new D `!covered` refusal, retaining per-node Work charges and
the strict resolver unchanged. It is actually accepted and fails exactly the
two result/cache checks at Work 53. Thus this control tests all-node coverage,
not missing entry initialization. It enforces the helper's complete actual-CFG
policy; the unreachable orphan does not demonstrate an old wrong execution.
The previous 40 checks were not rerun or recounted in this supplement.

All compile and unit commands retained 180 seconds, 512MiB startup and 64MiB
running-floor guards. V2 ran on CPU0–3; the supplement ran on CPU4–7 only after
the NoZ build released them, alongside the main suite on other cores. The
original supplement plans remain with zero executions; revised plans clarify
that explicitly authorized disjoint concurrency, changing no source, command,
pin, guard or budget. No second CTest suite was started.

Frozen v1/v2 roots on ROCK are
`/home/ubuntu/vmc-runs/loop-observation-cache-transactions-20261115-v1` and
`/home/ubuntu/vmc-runs/loop-observation-cache-transactions-20261115-v2`.
Their Mac copies are under `/Users/c/Desktop/deob/validation-artifacts/` with
the same basenames. V1 has 25 payloads / 460737 bytes; v2 has 56 payloads /
1796290 bytes, excluding each root's two control manifests. Complete regular
member sets, sizes and SHA hashes were independently checked on both hosts.
The dual certificate SHA is
`115c0ecd244282eb1de004cebbf0198007bee459412e735e4f978cba6892d096`.
V2 SUMMARY is `7a56d9273898305fce0a76fd676264977e85479291b07ab33524747e76f2b230`;
its saved-audit report is
`7dfea744d71cacbe557141f056ffb8219b2cb6dffd202800877d673d3a40ffd9`.
The separate supplemental root is
`/home/ubuntu/vmc-runs/loop-observation-orphan-coverage-20261115-v1`.
It has 35 payloads / 1074002 bytes, excluding its two controls. Its saved audit
is `7cba252b24510a4fd623a35b06377a46b0fe9764eb8c8be7457fba341c2c837f`;
SUMMARY is `ca368cd9d235da601cf83f95b898a96b172e359fdf751bde5d464e5271c246b7`.
The final three-root dual certificate is
`loop-observation-cache-transactions-DUAL-VERIFIED-20261115-v2.json`, SHA
`a6cb3554c54e782ef562e69481ee35f9891177031b674ccd99ce4b6875b7aff4`,
stored in both hosts' artifact parent directories. These additional records do
not revise the frozen v1/v2 manifests.

The internal tests use an empty immutable H_C; they do not replace validation
of the outer joint table/cell proof, its actual guards or arbitrary Guest
execution. A later change to the D body or imported helper behavior needs new
binding and executions, not reuse of these source hashes as if unchanged.
