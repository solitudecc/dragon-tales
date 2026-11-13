#pragma once
#include "GuardedTransitions.hpp"
namespace dragon::llvmlifter::detail {
// Finite SSA/address values select predictions only. Actual target LOADs,
// instruction flags, side exits and full fallbacks remain executable.
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &,
    const GuardedTransitionsOptions & = {});
// Opt-in STORE values are untrusted predictions, never forwarded memory facts.
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &,
    const GuardedTransitionsOptions &, bool storePredictions);
// Separate opt-in incomplete predictions. Existing result layouts are unchanged.
struct GuardedPartialCorridorsStats {
  GuardedTransitionsStats base;
  std::size_t partialCandidateEdges=0,partialAttemptedEdges=0;
  std::size_t partialRewrittenEdges=0,partialUnresolvedEdges=0;
};
GuardedPartialCorridorsStats runGuardedPartialCorridors(llvm::Module &,
    const GuardedTransitionsOptions & = {});
// Separate opt-in Cartesian Boolean/zero-mask hints, never source value facts.
GuardedPartialCorridorsStats runGuardedPartialMaskCorridors(llvm::Module &,
    const GuardedTransitionsOptions & = {});
}
