#pragma once

#include <cstddef>
#include <cstdint>

namespace llvm {
class Module;
}

namespace dragon::llvmlifter {

/// Bounds for the opt-in, proof-only dispatcher specialization pass. The pass
/// rewrites globally constant conditions, or clones a bounded shared branch
/// block for one incoming edge whose condition is proved constant. The latter
/// keeps the block's computations and repairs live-outs; unknown inputs retain
/// the original branch. A bounded downstream context may retain its unknown
/// checks and target switch. Finite but differing targets are never guessed.
struct DispatcherSpecializationOptions {
  std::size_t maxConditions = 256;
  std::size_t maxExpressionNodes = 128;
};

struct DispatcherSpecializationStats {
  bool available = false;
  bool complete = true;
  bool changed = false;
  std::size_t conditionsVisited = 0;
  /// Includes a condition proved in a selected incoming-edge context.
  std::size_t uniqueConditions = 0;
  std::size_t switchesVisited = 0;
  /// Includes folded branch copies; their original shared branch may remain.
  std::size_t branchesRewritten = 0;
  std::size_t switchesRewritten = 0;
  std::size_t unresolvedSwitches = 0;
  std::size_t budgetExhausted = 0;
};

DispatcherSpecializationStats runDispatcherSpecialization(
    llvm::Module &, const DispatcherSpecializationOptions & = {});

/// Explicitly allow copying simple AS0 i8/i16/i32/i64/i128 STOREs at their
/// original execution point. Literal undef/poison operands remain barriers.
/// Stores and subsequent reads retain their order; this does not infer memory
/// facts or enable STORE-to-LOAD forwarding. The original overload keeps STORE
/// barriers for context copies, without changing the options layout.
DispatcherSpecializationStats runDispatcherSpecialization(
    llvm::Module &, const DispatcherSpecializationOptions &,
    bool allowPlainStores);

} // namespace dragon::llvmlifter
