#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <dragon/Architecture.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <functional>
#include <span>
#include <vector>
#include <dragon/disassembler/Instruction.hpp>

namespace dragon {
namespace analysis {class IndirectTargetResolver;}

/// A disassembler for various architectures supported by LLVM
/// the disassembler will work as a simple tool, so we can provide
/// different inputs, which are mostly an array of bytes, and the
/// base address to take into account for the disassembly process.
/// Internally, it will use different disassembly algorithms for
/// doing the processing of the disassembly of the instructions.
///
/// At the moment we provide a Graph, the disassembler can make
/// use of this graph class to generate functions from the disassembled
/// code. At that moment, once we call its disassemble function
/// the Graph will be populated with the data, and the user can
/// make use of it. Also, the user can ask to disassemble the data
/// and retrieve a function from an address.
class Disassembler {
  public:
  /// Constructor for the disassembler, we need to provide an
  /// architecture that will internally be used to disassemble
  /// the bytes using LLVM disassembler. The configuration will
  /// contain optional values for the output format.
  Disassembler(Architecture architecture, const Configuration &configuration);
  ~Disassembler();

  Disassembler(const Disassembler &) = delete;
  Disassembler &operator=(const Disassembler &) = delete;

  Disassembler(Disassembler &&) noexcept;
  Disassembler &operator=(Disassembler &&) noexcept = default;

  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] Architecture architecture() const noexcept {
    return architecture_;
  }
  [[nodiscard]] const Configuration &configuration() const noexcept {
    return configuration_;
  }

  /// disassemble will receive as parameters a list of bytes that
  /// represent the instructions from code, or a .text section, etc.
  /// baseAddress is used to generate the instructions and to take
  /// into account things like jumps, and calls, etc.
  [[nodiscard]] std::vector<Instruction>
  disassemble(std::span<const std::uint8_t> bytes,
              std::uint64_t baseAddress) const;

  /// disassemble receives a list of bytes to apply the disassembly
  /// process, but this time, taking the base address, a disassembly
  /// process is applied, and basic block and functions are generated,
  /// all these values are stored into the Graph object.
  void disassemble(std::span<const std::uint8_t> bytes,
                   std::uint64_t baseAddress, Graph &graph) const;

  /// Extra knowledge about the code being analysed, for the Graph overload
  /// below. Everything is optional.
  struct AnalysisHints {
    /// Addresses known to be code entry points - an image's entry point, its
    /// function symbols and exports. Each becomes a function start and a
    /// block start; with RECURSIVE_TRAVERSAL each is also a root the
    /// traversal starts from. Addresses outside `bytes` are ignored. When
    /// empty, `baseAddress` is the single entry point (the behaviour of the
    /// plain overload).
    std::vector<std::uint64_t> entryPoints;
    /// Reads `out.size()` bytes at virtual address `address` from outside
    /// `bytes` (e.g. a jump table in .rodata/.rdata while `bytes` is .text).
    /// Returns false if the range is not mapped.
    std::function<bool(std::uint64_t address, std::span<std::uint8_t> out)>
            readMemory;
    /// Optional, trusted target analysis. Enables bounded monotone candidate
    /// growth with full closure revalidation; default discovery is unchanged.
    /// The shared object is used synchronously and never retained by Graph.
    std::shared_ptr<analysis::IndirectTargetResolver> indirectTargets;
  };

  /// As above, with entry points and a memory reader for the analysis.
  void disassemble(std::span<const std::uint8_t> bytes,
                   std::uint64_t baseAddress, Graph &graph,
                   const AnalysisHints &hints) const;

  /// Decode `bytes` with the configured strategy. For RECURSIVE_TRAVERSAL
  /// the traversal starts from every address in `roots` (those inside
  /// `bytes`), or from `baseAddress` when none is; LINEAR_SWEEP ignores
  /// `roots`.
  [[nodiscard]] std::vector<Instruction>
  disassemble(std::span<const std::uint8_t> bytes, std::uint64_t baseAddress,
              std::span<const std::uint64_t> roots) const;

  /// Decode instructions starting at startAddress until a basic-block
  /// terminator (jump, return, or trap). Returns only the instructions
  /// belonging to that one block.
  [[nodiscard]] std::vector<Instruction>
  disassembleBlock(std::span<const std::uint8_t> bytes,
                   std::uint64_t baseAddress, std::uint64_t startAddress) const;

  /// Lifts an arbitrary linear sequence of already-decoded instructions
  /// directly to IGNIL - e.g. a captured execution trace (DBI/emulator
  /// stepping, Unicorn, ...) that isn't claimed to form a single valid
  /// basic block or a coherent CFG. Bypasses CFG/function/block-validity
  /// analysis entirely: each instruction is lifted independently and the
  /// results concatenated in sequence order, with no synthesized
  /// fall-through terminator and no successor computation - unlike
  /// disassemble(..., Graph&), this never touches a Graph.
  ///
  /// `trace` is typically the result of disassemble(bytes, baseAddress)
  /// (the Graph-free overload above, which is itself already a pure
  /// linear decode with no CFG involvement) or of stitching together
  /// individually-decoded instructions from real, possibly non-contiguous
  /// execution addresses.
  ///
  /// Returns an empty ignil::Block for RISC-V (no IGNIL lifter exists yet)
  /// or an empty trace.
  [[nodiscard]] ignil::Block liftTrace(std::span<const Instruction> trace) const;

  private:
  struct Impl;

  // Use the LLVM internals for disassembling one instruction
  Instruction decodeOne(std::span<const std::uint8_t> bytes,
                        std::uint64_t address, std::size_t &offset) const;

  [[nodiscard]] std::vector<Instruction>
  linearSweep(std::span<const std::uint8_t> bytes,
              std::uint64_t baseAddress) const;

  [[nodiscard]] std::vector<Instruction>
  recursiveTraversal(std::span<const std::uint8_t> bytes,
                     std::uint64_t baseAddress,
                     std::span<const std::uint64_t> roots) const;

  Architecture architecture_;
  Configuration configuration_;
  std::unique_ptr<Impl> impl_;
};

} // namespace dragon
