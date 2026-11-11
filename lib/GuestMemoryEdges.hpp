#pragma once

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <unordered_set>
#include <vector>

namespace llvm {
class DataLayout;
class Function;
class LoadInst;
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

// The module pass first attempts all legacy direct paths, then consumes only
// these still-live original LOADs for new hubs. Edge copies do not erase their
// originals, and no later function can erase another function's instructions.
void forwardCheckedMemoryEdges(
    llvm::Function &, const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &,
    std::vector<llvm::LoadInst *> &pendingHubs);
void forwardCheckedMemoryHubs(
    llvm::Function &, const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &,
    const std::vector<llvm::LoadInst *> &pendingHubs);


// Queue only originals not copied by either legacy phase. The module pass
// consumes this queue after every function's legacy hub search has completed.
void forwardCheckedMemoryHubs(
    llvm::Function &, const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &,
    const std::vector<llvm::LoadInst *> &pendingHubs,
    std::vector<llvm::LoadInst *> &pendingPointers);
void forwardCheckedMemoryPointers(
    llvm::Function &, const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &,
    const std::vector<llvm::LoadInst *> &pendingPointers);

} // namespace dragon::llvmlifter::detail
