# Faster independent inventory output with the frozen schema

The expensive raw-frame binding is dominated by inventory printing. The
frozen generic helpers `printed()` and `operand()` call `Value::print` and
`printAsOperand` without sharing a `ModuleSlotTracker`; the state inventory
adds the same pattern in `typedOperand()` for every operand and PHI incoming.
The new standalone `checked_context_inventory_shared.cpp` shares one tracker
within a single parsed, verified, immutable Module. It changes no frozen
exporter, checker, default gate or production pass.

## Measured scope and exact output

All 28 existing named frames match the original complete JSON byte for byte:
C59 four pairs (8), Bit four pairs (8), V four pairs (8), and original D37
base-schema frames (4). State-schema 24 and original five-field base-schema 4
are emitted directly; no field is removed to manufacture a base inventory.
Every full output is compared without normalization or alpha equivalence.
Sizes and SHA also match. All 84 comparator controls reject a single middle
byte flip, last-byte truncation and extra trailing whitespace. New renders
are retained compressed, including every duplicate named frame.

| frozen scope | frames | new total CLI seconds including compressed capture | full JSON bytes |
| --- | ---: | ---: | ---: |
| C59 pairs | 8 | 28.885 | 158956498 |
| Bit pairs | 8 | 29.337 | 165185181 |
| V pairs | 8 | 27.991 | 152962716 |
| Original D37 base schema | 4 | 0.830 | 9606183 |

The initial cff O0 Bit-before probe emits 14327335 identical bytes in 2.093s:
parse/verify 0.044s, inventory 1.664s, remaining time serialization/write/process
cost. Direct profiling of the unchanged Python Proof on existing JSON takes
0.990s for cff O0 and 1.733s for combo O2, including cProfile overhead, plus
JSON parsing 0.152/0.317s. Its `current()` queries accumulate 0.258/0.276s, so
those queries do not explain the historical tens-of-seconds pair times.

After the complete 28-frame compatibility gate passed, one unchanged safe-v2
CLI was run on the same combo O2 raw/JSON inputs and formal main runtime,
with only its explicit inventory-tool argument selecting the new standalone
utility. It proves 2 contexts and 2 exact raw bindings in 11.260s; the saved
historical original-tool timing for that same pair is 96.306s (about 8.55x).
This is a historical timing comparison, not a simultaneous controlled A/B
benchmark. The old heavy inventory executable was not rerun.

There are 31 new pure-output tool processes: 28 compatibility frames, one first
probe and two raw bindings in the same-scope pair. Pipeline passes=0,
target-native executions=0, fresh full replays=0. The actual real60 archive
`2f39e589...` with 186 regular members and its result document remain unchanged.
No new entry-closure or native-correctness coverage is inferred from printing
faster.

## Usage and immutable Module boundary

Compile this utility against the public source headers and LLVM21 headers,
linking the explicit formal library directory:

```
c++ -std=c++20 -I<source>/include -I/usr/lib/llvm-21/include \
  scripts/validation/checked_context_inventory_shared.cpp \
  -L<formal-runtime> -Wl,-rpath,<formal-runtime> -ldragon -pthread \
  -o inventory-shared
```

Set `LD_LIBRARY_PATH` to that formal directory. State schema uses the same
four-argument process convention as the existing inventory utility:

```
inventory-shared frame.ll function-symbol output.json
inventory-shared frame.ll function-symbol original-base-schema.json base
```

The second command directly calls the original generic inventory builder.
The tracker is recreated for each process; it is not retained across a
producer's optimization or Module mutation. Reusing a tracker across such
mutations is outside this tool's design. Printing still executes all original
inventory loops and emits every opcode, flag, operand, type and PHI slot.
Timing and call-count diagnostics go to stderr; the original JSON schema is
unchanged.

The tested runtime backend is
`02d730ce862ec7bf60ccaccb05bd1b61cd3b971b8b444b56d3a0e108b60992e3`,
manifest `17c3831dbe81c46194f3bc459f01bb023069dfe32b87fb13b944cb5758c6fe9f`.
Source SHA is
`4044f1bdba9c486ac2aec9f5cc67c64ab9cb151b2414c66037973f298ec55550`;
binary SHA is
`4ba0db00cb71d0ea283b2ad9766dbfaf54fa60afde377a8d48faf6dd2c207942`.
Explicit LD/rpath resolution is verified. The frozen safe-v2 checker and all
its raw-binding checks remain unchanged; no default tool is substituted.

## Preserved experimental startup failures

The first Proof-only profiling wrapper omitted mandatory raw-binding CLI
arguments. Python cProfile reported process exit 0 despite the CLI refusal;
no Proof result existed, and it is not counted as a successful profile.
The next direct importer omitted the frozen sibling-source directory from
`sys.path` and failed before any Proof call. Both original diagnostics and
zero-call classifications are retained. The successful profiler imports the
unchanged source with its ordinary sibling import path and invokes Proof
only for timing; it makes no raw-binding validation claim.

## Owned build-cache dependency closure

For future fresh full60 work, the Python wrapper honors an existing explicit
`DRAGON_C_LIB` before its build-directory fallback. The inherited static
native driver links emitted module objects and source headers; a nonstatic
path uses the resolved `dragon._LIB`. Standalone CPP producers and inventory
tools use explicit formal LD/rpath. Necessary inputs are frozen source and
headers, Python wrapper/harness, formal-v2 libraries, system LLVM/compiler
and original ELF/source fixtures. There is no direct future read dependency
from this owner on owned build `libdragon.so`, build `c_shim/libdragon_c.so`
or `bin/dragon_tests`, in either build. This closure is not a deletion action;
all cache retirement still needs the root's separate exact proposal and GO.
