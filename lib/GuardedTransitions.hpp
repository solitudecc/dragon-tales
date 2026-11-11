#pragma once
#include <cstddef>
#include <string>
namespace llvm { class Module; }
namespace dragon::llvmlifter::detail {
struct GuardedTransitionsOptions {
  std::size_t maxCandidates = 64, maxRewrites = 32;
  // Cumulative module costs; each transaction additionally obeys core caps.
  std::size_t maxBlocks = 128, maxInstructions = 16384;
};
struct GuardedTransitionsStats {
  bool available = false, complete = true, changed = false;
  std::size_t candidateEdges = 0, attemptedEdges = 0, rewrittenEdges = 0;
  std::size_t unresolvedEdges = 0, rejected = 0, budgetExhausted = 0;
  std::size_t clonedBlocks = 0, clonedInstructions = 0;
  // Distinct initially hint-matched switch blocks (including selected dynamic
  // business terminals when enabled) still present at return,
  // including those left untouched by rejection/budget; not all module switches.
  std::size_t originalSwitchesRetained = 0, fallbackSwitchesCreated = 0;
  std::string message;
};
// Manifest bytes are candidate hints only. Every successful transition compares
// a newly executed LOAD with that hint and keeps the same-value full fallback.
GuardedTransitionsStats runGuardedTransitions(llvm::Module &,
    const GuardedTransitionsOptions & = {});
// Optional original business-STORE corridors. STORE/table association selects
// only a prediction; the core retains every actual LOAD and full fallback.
// Old overload/options/stats layout and default scheduling are unchanged.
GuardedTransitionsStats runGuardedTransitions(llvm::Module &,
    const GuardedTransitionsOptions &, bool businessCorridors);
}
