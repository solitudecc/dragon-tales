#pragma once

#include <cstddef>

namespace llvm {
class Module;
}

namespace dragon::llvmlifter {

/// Bounds for the opt-in, proof-only flattened-dispatch rewrite.
///
/// The pass rewrites predecessor edges whose state value is proven to be one
/// concrete switch value, or a bounded finite set whose values all select the
/// same destination. A missing/unknown value, a PHI-bearing target, or a
/// dispatcher with side effects leaves that dispatcher unchanged.
struct DeflatteningOptions {
  std::size_t maxDispatchers = 64;
  std::size_t maxIncomingEdges = 512;
  std::size_t maxExpressionNodes = 128;
  std::size_t maxValues = 16;
  /// Consume complete, proven cross-call state facts when a load-backed
  /// dispatcher is entered through another lifted function. This includes
  /// CallEffectSummary metadata and a directly dominating constant GuestState
  /// store; missing, malformed, incomplete or ambiguous facts remain explicit.
  bool consumeCallEffectSummaries = true;
  std::size_t maxCrossCallSites = 256;
};

struct DeflatteningStats {
  bool available = false;
  bool complete = true;
  bool changed = false;
  std::size_t dispatchersVisited = 0;
  std::size_t dispatchersRewritten = 0;
  std::size_t edgesRewritten = 0;
  std::size_t crossCallDispatchersRewritten = 0;
  std::size_t crossCallSummariesConsumed = 0;
  std::size_t crossCallDirectStoresConsumed = 0;
  std::size_t unresolvedDispatchers = 0;
  std::size_t budgetExhausted = 0;
};

DeflatteningStats runProvableDeflattening(
    llvm::Module &, const DeflatteningOptions & = {});

} // namespace dragon::llvmlifter
