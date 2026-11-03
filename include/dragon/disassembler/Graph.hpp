#pragma once

#include <dragon/Architecture.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/disassembler/Function.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <dragon/lifters/ignil/IGNIL.hpp>

namespace dragon {
namespace detail { struct GraphLLVMMetadata; }

/// A representation of whole disassembled code, Graph will be
/// in charge of function generation. Graph also serves as the
/// container from the instructions, as Functions and Blocks are
/// only views to those instructions.
class Graph {
  public:
  Graph(Architecture architecture, Configuration config);
  ~Graph();

  Graph(Graph &&) noexcept;
  Graph &operator=(Graph &&) noexcept;

  Graph(const Graph &) = delete;
  Graph &operator=(const Graph &) = delete;

  [[nodiscard]] Architecture architecture() const { return architecture_; }
  [[nodiscard]] const Configuration &configuration() const {
    return configuration_;
  }

  void addInstruction(Instruction insn);
  void addBlock(std::uint64_t addr);
  void addFunction(std::uint64_t addr);
  void update(const Instruction &insn);

  void addIGNILBlock(ignil::Block block);
  ignil::Block *getIGNILBlock(std::uint64_t addr);
  [[nodiscard]] const ignil::Block *getIGNILBlock(std::uint64_t addr) const;

  /// Retrieve a pointer to an instruction given its address
  /// in case there's no instruction in that address, returns null
  [[nodiscard]] const Instruction *get(std::uint64_t address) const;
  /// Retrieve a pointer to an instruction given its address
  /// in case there's no instruction in that address, returns null
  [[nodiscard]] Instruction *get(std::uint64_t address);

  /// Retrieve the list of instructions together with its address
  [[nodiscard]] const std::map<std::uint64_t, Instruction> &listing() const {
    return listing_;
  }

  // View accessors - iterate valid sets, construct views on the fly

  /// Retrieve the pointers to all instructions in a vector.
  [[nodiscard]] std::vector<Instruction *> instructions();
  /// Retrieve BasicBlocks, as the basic blocks are simply views
  /// to the instructions of them, we return objects, not pointers.
  [[nodiscard]] std::vector<BasicBlock> blocks() const;
  /// Retrieve Functions, as the functions are simply views
  /// to the instructions of them, we return objects, not pointers.
  [[nodiscard]] std::vector<Function> functions() const;

  [[nodiscard]] std::size_t size() const { return listing_.size(); }
  [[nodiscard]] bool empty() const { return listing_.empty(); }

  void merge(Graph other);

  // -- Query helpers used by BasicBlock, Function, and analysis passes --------

  [[nodiscard]] bool isValidBlock(std::uint64_t addr) const;
  [[nodiscard]] bool isInvalidBlock(std::uint64_t addr) const;
  [[nodiscard]] bool isValidFunction(std::uint64_t addr) const;

  /// Addresses of all blocks that were successfully disassembled.
  [[nodiscard]] const std::set<std::uint64_t> &validBlockAddresses() const;
  /// Addresses of all functions that were successfully identified.
  [[nodiscard]] const std::set<std::uint64_t> &validFunctionAddresses() const;

  private:
  friend struct detail::GraphLLVMMetadata;
  void insert(Instruction insn);

  struct Queues;

  Architecture architecture_;
  Configuration configuration_;
  std::map<std::uint64_t, Instruction> listing_;
  std::map<std::uint64_t, ignil::Block> ignilBlocks_;
  std::unique_ptr<Queues> queues_;
};

} // namespace dragon
