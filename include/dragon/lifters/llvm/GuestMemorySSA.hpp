#pragma once

#include <cstddef>
#include <cstdint>

namespace llvm {
class Module;
}

namespace dragon {
class Graph;
}

namespace dragon::llvmlifter {

/// Bounds for the conservative S2 Guest-memory forwarding pass.
/// Acyclic CFG edges may carry facts; calls and unknown aliases are barriers.
/// Loop edges require an exact invariant check; a canonical two-block loop may
/// use a PHI for a loop-carried value, and other loops remain barriers.
/// Conflicting join values may use PHIs when enabled. Little-endian constant
/// GEP ranges may also be rebuilt byte-wise after the fact path is proven.
struct MemoryForwardOptions {
  std::size_t maxRewrites = 65536;
  bool runMem2Reg = true;
  bool enableCrossBlock = true;
  bool enablePhi = true;
  // Prove only explicitly annotated static internal calls.  The default
  // remains a CALL barrier; no symbol/import-name heuristics are used.
  bool proveLeafCalls = false;
  std::uint32_t maxCallBlocks = 32;
  std::uint32_t maxCallOperations = 4096;
  std::uint32_t maxCallFacts = 8192;
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
  std::size_t byteRebuilds = 0;
  std::size_t leafCallsPreserved = 0;
};

namespace detail {
// Keep the original two-argument symbol for source and binary compatibility;
// the graph-aware overload is an explicit opt-in contract.
MemoryForwardStats runGuestMemorySSA(llvm::Module &, const MemoryForwardOptions &);
MemoryForwardStats runGuestMemorySSA(llvm::Module &, const MemoryForwardOptions &,
                                     const Graph *graph);
}

} // namespace dragon::llvmlifter
