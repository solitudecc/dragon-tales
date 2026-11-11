#pragma once

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <unordered_set>

namespace llvm {
class DataLayout;
class Function;
class Value;
}

namespace dragon::llvmlifter::detail {

struct CheckedMemoryEdgeBudget {
  std::size_t candidates = 0;
  std::size_t edgeProofs = 0;
  std::size_t indexNodes = 0;
  std::size_t clones = 0;
  std::size_t clonedInstructions = 0;
  std::size_t repairCells = 0;
  bool stopped = false;
  bool reported = false;
};

// The ordinary memory worklists must finish before this CFG-mutating fallback.
// It owns/rebuilds its dominator tree and never exports facts to those worklists.
void forwardCheckedMemoryEdges(
    llvm::Function &, const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &);

} // namespace dragon::llvmlifter::detail
