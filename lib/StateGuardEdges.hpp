#pragma once

#include <cstddef>

namespace llvm {
class BranchInst;
class DominatorTree;
}

namespace dragon::llvmlifter {
struct DispatcherSpecializationOptions;
struct DispatcherSpecializationStats;

namespace detail {

// Shared by all functions in one invocation. The limits are private: no
// public option or ABI change is needed to keep cloning and SSA repair bounded.
struct StateGuardEdgeBudget {
  std::size_t edgeProofs = 0;
  std::size_t expressionNodes = 0;
  std::size_t clones = 0;
  std::size_t clonedInstructions = 0;
  std::size_t repairCells = 0;
  bool stopped = false;
  bool reported = false;
};

bool specializeStateGuardEdge(
    llvm::BranchInst &, llvm::DominatorTree &,
    const DispatcherSpecializationOptions &,
    DispatcherSpecializationStats &, StateGuardEdgeBudget &,
    bool allowPlainStores);

} // namespace detail
} // namespace dragon::llvmlifter
