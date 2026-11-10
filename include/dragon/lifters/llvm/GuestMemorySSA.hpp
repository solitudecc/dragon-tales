#pragma once

#include <cstddef>

namespace llvm {
class Module;
}

namespace dragon::llvmlifter {

/// Bounds for the conservative S2 Guest-memory forwarding pass.
/// Acyclic CFG edges may carry facts; calls, unknown aliases and natural-loop
/// edges are barriers. Conflicting join values may use PHIs when enabled.
struct MemoryForwardOptions {
  std::size_t maxRewrites = 65536;
  bool runMem2Reg = true;
  bool enableCrossBlock = true;
  bool enablePhi = true;
};

struct MemoryForwardStats {
  bool available = false;
  bool complete = true;
  bool changed = false;
  std::size_t blocksVisited = 0;
  std::size_t guestLoads = 0;
  std::size_t guestStores = 0;
  std::size_t loadsForwarded = 0;
  std::size_t loadsReused = 0;
  std::size_t callBarriers = 0;
  std::size_t aliasBarriers = 0;
  std::size_t partialOverlaps = 0;
  std::size_t phisInserted = 0;
  std::size_t joinBarriers = 0;
  std::size_t loopBarriers = 0;
  std::size_t budgetExhausted = 0;
};

namespace detail {
MemoryForwardStats runGuestMemorySSA(llvm::Module &, const MemoryForwardOptions &);
}

} // namespace dragon::llvmlifter
