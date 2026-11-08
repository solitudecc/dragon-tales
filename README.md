# dragon-tales

A binary analysis library for disassembling, assembling, and analyzing machine code across multiple architectures. Built on LLVM 21.

Features:
- Disassembly (linear sweep and recursive traversal) and assembly across six architectures
- CFG and function recovery, with x86-64 CET landing pads distinguished from independent function entries ([scope and validation](docs/cet-function-boundaries.md))
- **IGNIL** — a lightweight intermediate language for architecture-independent analysis
- **IGNIL peephole simplifier** — block-local constant folding, algebraic identities, dead-Temp elimination
- **Backward slicer** — propagates values through IGNIL CFGs, honouring sub-register aliasing; join nodes are merged when every path yields the same constant
- **LLVMLifter** — lifts a recovered `Function`, or a single flattened IGNIL block (no CFG needed), to LLVM IR, with an optimization pipeline (mem2reg/instcombine/GVN/DCE) on top
- **Composable pass pipelines** — register C++/C/Python plugins for Graph or LLVM stages, configure steps and budgets, track analysis invalidation and inspect per-pass reports. The default LLVM sweep remains compatible; see [architecture and plugin examples](docs/pass-pipeline.md).
- **Finite-set dispatcher recovery** — opt-in Graph pass grows and revalidates computed jump targets, with conservative call/alias barriers and explicit-store stack tables. C++/C/Python APIs work without Z3; see [scope and validation](docs/finite-dispatch.md).
- **Jump-table content proofs** — x86/AArch64 targets require contents proved at the actual LOAD, from Guest writes or an explicit trusted read contract. Initial image bytes and read-only section flags are insufficient; see [contracts and compatibility](docs/table-content-proof.md).
- **Proved call effects** — optional finite analysis derives preserved state, stack write footprints and agreed constants from bounded x86 call trees, including restored stack frames; see [proof limits](docs/stack-call-effects.md).
- **Conditional initial-state queries** — explicitly seed admitted entries with owned register/memory facts and follow writes, calls and loops conservatively. Read-only C++/C/Python results retain their assumptions; see [contracts and limits](docs/finite-initial-state.md).
- **Integer predicate folding** — optional bounded Z3 proofs replace constant branch/select conditions and simplify LLVM CFGs; included in Python `LLVMLifter.optimize()`. See [scope, API and validation](docs/llvm-predicate-folding.md).
- **Linear MBA simplification** — synthesizes smaller integer expressions from truth signatures, requires a word-level Z3 equivalence proof for every rewrite, and preserves independent SSA/memory inputs. Included in Python `LLVMLifter.optimize()`; see [limits and validation](docs/llvm-mba-simplification.md).
- **Static function-entry views** — explicitly limit external entry to each selected function's start, allowing earlier definitions to constrain later blocks; other PCs return an explicit exit. Default resumable entry remains available. See [entry contracts and validation](docs/llvm-static-entry.md).
- **LLVM Guest execution (V2)** — opt-in x86-32/x86-64/AArch64 state and memory dispatch, architectural near CALL/RET, explicit transfer/missing-target/budget exits, and conservative import provenance. Legacy and trace interfaces remain available; see the [execution contract](docs/llvm-execution-contract.md).
- **Z3 integration** — lower a sliced expression to a Z3 bitvector formula, enumerate the concrete values it can take, or solve for a unique one
- **Symbolic execution** — run an IGNIL block over Z3 bitvectors with registers and memory seeded concretely or symbolically: solve for an input that makes a check pass, prove what an obfuscated expression really computes, and resolve a computed jump (jump table, VM dispatch) to the concrete addresses it can reach
- **SwitchDetector** — recognize a jump-table pattern and bound, and decode supplied table bytes; runtime CFG targets additionally require a content proof
- **FLIRT-compatible signature matching** — identify known functions from `.pat` signature databases
- **Trace lifting** — lift an arbitrary linear instruction sequence (e.g. a captured execution trace) straight to IGNIL, bypassing CFG/function/block-validity analysis entirely
- **Binary loader** — parse an ELF or PE/COFF file (or in-memory bytes) straight into a `LoadedImage`: sections and ELF `PT_LOAD` segments at their virtual addresses with R/W/X permissions, entry point, image base, symbols with import/export flags, and the architecture auto-detected from the header (no need to name it). `loader::disassembleImage` goes from a file to a populated `Graph` in one call. Built on LLVM's `llvm::object`, so no extra dependency
- **`disassembler` CLI** — a command-line tool that loads an ELF or PE binary and dumps disassembly, useful for quick inspection and scripting
- C shim (`dragon_c.h`) and Python API (cffi) covering most of the feature set above (a few C++-only corners — pattern-matching DSL templates, architecture-specific instruction classification helpers, and similar — are documented as such in `docs/cpp_api.md`)

