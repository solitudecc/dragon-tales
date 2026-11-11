#pragma once
#include "GuardedTransitions.hpp"
namespace dragon::llvmlifter::detail {
// Finite SSA/address values select predictions only. Actual target LOADs,
// instruction flags, side exits and full fallbacks remain executable.
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &,
    const GuardedTransitionsOptions & = {});
}
