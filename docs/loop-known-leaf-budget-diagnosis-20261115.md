# Loop scalar-node budget diagnosis — 2026-11-15

The bounded-variable-shift build uses Image source
`b6b18cc70504e690c1eb3671858309237a06b48451ff9ace8561f6ada91dc01e`
and frozen runtime `777a373f…`. Its real B result remains a precision refusal,
not a published rewrite. The final message reports nodes138, but it is not the
first refusal and must not be used to explain the cause.

A private, renamed, complete-source inclusion with logging only reproduces
the complete saved installed-stage objects byte for byte: A work885391 and B
work947067, including unchanged ordinary functions and source modules. This
diagnostic executes two private analysis calls, zero installed Image calls and
zero Guest-native programs. Imported helpers and original source are pinned.

The first B cap occurs for candidate C51072, root query42, in the independent
anchor phase at the original `memory.valid117` STORE of `%162`. The first
rejected expression is `%157 = trunc i64 %dispatch.live2207 to i32`, at
depth19, nodes129, definition points21 and work947058. There are52942 work
units remaining. This is neither the depth32 limit nor the shared1M work cap.

Up to and including that first refusal there are90 scalar attempts and39
additional opaque-shape node charges, exactly129. The21 known-leaf visits are
all literals, with13 distinct original Value identities and8 repeats; there
are no H-map keys among them. Repeated literals include50864 and the typed
constants28,2,128,1,3. Seven opaque queries all succeed, with footprints
7/7/7/7/7/4/7 and allowances95/87/66/58/40/32/22. They refer to four distinct
roots. Repeated opaque cache hits still return and charge their complete
footprint, as required by that helper's unchanged contract.

The entire failing query ends with99 scalar attempts and28 known visits.
These are a failed execution prefix, not the size of a successful proof.
Removing eight repeated charges does not establish that the complete query
will fit. A candidate optimization must execute the full original query and
retain every STORE, anchor, alias, call and hazard obligation.

The proposed Loop-only policy charges one scalar node for each same-SSA
literal or immutable H_C leaf per outer query. Each visit must still pay Work
and pass the depth/type/undef checks. It must not reuse a node from provisional
D ranges, numeric LOAD caches, path facts or PHI results. Nested queries and
Coarse/Strong attempts share the paid-leaf set; only an outer root clears it.
Opaque footprints and the old Image APIs retain their existing policy. This
changes repeated-leaf accounting; it does not raise the numeric128 cap or
prove any additional memory fact. Its implementation and execution results
require separate validation.

Evidence on both hosts is under
`loop-image-private-v8-archive-20261115-v1/`. Archive
`private-v8-original-and-corrected.tar.gz` is2274385 bytes, SHA
`9ec63da42e0d96f12dfd944652eaf6d22d1d14681d49637094055ee826fb9e93`,
with71 regular members and12746816 raw bytes. Both hosts independently verify
the complete member set, every size/SHA and gzip CRC/EOF without a trailing
stream. Dual certificate:
`b7886aff9dc565375bc90757a582b00119286f5dc302f25852433a7010971d16`.
The exact stage gate SHA is
`7282bbb75f97ef5f3175b443815b38da58a1a8256c7dae6b9b83f6ae3f9c7218`;
the saved-only node audit is
`fb11108fa7555c11b8aec39ec614a7c777ef65eac9c5bd92741407842d52e8ee`.

The first logging-only probe failed compilation because an LLVM format object
was streamed directly into `std::ofstream`. That attempt made zero analysis
calls and is retained in the same archive. A fresh namespace changes only the
logging conversion to `.str()` and supplies the successful diagnosis. No
production semantics, budget, input or validation check changed for that fix.