## Requirements

- LLVM 21 (must be installed on the system)
- CMake 3.20 or later
- A C++20 compiler
- Python 3 with cffi (optional, for the Python API)

## Supported architectures

- x86 (32-bit and 64-bit)
- ARM (32-bit and 64-bit)
- RISC-V (32-bit and 64-bit)

## Prebuilt releases

Tagged releases ship a self-contained SDK archive per platform
(`dragon-<version>-{linux-x64,macos-arm64,windows-x64}.{tar.gz,zip}`) on the
GitHub [Releases](../../releases) page. LLVM is statically linked into the
libraries, so **no system LLVM install is required** to use them; Z3 is bundled
alongside. Each archive contains:

```
lib/      libdragon + libdragon_c (shared) and the bundled Z3 runtime
bin/      the `disassembler` CLI (plus the DLLs on Windows)
include/  the C++ headers (dragon/...) and the C-shim header (dragon/dragon_c.h)
share/dragon/python/dragon.py   the Python API
```

Point the Python API at the bundled library with
`DRAGON_C_LIB=/path/to/lib/libdragon_c.so` (`.dylib`/`.dll` as appropriate).

Use headers and libraries from the same SDK. The table-content update changes
public C++ object layouts, so C++ clients must rebuild. Existing opaque C entry
points and the Guest ABI remain available; the stricter default analysis can
leave unproved image tables unresolved.

To build from source instead:

## Building

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

To run the tests:

```sh
ctest --output-on-failure
```

A handful of x86 semantic round-trip tests are *expensive*: they JIT-compile
the lifted IR and emulate the raw bytes under Unicorn across a large
cross-product of edge-case inputs (thousands of round-trips each). They are
tagged `[.][expensive]`, which hides them from the default `ctest` run so it
stays fast. Run them explicitly — typically on a more powerful machine or in
CI — with:

```sh
./build/bin/dragon_tests "[expensive]"
```

### Building on Windows (Visual Studio + vcpkg)

On Windows the dependency discovery in `CMakeLists.txt` needs a few hints,
because it is written for Linux package layouts: Z3 is located through LLVM's
bundled `FindZ3.cmake` module, and Unicorn is located through `pkg-config`.
Neither mechanism knows where vcpkg installs things, and LLVM is found via
`LLVM_DIR` rather than the system path. Supplying three hints fixes all of it.

**Prerequisites**

- Visual Studio 2022 (or its Build Tools) with the *Desktop development with C++* workload
- CMake 3.20+
- LLVM 21 unpacked somewhere, e.g. `C:\llvm-21` (the prebuilt
  `clang+llvm-*-x86_64-pc-windows-msvc` release works)
