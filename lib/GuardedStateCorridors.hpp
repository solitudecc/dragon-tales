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
struct GuardedCheckedCorridorsStats {
  GuardedPartialCorridorsStats partial;
  std::size_t loopContexts=0,loopBackedges=0;
};
// Continue only pre-existing generated seeds. Fresh root PHIs carry every
// actual selected loop slot; all original controllers retain their behavior.
GuardedCheckedCorridorsStats runGuardedCheckedCorridors(llvm::Module &,
    const GuardedTransitionsOptions & = {});
// Separate opt-in result-bit-pattern hints for bounded integer right slices.
// Ordinary original seeds are eligible as well as existing generated seeds.
GuardedCheckedCorridorsStats runGuardedBitSlices(llvm::Module &,
    const GuardedTransitionsOptions & = {});
// Independent dragon.guard-predictions hints. Typed Guest STORE values may
// predict one unknown Guest LOAD atom inside a table-address expression.
// Actual reads, operations, flags and full fallbacks are never substituted.
// Scalar fshl/fshr may remain in an already executed seed; copied paths are
// still call-free. Their results supply no value or definedness facts.
GuardedCheckedCorridorsStats runGuardedStoreExpressions(llvm::Module &,
    const GuardedTransitionsOptions & = {});
}
