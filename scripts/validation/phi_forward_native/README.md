# Iterative PHI forwarding: independent execution check

The same producer calls the existing low-bit projection API on frozen old,
main and no-Z3 libraries, with unchanged default caps. Four fixtures use 40/80
forwarding blocks in either the relation or definedness proof. The fixture
generator is copied exactly from the final unit-test generators, replacing
only Catch's generation assertions with an ordinary throwing helper.

The old API refuses all four without changing IR; the new backends project
the pure uses and produce byte-identical IR to one another. Native execution
uses a separate Python u32 increment-loop/byte-array oracle. Raw IR is
calibrated first; LLVM O0/O2 raw/after compare the complete 80-byte protocol:
zero-extended return, every byte of a 64-byte arena, and pointer offset.
Inputs include 0/1/all-ones/signed boundaries/alternating bits and other u32
values, four byte alignments and four memory patterns. Unsigned wrap and the
actual bounded loop's repeated iterations are covered. No undef/poison input
is executed and no byte is excluded or normalized.

The four common main/no-Z3 outputs execute once: 2048 raw + 2048 after matches,
12288 comparator corruption controls, and 1024 compiled return-bit mutant
observations with all1024 specified detections. Saved audit rechecks twenty
streams /4096 normal frames /1024 faults without new target executions.
These are LLVM fixtures, not new real business functions, machine Guest66112
snapshots, Unicorn or a whole-corpus campaign.

Build the producer with LLVM21 headers, the repository's `include` and `lib`
include directories (the unchanged private header is required), linking the
same-ABI old SDK, then run:

```sh
python3 replay.py NEW_EVIDENCE_DIR PRODUCER OLD_SDK MAIN_SDK NOZ3_SDK
python3 saved_audit.py NEW_EVIDENCE_DIR
```

The controlled host's Z3 runtime location is recorded in `replay.py`. Source
and runtime pins, all commands, inputs/references, streams, LLVM IR, objects,
executables and failure logs remain in the evidence directory.
