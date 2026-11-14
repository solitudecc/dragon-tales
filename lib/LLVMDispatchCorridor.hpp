#pragma once

#include <dragon/lifters/llvm/DispatchCorridor.hpp>

#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
namespace dragon::llvmlifter::detail {
// Validation-only fault control. 1 fails after cloning the carrier; 2 fails
// after body publication. Both must restore the entire original module.
DispatchCorridorStats threadDispatchCorridorCommitFailureForTest(
    llvm::Function &, const std::vector<DispatchCorridorEdge> &,
    unsigned failureStage, const DispatchCorridorOptions & = {});
} // namespace dragon::llvmlifter::detail
#endif
