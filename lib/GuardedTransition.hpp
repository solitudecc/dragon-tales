#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace llvm { class BasicBlock; class ConstantInt; class Function; }
namespace dragon::llvmlifter::detail {
// Current-IR pointers are supplied explicitly, never a proof certificate.
struct GuardedTransitionRequest {
  llvm::BasicBlock *seed = nullptr;
  unsigned seedSuccessor = 0;
  llvm::BasicBlock *seedTarget = nullptr;
  std::vector<llvm::BasicBlock *> blocks;
  // Exactly blocks.size()-1 actual successor-slot indices.
  std::vector<unsigned> forwardSuccessors;
  llvm::ConstantInt *candidate = nullptr;
};
// Separate request: the existing single-candidate API and its layout remain
// unchanged. Candidates are predictions, never facts about mutable memory.
struct GuardedTransitionSetRequest {
  llvm::BasicBlock *seed = nullptr;
  unsigned seedSuccessor = 0;
  llvm::BasicBlock *seedTarget = nullptr;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<unsigned> forwardSuccessors;
  std::vector<llvm::ConstantInt *> candidates;
};
struct GuardedTransitionOptions {
  // Independent budgets, including guard/fallback, snapshots and SSA repair.
  std::size_t maxBlocks = 32;
  std::size_t maxInstructions = 2048;
};
struct GuardedTransitionResult {
  bool changed = false;
  bool complete = true;
  bool rejected = false;
  bool budgetExhausted = false;
  std::size_t clonedBlocks = 0;
  // Original copied instructions (including selected-away PHIs), plus new
  // guard/snapshot/repair instructions. Conservative cost, not net growth.
  std::size_t clonedInstructions = 0;
  std::size_t snapshotPhis = 0;
  std::size_t repairPhis = 0;
  std::size_t originalExitEdges = 0;
  std::string message;
};
// Preserve accesses/side exits. Compare the actual terminal LOAD with candidate;
// success goes to an existing case, failure uses the SAME value/full switch.
// Failure leaves source unchanged. Success preserves Module/Function/Arguments,
// but invalidates body pointers. Original blocks remain an unchanged-order
// prefix; new blocks are appended. No cleanup deletes/reorders original blocks.
// A caller may restore frozen block ordinals after a successful call, then MUST
// revalidate each actual edge. Intervening LLVM cleanup voids that ordinal rule.
// This helper does not prove state/table facts.
GuardedTransitionResult cloneGuardedTransition(
    llvm::Function &, const GuardedTransitionRequest &,
    const GuardedTransitionOptions & = {});
// A small switch on the SAME actual LOAD selects known cases; its default
// retains the full original switch/default. At most 16 distinct candidates.
GuardedTransitionResult cloneGuardedTransitionSet(
    llvm::Function &, const GuardedTransitionSetRequest &,
    const GuardedTransitionOptions & = {});
}
