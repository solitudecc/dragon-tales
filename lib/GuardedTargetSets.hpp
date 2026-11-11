#pragma once
#include "GuardedTransitions.hpp"
namespace dragon::llvmlifter::detail {
// Read only dragon.indirect-target-hints. Never consumes or adds entry facts.
// Reuses the established module-budget/result layout. Each committed edge adds
// one small fast switch and one same-value full fallback switch.
GuardedTransitionsStats runGuardedTargetSets(llvm::Module &,
    const GuardedTransitionsOptions & = {});
}
