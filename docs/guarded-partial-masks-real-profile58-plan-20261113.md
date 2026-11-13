# Independent real profile 58

This is preparation only. Neither the new producer nor the new adapter has been
compiled or executed. Production code and the V4 auditor are not changed here.
The original 64-step failures, the 57-step two-hint failure and every earlier
preparation remain separate.

The new profile is exactly `D37 + base19 + guarded-partial-masks1 +
guarded-state-values1 = 58`. The inherited 56 steps, four frozen ELFs, original
source/Graph, strategy 0, product O0, four inputs, full 1 MiB/seven-Exit-field
comparison and 144 specified controls stay on their existing harness paths.
There is no cleanup after V. Both new steps use one iteration and one pass run,
with unchanged options 64 candidates, 32 rewrites, 128 blocks and 16384
instructions each. The mask pass uses existing entry/target prediction metadata;
it receives no new image-content or alias fact. The state-value hints remain
external untrusted predictions and are injected only after the mask step.

The producer `guarded_partial_masks_state_values_real_probe.cpp` accepts:

```
probe base56.ll value-hints.json-or-null output-prefix
```

The companion `guarded_partial_masks_state_values_real.py` retains the original
D37 adapter and unchanged compiler/comparator. Its additional arguments are
`--combined-probe`, `--combined-hints`, `--mask-value-probe` and `--value-hints`.
It supplies the final 58-step export to the existing native compiler and binds
the actual source/probe/hint/IR hashes. The full replay must not start until the
new runtime, focused/full/native checks and four strict structural gates are
available. Preparing this adapter is not another native replay.

## Actual phase frames

| Artifact suffix | Actual frame |
| --- | --- |
| `.mask.before.ll/.json` | Parsed base56 module before either new pass and before V-hint injection |
| `.mask.after.ll/.json` | Result of exactly one `guarded-partial-masks` run |
| `.value.before.ll/.json` | Actual V input after M and after untrusted V-hint injection |
| `.value.after.ll/.json` | Result of exactly one `guarded-state-values` run |
| `.before.ll/.json` | Alias of the actual M input |
| `.M.ll/.json` | Alias of the actual M output |
| `.V.ll/.json` | Alias of the actual V output |
| `.after.ll/.json` | Alias of the actual V output; no subsequent transformation |

The producer writes `.M-report.json`, `.V-report.json`, `.bindings.json` and
`.verify.json`. The last file states inherited56/additional2/total58. Entry
metadata must remain byte-identical throughout. Input and output modules verify.
Every JSON inventory includes the actual instruction, branch, switch and PHI
structure of the selected function.

M-before must be paired with M-after for the mask proof. V-before must be paired
with V-after for each actual frozen equality, original ICmp, selected clone path,
PHI edge and fallback proof. Comparing V-after directly against the pre-M frame
would conflate two different input CFGs. The M phase need not itself close every
loop; its rewrites must be independently valid. The final conditional closure
uses the common original source Exit-PC anchor and includes every final central
switch, original side exit, business case and unresolved path. The old metrics
stay available separately. No mask value is promoted to a range or memory proof.
The final strict report must validate the combined module, not discard ordinary
edges on the strength of an earlier phase's counters.

## Snapshot I/O and archives

Snapshot reuse requires exact rendered byte equality, independently for IR and
JSON. A snapshot remains an immutable file. Output paths cannot be overwritten.
V-after is rendered once, then `.V` and `.after` are hard links to that exact
snapshot. M-after and V-before are compared only after the real metadata
injection; differing bytes are written separately. `.bindings.json` records
every alias, and the adapter checks actual inode identity and file hashes.

Archives containing these aliases **must use `tarfile.open(...,
dereference=True)`**. Each pathname must appear as a regular payload with its own
length and SHA in the archive manifest. Link headers cannot substitute for the
complete member set. This retains both artifacts while reducing expanded disk
usage. Future tools must not mutate any frozen snapshot in place.

The old 64-step combo probes timed out at their declared 180 seconds before a
tiny-stage report was produced. Repeated dumps are a separate I/O cost; they
have not been established as the cause of those timeouts. Their raw failures
remain in archive `8e495c18d9a77d4abbafc7fc0a87cfbc410f8324165c403f1ae2ff1aff171ded`.
The fixed two-hint 57-step probe archive is
`8381dc20397da977e18c5eca320138c3dd78758142150049878a4fbe6a1d2a4a`;
it retains the strict T/T/T/F result. Its eight identical V/after pairs are a
possible explicitly verified hard-link maintenance action, not permission to
delete any unarchived evidence.

Keep the original limits: at least 512 MiB before a structural probe, at least
1 GiB before full replay, 64 MiB runtime disk floor and 180 seconds per external
probe. No candidate executes until the new source, hints, runtime, budgets,
frame bindings and staged auditor inputs are frozen. No new exclusions, skips,
normalization, corpus campaign or machine-instruction proof is claimed here.
