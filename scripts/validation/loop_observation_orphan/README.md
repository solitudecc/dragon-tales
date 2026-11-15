# Independent unanchored-subgraph control

This source-inclusion experiment isolates complete actual-edge coverage from
the separate missing-entry-definition check. The valid IR has a seeded entry
path to the query and an unreachable orphan SCC that is also an actual
predecessor. The declared conservative policy must reject the unanchored
subgraph. This is a policy test, not evidence of an old wrong execution.

`emit_overlay.py` takes the exact good overlay from the cache-transaction
experiment, the `source-v1` probe and a fresh `--out` directory. It reproduces
the good source and a single-site mutant that omits the all-node coverage
condition while retaining work charges. `prepare_plans.py`, `guard.py` and
`audit_saved.py` preserve the dated SDK/environment pins. `clarify_plans.py`
reproduces the recorded plan clarification; it does not run a target.

The packaged generator was checked to reproduce the already executed overlay
bytes. It was not itself the original target launcher. Restore exact archived
inputs or create a separately reviewed new plan; never overwrite frozen roots.
No generated overlay or mutant belongs in the production library.

See [the validation report](../../../docs/loop-observation-cache-transactions-validation-20261115.md)
for the eight good assertions, the two detected mutant failures and separate
counts from the cache-transaction experiment.
