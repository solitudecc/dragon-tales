# Tigress decoded-region checked-context probe (2026-11-15)

This is an opt-in fixture adapter, separate from the production implementation.
It consumes the frozen Flatten O2 decoded-region raw LLVM and original ELF /
initial frame. The previous ten-step baseline never invoked Image/Decoder/
Corridor; its remaining dispatcher was not a measured failure of those APIs.

Existing C++ APIs are sufficient for this first admission experiment. The
adapter builds two independent `GuardedEntryContext` views from the same
unchanged raw source:

- A checks the candidate initial SP and all 640 table bytes at runtime.
- B additionally checks the complete original RSI snapshot (8 bytes).

Every guard miss keeps the ordinary root call. Candidate table offset/count
come from the independent machine input; actual ELF bytes provide proposed
guard values, not trusted memory. The target LOAD, scaled-index SSA and switch
are selected from actual Guest pointer/dataflow structure, never from block,
SSA or source state names. SP is evaluated only as a candidate from the
original input and the unique actual wrapper CALL's preceding register flush /
scalar alloca add-sub chain. RSI has no explicit wrapper flush in this source,
so its original input value is only a candidate, not a memory-preservation
certificate. The real kernel view still checks it before using any context.

The probe never substitutes RSI, initializes Guest contents, creates noalias /
assume, installs `read_known_memory`, or encodes source next-state results.
It invokes `runClosedImageTableContext` with unchanged default caps. A refused
Image proof does not authorize downstream Decoder/Corridor calls. Full raw
before/after modules, guard manifests, actual selector/load/switch text,
ordinary-body and Module/Function/Argument identity checks remain saved.
After-call IR is written before verification/identity gates, preserving a
potential failure product. The ordinary fallback is checked before and after.

## Actual old-SDK before result

ROCK root:
`/home/ubuntu/vmc-runs/tigress-dispatch-context-before-20261115-v1`.
Raw SHA is `4315ddcd08fd7e26861197be9a550ca7a1eee685be4206cc56f8c828d07abc05`;
ELF SHA is `bb98e3f3841bfe7b9a5ee813577c2c317d841b3674b48966ba6244a434dcaa6b`.
The input is exact original `input-0.bin` from the selected transfer manifest.
The adapter derived SP `0xc7f8` and proposed RSI `0x8000`; neither value is a
literal in its C++ source.

Source-v1 `00185794...` and its frozen plan were never compiled or run.
Source-v2 adds the after-call fallback check, safe missing-attribute admission,
and preservation of after IR before failure gates. Actual C++ source SHA is
`95f61b9d8213967e8009c9159d087d813b50d4077056ec27690493621832361b`;
guard SHA is `e555c34a7ac7e4c0b668efa8eea9becbf2ebe9803f45c7ce99ae09d15851382b`.
Input-plan SHA is
`471579fb9eafc72f1d7a432eb929910cb8427d2f69c76a563fd7346d4c20a108`;
guard-plan-v2 SHA is
`ade191844a492ebdae859d8caf908a0ed492b4f12a3a5f8b89e7f1d4e771a86f`.

One g++14 translation unit was compiled with `-O1 -g0` against the original
actual SDK backend `28ddd0d8...`. It links only libdragon, with no helper.o or
extra libLLVM. Both called APIs are UND in the executable. Saved LD_DEBUG
contains one Dragon init (the original SDK libdragon.so) and only the wheel
Z3 SONAME `libz3.so.4.13`. The shim `9d95c640...` is an SDK-cohort pin, not
loaded by this direct C++ client. Binding certificate SHA is
`d1e24a5d63f554ee186d7ee4ca7bdff05d5e449d30aa15eadf68907aff66693a`.

Compilation exited 0 in 10.057s; the one A/B CLI exited 0 in 1.778s. Both ran
on CPUs 4–7 under the original 180s / 512MiB startup / 64MiB running guards.
All execution-time pins matched at each guarded command's end. Later root
source edits or relinks must be tracked as separate versions; these records
refer to the old SDK at execution time.

| Actual Image result | A: SP + table | B: plus RSI guard |
|---|---:|---:|
| closed / changed | false / false | false / false |
| budget exhausted | false | false |
| guarded table bytes | 640 | 640 |
| preserved STOREs before refusal | 35 | 35 |
| constant values published | 0 | 0 |
| work | 127694 | 127713 |
| message | STORE range may overwrite a protected table byte | same |

A's manifest contains only the SP register guard; B contains SP and RSI.
Each has the full 640-byte table guard. The complete before/after view-module
text is byte-equal on both refusals (1,500,102 / 1,500,366 bytes). Ordinary
root/caller bodies and Module/Function/Argument identities are unchanged, and
the ordinary-root guard-miss fallback remains. Actual counts are two
ImageClosed calls and zero Decoder, Corridor, Guest-native, C or Unicorn
runs. `REPORT.json` SHA is
`40fe838954cfa36781cc1d90336d3c96a4f816b7e1861fe1c3943fe4eb5f8cab`.

