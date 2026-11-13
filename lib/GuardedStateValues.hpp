#pragma once
#include "GuardedTransitions.hpp"
namespace dragon::llvmlifter::detail {
// Explicit, untrusted state/target predictions. A committed rewrite always
// retains state-mismatch and same-read target-mismatch fallbacks.
GuardedTransitionsStats runGuardedStateValues(llvm::Module &,
    const GuardedTransitionsOptions & = {});
}