- [vcpkg](https://vcpkg.io)

**1. Install the libraries with vcpkg** (the `x64-windows` triplet; `pkgconf`
provides the `pkg-config` executable Unicorn discovery needs):

```powershell
vcpkg install z3:x64-windows unicorn:x64-windows pkgconf:x64-windows
```

**2. The three hints CMake needs**

| Hint | Purpose | Example value |
|------|---------|---------------|
| `LLVM_DIR` | locate LLVM 21 | `C:/llvm-21/lib/cmake/llvm` |
| `CMAKE_PREFIX_PATH` | let `FindZ3` see the vcpkg tree | `C:/vcpkg/installed/x64-windows` |
| `PKG_CONFIG_EXECUTABLE` + `PKG_CONFIG_PATH` | let Unicorn be found via pkg-config | `.../tools/pkgconf/pkgconf.exe` and `.../installed/x64-windows/lib/pkgconfig` |

**3a. Configure and build (one command)**

```powershell
$env:PKG_CONFIG_PATH = "C:/vcpkg/installed/x64-windows/lib/pkgconfig"
cmake -S . -B build `
  -DLLVM_DIR=C:/llvm-21/lib/cmake/llvm `
  -DCMAKE_PREFIX_PATH=C:/vcpkg/installed/x64-windows `
  -DPKG_CONFIG_EXECUTABLE=C:/vcpkg/installed/x64-windows/tools/pkgconf/pkgconf.exe `
  -DDRAGON_USE_Z3=ON -DDRAGON_USE_UNICORN=ON
cmake --build build --config Release
```

A successful configure prints:

```
-- Found LLVM 21.1.8 at C:/llvm-21/lib/cmake/llvm
-- Found Z3 5.0.0 at C:/vcpkg/installed/x64-windows/lib/libz3.lib
-- Found Unicorn 2.1.4
```

**3b. Or use a preset (recommended)**

Save the settings once in a `CMakeUserPresets.json` next to `CMakeLists.txt`,
then configure with a single flag. This file holds machine-specific paths, so
keep it out of version control (it is git-ignored).

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "win-vcpkg",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "LLVM_DIR": "C:/llvm-21/lib/cmake/llvm",
        "CMAKE_PREFIX_PATH": "C:/vcpkg/installed/x64-windows",
        "PKG_CONFIG_EXECUTABLE": "C:/vcpkg/installed/x64-windows/tools/pkgconf/pkgconf.exe",
        "DRAGON_USE_Z3": "ON",
        "DRAGON_USE_UNICORN": "ON"
      },
      "environment": {
        "PKG_CONFIG_PATH": "C:/vcpkg/installed/x64-windows/lib/pkgconfig"
      }
    }
  ],
  "buildPresets": [
    { "name": "win-vcpkg", "configurePreset": "win-vcpkg", "configuration": "Release" }
  ]
}
```

```powershell
cmake --preset win-vcpkg
cmake --build build --config Release
```

**Notes**

- Adjust the paths to your machine. If vcpkg lives elsewhere, set
  `%VCPKG_ROOT%` accordingly; if you unpacked LLVM elsewhere, point `LLVM_DIR`
  at that tree's `lib/cmake/llvm`.
- Do **not** pass vcpkg's toolchain file
  (`-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake`) here. It restricts package search
  and stops CMake from finding an LLVM that lives outside vcpkg, which is the
  usual setup for this project.
- Unicorn is optional: without a working `pkg-config` it is simply disabled and
  only the `tests/semantic/` round-trip tests are skipped. Z3, by contrast, is
  required whenever `DRAGON_USE_Z3=ON`.

- **Always build the `Release` configuration.** The prebuilt
  `clang+llvm-*-windows-msvc` package ships only Release libraries, which use
  the dynamic release CRT (`/MD`). The Visual Studio generator defaults to
  `Debug` (`/MDd`) when no `--config` is given, and linking Debug project code
  against Release LLVM fails with hundreds of `LNK2038` runtime-library /
  `_ITERATOR_DEBUG_LEVEL` mismatches ending in `LNK1319`. Pass
  `--config Release` (or use `RelWithDebInfo`, also `/MD`) on every
  `cmake --build`:

  ```powershell
  cmake --build build --config Release -j
  ```

## Structure

```
include/        C++ public headers (dragon/)
lib/            C++ implementation
c_shim/         C API (dragon_c.h + libdragon_c.so)
python/         Python API (cffi, wraps the C shim)
tool/           CLI tools (disassembler, opcode_audit)
tests/          Catch2 test suite
docs/           API documentation, examples, and the manual
```

The public headers live under `include/dragon/`, grouped by area
(`disassembler/`, `lifters/`, `analysis/`, `loader/`, `sigs/`). See the
[API documentation](#documentation) below for the full surface.

## Documentation

Full examples for every API layer are in [`docs/`](docs/):

- [`docs/cpp_api.md`](docs/cpp_api.md) — C++ API
- [`docs/c_api.md`](docs/c_api.md) — C shim API
- [`docs/python_api.md`](docs/python_api.md) — Python API
- [`IGNIL.md`](IGNIL.md) — IGNIL design and implementation guide

A hands-on walkthrough, organized by task rather than by class, is in
[`docs/manual/`](docs/manual/index.md) — from raw bytes through IGNIL and LLVM
IR to [symbolic execution](docs/manual/08-symbolic-execution.md).

## Quick start

**C++**

```cpp
#include <dragon/disassembler/Disassembler.hpp>

