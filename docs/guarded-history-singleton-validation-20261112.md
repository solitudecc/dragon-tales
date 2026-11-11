# Historical Boolean identity and singleton PHIs — implementation milestone

The I2 source combines two narrow changes on `4aa5916`: a dominating historical i1 PHI is one already-executed Boolean identity during the final correlated-prediction retry; a path-root PHI with one actual incoming value may be removed inside the trial when its external value dominates the seed. No memory, alias, table or callee-preservation fact is introduced. Original target LOADs and complete same-value fallback remain.

The original I regression and independent precision gates were not modified. Existing finite/correlated/STORE successful predictions keep priority over the new historical retry. Discovery and core share the singleton-PHI rule, and its use count is charged to the existing transaction limit. Region membership reuses the core preflight map.

Old I1 runtime tests compile using the existing API. In both configurations, the four historical-Boolean cases execute 81 assertions with 10 failures in three cases; the four singleton-PHI cases execute 36 assertions with four failures. The new combined focused group passes **58 cases / 2,035 assertions** in both configurations. Full CTest passes **1,123/1,123** with Z3 and **988/988** without Z3 (112.60 and 103.05 seconds). No tests, comparisons or exclusions were relaxed.

Exact candidate patch SHA: `c58e7b256f7077c1046aa48179dffc59b0b9091a37951ccf339d80038f8ddf56`.
Controller SHA: `8f87744478c5570bd0496621ebb87e3315ae20f524b9dd787d954a68340293e7`.
Core SHA: `16d19490590158413181d5cb5a171281d82fee291eb52688b4e1bd913e014491`.
Main frozen backend: `2a284c9593dceb1241e92730e4811f2aff9cd1121f5bee72f2d1f2abd2abfb96`.
No-Z3 frozen backend: `cb0c78271047196d95f1d29b54573d629fcb59053c8b6560e84fa0e6ed5aa40a`.

Source/build/old-failure/full-test evidence is `guarded-history-singleton-20261112/guarded-history-singleton-metadata-v1.tar.gz` on VM and Mac. SHA `17990c4eb11411f0a9d3d9f6f473de9d887dd52a1d4c25dfd232c3b43a147b4a`; all 68 payload files were independently checked for exact set, size and SHA. The fully archived prior K patch was reversed exactly before applying I2; this runtime does not contain K.

At this implementation milestone, the original main-build 13-recipe precision gate passes: three history copies, resumable counts `[1,1,1,0]`, and byte-identical normal/resumed final IR. All 94 previous H/H2 compatibility IR files are unchanged. Complete independent native matrices, no-Z3 strict gates, real composed replay and actual-loop observations are still being completed separately. These test-suite and precision results alone are not completion of real loop de-flattening; the earlier I1 failed gates remain archived.
