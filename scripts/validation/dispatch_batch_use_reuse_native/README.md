# Dispatch-batch structural-use reuse: independent execution check

`producer.cpp` uses the same public API and source on old/main/no-Z3 libraries.
The generated loop has 16 or 70 states and twenty evolving integer values.
`fixture.hpp` is the exact `carriedBatchLoop` generator from the regression TU;
no original business source or expected result is supplied to the pass.

For each library and fixture the producer requests the fixed 180000 Work
budget, plus a separate 1000000 Work reference control. The latter is only a
direct-API fixture control: it does not increase the real Image workflow cap.
Old code refuses the 70-state low-budget case without changing IR. New code
accepts it. Both new low-budget IRs are byte-identical to the old high-budget
accepted IRs and to no-Z3; the old accepted 16-state case is also unchanged.

`replay.py` defines an independent modular-u32/byte-array recurrence. It first
calibrates raw IR, then executes transformed IR at LLVM O0 and O2. Each fixture
uses 512 inputs: zero/one/all-ones/signed boundaries/alternating bits and other
u32 values, eight limit values, and size pointers at arena offsets 32/0/1/2.
The STORE precedes the possibly overlapping LOAD. A real business bound of
256 successful iterations keeps every input finite; no fixture is skipped.

Every 80-byte frame compares the zero-extended return, all 64 arena bytes and
the size-pointer offset. There are 2048 raw and 2048 transformed matches,
12288 comparator corruption controls, and 1024 compiled return-bit mutant
observations with 1024 specified detections. Identical main/no-Z3 products are
executed once. These are LLVM native fixtures, not complete Guest66112 machine
snapshots, new Unicorn tests, or new real business samples.

Build the producer against the old same-ABI SDK and the LLVM21 headers, then:

```sh
python3 replay.py NEW_EVIDENCE_DIR PRODUCER OLD_SDK MAIN_SDK NOZ3_SDK
python3 saved_audit.py NEW_EVIDENCE_DIR
```

Use the controlled host's Z3 runtime location required by the old SDK (recorded
in `replay.py`). Every compile/execute command, input, reference, stdout/stderr,
IR, object, executable and source/runtime pin is retained. Saved-stream audit
rechecks ten streams / 4096 normal frames / 1024 fault frames with zero new
target executions. No exclusions, normalization or skipped cases are used.