dragon::Configuration config;
dragon::Disassembler dis(dragon::Architecture::X86_64, config);

auto instructions = dis.disassemble(bytes, base_address);
for (const auto& insn : instructions)
    std::cout << std::hex << insn.address() << "  " << insn.disassembly() << "\n";
```

**Python**

```python
import sys
sys.path.insert(0, 'python')
import dragon

cfg = dragon.Configuration()
dis = dragon.Disassembler(dragon.Architecture.X86_64, cfg)
graph = dis.disassemble(b'\x90\xC3', base_address=0x1000)
for insn in graph.instructions():
    print(hex(insn.address), insn.disassembly)
```

**Backward slicing (C++)**

```cpp
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

// graph must be populated by Disassembler::disassemble(bytes, base_addr, graph)
// so IGNIL blocks exist
dragon::BackwardSlicer slicer;
auto expr = slicer.slice(dragon::x86::ZF, block_addr, graph);
std::cout << dragon::slice::toString(expr) << "\n";
// e.g. EQ(SUB(RAX:i64, RBX:i64), 0x0:i64)
```

**Backward slicing (Python)**

```python
with dragon.BackwardSlicer() as slicer:
    with slicer.slice_reg(graph, block_addr, *dragon.x86.ZF) as expr:
        print(expr.to_string())   # EQ(SUB(RAX:i64, RBX:i64), 0x0:i64)
```

**ARM64 works the same way** — swap `Architecture::X86_64` for `Architecture::ARM64`
and `dragon::x86` / `dragon.x86` for `dragon::arm` / `dragon.arm`:

```cpp
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>

auto expr = slicer.slice(dragon::arm::ZF, block_addr, graph);
// e.g. cmp w0, #5  ->  EQ(SUB(W0:i32, 0x5:i32), 0x0:i32)
```

**Loading a binary (C++)** — no need to extract a `.text` blob or name the
architecture; both come from the file:

```cpp
#include <dragon/loader/ImageDisassembler.hpp>
#include <dragon/loader/Loader.hpp>

auto image = dragon::loader::LoadedImage::loadFile("a.out");
if (image) {
    // architecture is detected from the ELF/PE header
    dragon::Graph graph = dragon::loader::disassembleImage(*image);
    std::cout << "entry " << std::hex << *image->entryPoint()
              << ", " << graph.size() << " instructions\n";
}
```

**Loading a binary (Python)**

```python
img = dragon.LoadedImage.load_file('a.out')
print(hex(img.entry), 'arch', img.architecture)
for s in img.sections():
    print(s)
graph = img.disassemble()
print(len(graph), 'instructions')
```

## License

dragon-tales is released under the [MIT License](LICENSE) — free to use, modify
and redistribute, including in commercial and closed-source work. The only
condition is that the copyright and license notice be retained in copies, so
please keep the credit to the project.
