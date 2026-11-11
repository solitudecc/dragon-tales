#pragma once

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class BasicBlock;
class Instruction;
class DataLayout;
class Function;
class LoadInst;
class Value;
}

namespace dragon::llvmlifter::detail {

// Frozen before the first checked-edge mutation in a function. Identities
// refer only to original instructions/edges; none of the clones enter here.
struct CheckedMemoryOriginalBlock {
  llvm::Instruction *terminator = nullptr;
  std::vector<llvm::BasicBlock *> successors;
};
struct CheckedMemoryOriginalFunction {
  llvm::Function *function = nullptr;
  std::unordered_set<llvm::Value *> memoryBases;
  std::vector<llvm::LoadInst *> loads;
  std::unordered_map<llvm::BasicBlock *, CheckedMemoryOriginalBlock> blocks;
};

struct CheckedMemoryEdgeBudget {
  std::size_t candidates = 0;
  std::size_t edgeProofs = 0;
  std::size_t indexNodes = 0;
  std::size_t clones = 0;
  std::size_t clonedInstructions = 0;
  std::size_t repairCells = 0;
  bool stopped = false;
  bool reported = false;
  bool enableWorklist = false;
  std::vector<CheckedMemoryOriginalFunction> originals;
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

// Optional continuation after all module-wide legacy phases. A successful
// original LOAD rotates to the queue tail; each rewrite consumes a distinct
// frozen original edge and all existing budgets remain shared.
void forwardCheckedMemoryWorklist(
    const llvm::DataLayout &, const MemoryForwardOptions &,
    MemoryForwardStats &, CheckedMemoryEdgeBudget &);

} // namespace dragon::llvmlifter::detail
