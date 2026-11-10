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
/// same destination. A missing/unknown value or PHI-bearing target keeps its
/// incoming edge through the original dispatcher; independent proved edges
/// may still be threaded. A dispatcher with side effects or live-out values
/// remains unchanged.
/// A select with a proved defined condition may reuse that exact condition to
/// branch between two independently proved targets. Memory selectors require
/// a same-predecessor reaching store; unknown writes and synchronizing accesses
/// stop that backward scan, without removing or moving any prefix operation.
struct DeflatteningOptions {
  std::size_t maxDispatchers = 64;
  std::size_t maxIncomingEdges = 512;
  std::size_t maxExpressionNodes = 128; ///< Shared scan/value-proof budget per edge.
  std::size_t maxValues = 16;
  /// Retained for source compatibility. Cross-call entry specialization is
  /// disabled: call effects are postconditions, not callee-entry proofs.
  /// A separate pass must establish a sound entry context before using them.
  bool consumeCallEffectSummaries = false;
  std::size_t maxCrossCallSites = 256;
};

struct DeflatteningStats {
  bool available = false;
  bool complete = true;
  bool changed = false;
  std::size_t dispatchersVisited = 0;
  std::size_t dispatchersRewritten = 0;
  std::size_t edgesRewritten = 0;
  // Reserved compatibility counters; the same-function pass leaves them zero.
  std::size_t crossCallDispatchersRewritten = 0;
  std::size_t crossCallSummariesConsumed = 0;
  std::size_t crossCallDirectStoresConsumed = 0;
  /// Includes partially rewritten dispatchers with unproved incoming edges.
  std::size_t unresolvedDispatchers = 0;
  std::size_t budgetExhausted = 0;
};

DeflatteningStats runProvableDeflattening(
    llvm::Module &, const DeflatteningOptions & = {});

} // namespace dragon::llvmlifter
