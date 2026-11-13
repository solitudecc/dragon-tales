# Partial address mapping: implementation milestone

The explicit `guarded-partial-corridors` controller can keep a nonempty subset
of finite address hints when other entries are missing or do not name original
switch cases. This is a prediction only. The actual target LOAD and complete
same-value fallback remain. Strict controllers retain their previous behavior.

The frozen build is `254d024` plus patch
`4cad73191215778730c5438eaa880620a26bd41095cd2cc36beeb47524714019`.
Controller source SHA-256 is
`37eb76574017c8292cb442322653d04c67f4549999c3fdec2ca0a8e02d90066d`;
regression source is
`b6fbfd07f03a1b81034ead2bf8d6db05e8c4d88e13c5f342fbd20f3231a83795`.
The core and original J regression are unchanged.

The final four regression cases compile on the preceding J runtimes in both
builds. Each old run executes 166 assertions and fails three cases / ten
assertions. The builtin already exists there: these failures demonstrate the
missing mapping behavior, rather than an absent API. New related focus passes
70 cases / 2778 assertions in each build. Full CTest passes 1135/1135 with Z3
and 1000/1000 without Z3. No checks were relaxed.

Both new runtimes generate exactly the previous J 41, I2 45 and H 94 saved IR
products, including original reports/counters checked by the compatibility
scripts. The J byte comparator detects all 41 specified byte corruptions.
Both K API products also equal their previously executed native IR exactly.
These are compatibility checks; prior native observations are not recounted.

Frozen backend SHA-256:

- main: `d33340910d58a340c0278fba19a9eafb3e10967e3dac287154e69a33ac2efd66`
- no-Z3: `75ce04792e413360bc6ce3dc1133e8e70f6fbd6e2320ef599c29065521d85fb0`

Source, old failures, focused/full logs and compatibility evidence are in
`~/vmc-runs/guarded-partial-addresses-20261112/`.
The dual-host metadata archive has SHA-256
`3cbd89600ce072b55a451ee46654a6148421515fe5165b1595507f08220313f3`:
838 payload files, exact membership and every byte length/hash independently
verified on macOS. Local copy:
`/Users/c/Desktop/deob/validation-artifacts/guarded-partial-addresses-20261112/guarded-partial-addresses-metadata-v1.tar.gz`.
Raw runtimes are retained separately.

This commit seals implementation and unit/compatibility validation only.
Independent native tests for the newly admitted partial mappings and real
four-input loop probes are pending review and will be recorded separately.
It does not establish complete loop de-flattening. No new instruction, Unicorn
or full 69-input corpus campaign is claimed. Merge into main remains pending
required final validation.
