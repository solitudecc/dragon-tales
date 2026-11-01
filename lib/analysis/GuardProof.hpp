#pragma once
#include <dragon/analysis/SliceExpr.hpp>

namespace dragon::slice {
// A candidate count is accepted only by an exhaustive range proof or SMT.
bool proveGuardBound(const Expr &condition, const Expr &index,
                     const Expr &core, unsigned indexBits,
                     std::uint32_t cases, bool dispatchWhenTrue);
}
