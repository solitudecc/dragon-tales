# Portable real Tigress LLVM before calibration

This is a separate harness for the frozen `flatten-switch-O2` selected input
set. It is prepared after decoded-region baseline v4. It does not modify the
frozen `f050...` producer or its output, lift another ELF, run a Dragon pass,
run a C oracle, or start Unicorn emulation. The existing C16/UC16 records are
historical reference provenance and contribute zero new comparison counts.

A new-host calibration is required even though the actual 747,549-byte raw
LLVM is exactly equal to the archived canonical raw. The runner pins the raw
SHA `4315ddcd...` and its actual first line `; ModuleID = 'lifted'`, the original
1,829-byte driver, all selected50 files, 16 original 66,048-byte inputs, and
16 original 66,136-byte expected frames. No ModuleID or snapshot normalization
is allowed. The new sealed plan must also pin actual Python, the preserved
correct Unicorn reference environment, compiler/opt/llc, Execution.h, C header,
Python SDK wrapper, original comparator helpers, backend/shim, build manifest,
solver SONAME, CPU affinity, and all logical/resolved tool paths.

The supplied plan example is **ready=false**. A reviewed new ready plan and
its SHA are required; the example cannot execute. Paths are explicit bindings,
not old-HOME replacement rules. Store plans and new output outside both the
selected-input and source roots. The actual frozen preparation plan remains
separate from old provenance manifests.

For each host level O0/O2 the runner verifies the original raw LLVM. O2 uses
LLVM's ordinary `default<O2>` once, then verifies that result. Both use `llc
-O0` for native object generation and the pinned C compiler `-O0` for the
unchanged driver. There are 16 executions at each level, for **32 new LLVM
before comparisons**, each comparing all bytes and the original 76 register
fields, complete 65,536-byte Guest, both call counters, seven-u64 Exit, zero
budget and complete root RET. The 66,136-byte output is exactly: 24 header,
512 registers, 65,536 Guest, 56 Exit, 8 budget. Length, magic and EOF are exact.
Original `Adapter.compare` runs unchanged. Original parser and three corruption
control function bodies are loaded verbatim as AST functions, avoiding their
nonportable historical main entry points. No mathematical specification is
rewritten or substituted. All 32 successful frames receive the original
RAX/memory/exit comparator-only corruption controls: **96 controls**, not 96
additional target executions.

The correct reference interpreter remains required even though this runner
does not call its emulator. Imports and worker mappings must contain its
explicit pinned Unicorn DSO and the actual pinned SDK/backend/shim/Z3 context.
Compiler/opt processes have separate logs and their own dependency contexts.
C++ test references and SDK DSOs are not rebuilt or replaced by this runner.

The original limits remain: one sequential subprocess, 1M Guest reference
provenance limit, 180-second per-subprocess/effective complete CLI deadline,
512MiB startup free-space gate and 64MiB running floor. There is no retry,
budget expansion, skip, undefined-field exclusion or reduced comparison after
a mismatch. A mismatch stops before any after calibration; original output,
input, stderr, loader, tool logs and partial result remain. Ordinary library
and tool symlinks are recorded and rechecked; selected files reject symlinks.
All pins are checked again on successful completion.

Example execution after the parent's independent resource GO:

```sh
taskset -c 4-7 /ABSOLUTE/REFERENCE/venv/bin/python \
  /ABSOLUTE/SOURCE/tigress_dispatch_native_before.py \
  --plan /ABSOLUTE/SEALED/PLAN.json --plan-sha256 SHA256 \
  --out /ABSOLUTE/NEW/before
```

`--local-checks NEW.json` uses only stdlib and unchanged pure AST comparator
bodies, with unused Unicorn register-ID values stubbed for this reader-only
control. It imports no SDK and executes no native/Unicorn target. It proves a
positive frame, the original three exact-field corruption detections, and
rejection of short/trailing/magic/padding/budget corruption. It is not a new
native or instruction semantic campaign. The first local preparation failure
(missing tuple-assigned DATA in the control-only loader) and its source are
retained independently; the original helper was not changed.

After an actual run, `tigress_dispatch_native_saved_audit.py` re-reads all
32 original input/output pairs, all 96 controls, final pins, exact protocol
and root marker without invoking a target. A passing saved audit supplies no
new native count. The before runner provides no after execution mode; later
optimized/checked-view semantics need a separately pinned plan and this new
host's successful before receipt. This 16-input calibration alone does not
prove equivalence for every Guest input, logical size, guard miss or fault
path, and does not establish removal of the central dispatch loop.

## Actual ROCK before calibration, 2026-11-15

The independently authorized run used unchanged runner SHA
`8c283ece2b49114d61d96a08b356b37846d6572326c50c4138664d366ee7f7a9`
and READY plan
`22585a0205e5232a6eea0af4cf9ed1bda4a4754717995a689f1d5d8fe6899c4e`.
It completed once in 4.608366617 seconds on CPUs4–7, with **32/32 complete
raw-frame matches, 32 root returns, and 96/96 original comparator controls**.
Skips, exclusions and normalizations were all zero. There were no new C,
Unicorn, lift, analysis or Dragon pass calls. O0 and O2 each used the same
16 frozen inputs and all output bytes matched their frozen expected frames.

Worker initial/final mappings contained only the pinned original main SDK
backend `28ddd0d8...`, shim `9d95c640...`, correct reference UC `404233d2...`,
and wheel solver SONAME `libz3.so.4.13`. The 71 input/source/tool/runtime pins
were unchanged at completion. The old build manifest was consumed only as
SDK provenance; its historical test executable entry is not a claim about
the independently recompiled CTest test binary. No test binary was a native
calibration input.

ROCK evidence is at
`/home/ubuntu/vmc-runs/tigress-dispatch-native-before-20261115-v1`.
`before/RESULTS.json` SHA is
`fc8d3fc7cc61849abadf25ca65380ed2c0dc09df02f489559d16218084691f2a`;
`before/GUARD.json` SHA is
`5267253861fa351da95139686822c245df2239a3022360ebec3166df3002a39c`.
The saved-only audit rechecked 32 complete inputs, 32 complete outputs and
96 controls; `SAVED-AUDIT.json` SHA is
`738eefa817005ac304e0a2ce7fc89d9e72a74e30f7f27fe10a7b351bb1acd0df`.
This audit performed zero additional native executions.

All 208 new evidence payloads / 5,512,267 bytes were copied completely to
`/Users/c/Desktop/deob/validation-artifacts/tigress-dispatch-native-before-20261115-v1`.
Both hosts have exact member sets, sizes and SHA256, with FILESET
`1c6818efcc57e23fe0a7a9bd32def78349b95691ec6d077806341857eda3048e`
and certificate
`4b3ea449b44f7b63663c1f4c703ea4069652516aafb7f8db3c1e18df7917f132`.
The original selected50 input/reference set, runtime DSOs and tools remain
external exact pins rather than duplicate payloads in this new namespace.
The run and mirror are frozen. This supplies the required new-host **before**
calibration only; it adds no after/CFF-removal or broader-input claim.
