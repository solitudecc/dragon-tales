# Preserve loop discovery after a predictable suffix (2026-11-13)

The first independent public validation of the Bit v1 runtime failed its frozen
loop gate. In `l32t1`, the header reads a fresh source and computes the table
state without using a root PHI. The short `index -> dispatch` corridor already
predicts targets, so the existing first-success stop prevented searching back
to `head`. V1 produced one straight copy and zero loop contexts; its budget was
not exhausted. The fixture has no generated source blocks, and max_rewrites is
32. This is a discovery/precision defect, not evidence of wrong execution.
Candidate native execution stopped at zero; the failed output and gate remain.

The Bit-only search now retains a successful straight candidate and continues
within the original reverse-search limits. Actual predicted cases returning to
the frozen first corridor block identify loop candidates. They precede straight
candidates, with stable branch/switch order within each class. The two original
bounded containers retain their old caps; loops can replace the last retained
straight candidate. A later first loop for the same original seed/slot replaces
its earlier variant, and partial counters follow that variant. All additional
scans consume the existing shared work budget. Exhaustion still reports
Incomplete. Existing edge validation, stale-candidate refusal, transactions,
PHI repair, same-read fallbacks and core code are unchanged. Other profiles
retain first-success discovery and their prior rewrite ordering.

## Regression and completed tests

The new public regression computes its state from a fresh header slice,
independent of the header PHI, and checks ordinary/generated seeds under
max_candidates 1 and 32. It checks a copied header, fresh values, full fallback,
backedge targets, all PHI successor slots and absence of a duplicated chooser.
Against both Bit v1 runtimes c23bf0a5/58dcdd85 it compiles and fails one case /
one of ten assertions: loop_contexts is 0 instead of 1. The same final source
also compiles against exact origin/main c9cd781 headers and archived runtime
926a3abf; one of five assertions fails because the builtin is absent. These two
before results have different meanings and are not combined.

Final main/no-Z3 related tests each pass 98 cases / 8105 assertions. Full CTest
passes 1163/1163 and 1028/1028. All 339 actual VM source/configuration pins
match the local candidate. Final production CPP SHA256 is
`a82d9a337f45dc7a743b19b152f75a78cb66307d66d88977cce815d9b11d0177`;
test SHA256 is
`7393128ae1381f433d988ca0c36c919a5005c00236f29e527e65d74f63dc3673`.
The source-pin manifest is
`79314e0349554b0bed6d427f7431ffc32424bf3c60f53176ad8d33005b81f749`.

Official runtimes are frozen at
`~/vmc-runs/guarded-bit-loop-discovery-20261113/runtime-{main,noz3}-v2/`.
Their backend hashes are respectively
`02d730ce862ec7bf60ccaccb05bd1b61cd3b971b8b444b56d3a0e108b60992e3`
and `be804e7bef457bcd3fce984c2e16ed504b96152da8961a3da026e2f9c0e604e4`.
Manifest hashes are
`17c3831dbe81c46194f3bc459f01bb023069dfe32b87fb13b944cb5758c6fe9f`
and `f162ebfdf9fb5bf9645a9eb3d4246a0fb209106b5b3257ebf103a1c3ca0da76e`.
Both bind the complete source pins, local reproduction patch and prior runtime
archive. Their creation-time pending statuses remain immutable; later evidence
must bind these hashes rather than edit the manifests.

An intermediate prototype, backend 9dd884b6, passed all 38 frozen independent
API/IR recipes: 24 normal forms each had one loop context; the CALL-inside case
and refusals retained their input; three old profiles matched the frozen old
output bytes. It executed zero native targets. Final source adds explicit work
charging to the final grouping scan; that change requires its own final
runtime validation and is not substituted by the prototype result.

## Remaining gates at this implementation checkpoint

Final candidate public/native/saved-stream validation and actual real60 remain
pending. The old real result is still T/T/T/F; no full60 or new actual real
loop observation is claimed. No machine-instruction/Unicorn or whole-corpus
campaign is added. Native/real results will be committed separately before main.
The Bit-v1 runtime binaries and its initial failed independent diagnostic are
preserved in the dual-host archive SHA256
`52269575acb2ef601f3afadbcd6afb6eb939752afe5dcb9a00c1d3330a17f12d`.