The complete small before namespace is mirrored at
`/Users/c/Desktop/deob/validation-artifacts/tigress-dispatch-context-before-20261115-v1`.
Its 30 payload files / 7,066,631 bytes match by full regular member set, size
and SHA256 on both hosts; compiled SDK libraries are external references.
`BEFORE-SUMMARY.json` SHA is
`18abaa288eef239b63a3463d9b7cc0855ec2fcf40d5782252ed5da8b9a687f08`;
`FILESET.json` SHA is
`75368dcd7245c1a302fb79cc67c593109581b035248b9506fbc46e21da5725ff`;
`DUAL-VERIFIED.json` SHA is
`252370d24010c98f524876288b5d6fb40e5a55480fedeee26333d57167f6ef8a`.
These two control receipts are outside their own payload set. All before
payload is frozen; later core trials require a new namespace/plan.

This proves a conservative refusal category, not which exact STORE failed.
The global counters are not target-level causality. In the old implementation,
`guardFacts` can recognize the extra check chain but `substituteGuardedSP`
consumes only the SP snapshot. Using the executed RSI equality requires a
generic proof consumer of the exact original entry LOAD; no new request or
stats layout is needed. Even that improvement does not promise full closure:
stack-spilled/reloaded pointers and later unsupported ranges remain separate
proof obligations. No budget was raised and no refused sample was removed.

Future focused controls should distinguish full checked entry snapshots from
partial/non-dominating checks, a fresh LOAD at the same address, and runtime
output/table alias or guard miss. Native full-state comparisons and deliberately
wrong transformations need their own frozen reference/before calibration;
this static admission experiment does not supply them.

## Actual updated-SDK admission trials

The frozen adapter/input/header/guards/caps were also run against the two new
SDK revisions, each in a new namespace. Revision v1 used backend `435fcf7f...`
and compiled the same source once (8.993s), then made two Image calls in 1.494s.
Both still refused the possible table-overwriting STORE, with 35 preceding
preserved stores; work was 127694 / 127719 for A / B. This is not the synthetic
SP-observation failure: that message was never reported on this real input.
Its report SHA is
`1f5020a188f31c3e624b1484d1cb96a06a9c1e48c07b2ed734b73c0a710376f0`.

Final revision v2 uses backend
`a8690acf6be36264fd628aa1ebdb8075605f452327609aa91fae63e4f35b1de3`.
It reuses the byte-identical dynamically linked v1 client: the source, public
headers, compiler ABI and API layout are unchanged. It adds zero compilations.
CLIENT-REUSE.json binds the original compilation and client SHA
`c7719c38180a0f50ac6f955bfc3a40368b1fdf783f013bf864f80aef44585654`.
The new actual-library run, not a private implementation object, is separately
guarded by plan
`589e143971cf7132e60688d2651362ef80fd4e15158ed3c843ca2c2ae69cbd70`.
Its one A/B CLI exited 0 in 1.731s with all pins unchanged.

Both final cases still report `closed=false`, `changed=false`, no exhausted
budget, 640 guarded table bytes, 35 preserved stores and zero published values.
The message remains `STORE range may overwrite a protected table byte`;
work is 128584 / 128611. The added normalization observation work is charged to
the original cap, not hidden by a budget increase. Each before/after module is
byte-identical and ordinary bodies, identities and guard-miss fallback remain.
Final report SHA is
`8bee6cce43972bef3596bcff86679261a32b040e45f7d929d2b5dca5cae38829`.
There are four new Image calls across the two updated-SDK trials, zero
Decoder/Corridor calls and zero new real native/C/Unicorn runs in this adapter.
The separate raw native-before calibration is documented in
[tigress_dispatch_native_before.md](tigress_dispatch_native_before.md).

The complete two updated-SDK trial trees are frozen under
`/home/ubuntu/vmc-runs/tigress-dispatch-context-final-20261115-v1` and the same
basename in Mac `validation-artifacts`. The 41 payloads / 14,049,202 bytes pass
exact complete membership, size and SHA checks on both hosts; source pre/post
stat/ctime/SHA snapshots are equal. Manifest SHA is
`ef1f087d3597fbc94bea113a5e5fd1da0dc528d3f08d26054c0f2bd7c50710a8`,
dual-host certificate SHA is
`ad8fc897b03af83e4ea33720c7171ff8bf7e3c80d1441a5466c6d060cfd0fcea`.
Packaging executed no targets and changed no original trial files. The old
435f runtime and its three source files have their own verified complete-product
archive; the final backend remains pinned to the completed main build.

These results preserve an honest refusal; they do not establish full real
Flatten removal or identify a unique failing STORE from the global counter.
Entry snapshot facts do not prove later stack reloads or dynamic call effects.
