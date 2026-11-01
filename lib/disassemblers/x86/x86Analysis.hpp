#pragma once

#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

#include <dragon/Architecture.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include "utils/Logger.hpp"

#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>

namespace dragon {

class Instruction;
class Disassembler;
class Graph;
class X86Info;

namespace x86 {

class X86IGNILLifter;

class X86Analysis {
  public:
  /// Constructor for x86Analysis, we need to understand what architecture
  /// we are working with, the disassembler to retrieve all the instructions
  /// and the configuration for logging.
  X86Analysis(Architecture architecture, const Disassembler &disassembler,
              const Configuration &configuration,
              const llvm::MCInstrInfo &instrInfo,
              const llvm::MCRegisterInfo &regInfo);
  ~X86Analysis();
  /// Populate the graph received with the decoded
  /// instructions, blocks and functions.
  /// @param bytes bytes from instructions to be decoded.
  /// @param baseAddr base address used to decode the instructions.
  /// @param graph graph where data will be populated.
  bool analyze(std::span<const std::uint8_t> bytes, std::uint64_t baseAddr,
               Graph &graph,
               const Disassembler::AnalysisHints &hints) const;

  private:
  /// We apply a simple analysis to retrieve possible block starts,
  /// we start from the first instruction retrieved by the disassembler
  /// looking for jump instructions, the next instructions and the
  /// jump address are marked as start of blocks.
  void discoverBlockStarts(
          std::vector<Instruction> &instructions,
          std::unordered_set<std::uint64_t> &basicBlockStarts) const;

  /// Scan for epilogue sequences and mark the instruction that follows each
  /// one (after skipping up to 16 bytes of nop/trap padding) as a candidate
  /// function and block start.
  void
  discoverEpilogues(std::vector<Instruction> &instructions,
                    std::unordered_set<std::uint64_t> &blockStarts,
                    std::unordered_set<std::uint64_t> &functionStarts) const;

  /// We apply a simple analysis to retrieve the beginning of the functions
  /// we follow calls to look for functions, those points will be marked
  /// as function beginnings and as basic blocks too.
  void discoverFunctionStarts(
          std::vector<Instruction> &instructions,
          std::unordered_set<std::uint64_t> &basicBlockStarts,
          std::unordered_set<std::uint64_t> &functionStarts) const;

  /// Flush all pending block and function start addresses into the graph.
  void commitPending(Graph &graph,
                     const std::unordered_set<std::uint64_t> &blockStarts,
                     const std::unordered_set<std::uint64_t> &fnStarts) const;

  /// Lift IGNIL for every block in the graph that has not been lifted yet.
  void liftPendingBlocks(Graph &graph) const;

  /// Collect candidate jump targets from a backward-slice result.
  /// Returns the constant directly if the slicer resolved it; otherwise
  /// falls back to Z3 enumeration if enabled in the configuration.
  std::vector<std::uint64_t> collectTargets(const slice::Expr &result,
                                            std::uint64_t blockAddr) const;

  /// Disassemble instructions at target and integrate them into the graph,
  /// updating blockStarts and fnStarts with any newly discovered addresses.
  /// Returns true if new instructions were added.
  bool integrateNewBlock(std::uint64_t target,
                         std::span<const std::uint8_t> bytes,
                         std::uint64_t baseAddr, Graph &graph,
                         std::unordered_set<std::uint64_t> &blockStarts,
                         std::unordered_set<std::uint64_t> &fnStarts) const;

  /// Tentatively resolve reachable indirect JMPs to a CFG fixed point.
  /// Recheck every candidate after expansion; discard rejected candidates
  /// and all instructions/blocks discovered through them before publishing.
  void resolveIndirectJumps(Graph &graph, std::span<const std::uint8_t> bytes,
                            std::uint64_t baseAddr,
                            const Disassembler::AnalysisHints &hints,
                            std::unordered_set<std::uint64_t> &blockStarts,
                            std::unordered_set<std::uint64_t> &fnStarts) const;

  Architecture architecture_;
  const Disassembler &disassembler_;
  const Configuration &configuration_;
  const llvm::MCInstrInfo &instrInfo_;
  const llvm::MCRegisterInfo &regInfo_;
  const X86Info *x86Info_;
  std::unique_ptr<X86IGNILLifter> x86IgnilLifter_;
  internal::Logger logger_;
};

} // namespace x86
} // namespace dragon
