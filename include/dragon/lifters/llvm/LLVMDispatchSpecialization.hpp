#pragma once

#include <cstddef>
#include <cstdint>

namespace llvm {
class Module;
}

namespace dragon::llvmlifter {

/// Bounds for the opt-in, proof-only dispatcher specialization pass. The pass
/// rewrites only conditions that are uniquely constant in the current IR;
/// finite but differing targets remain candidates and are never guessed.
struct DispatcherSpecializationOptions {
  std::size_t maxConditions = 256;
  std::size_t maxExpressionNodes = 128;
};

struct DispatcherSpecializationStats {
  bool available = false;
  bool complete = true;
  bool changed = false;
  std::size_t conditionsVisited = 0;
  std::size_t uniqueConditions = 0;
  std::size_t switchesVisited = 0;
  std::size_t branchesRewritten = 0;
  std::size_t switchesRewritten = 0;
  std::size_t unresolvedSwitches = 0;
  std::size_t budgetExhausted = 0;
};

DispatcherSpecializationStats runDispatcherSpecialization(
    llvm::Module &, const DispatcherSpecializationOptions & = {});

} // namespace dragon::llvmlifter
