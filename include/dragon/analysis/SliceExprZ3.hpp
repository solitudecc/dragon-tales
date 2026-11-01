#pragma once

#include <dragon/analysis/SliceExpr.hpp>
#include <z3++.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dragon::slice {

/// Lowers a SliceExpr tree into a Z3 bitvector expression.
///
/// SymReg nodes become free Z3 bitvector constants. The same SymReg
/// (same offset + width) always produces the same Z3 symbol within one
/// lowerer instance, so constraints can be shared across multiple lower()
/// calls in a single solver query.
///
/// Load nodes share a symbol only for the same producing operation or a
/// proven unchanged memory snapshot and address. Unidentified reads are fresh.
///
/// Call reset() between independent solver queries to clear the cache.
class SliceExprZ3 {
  public:
  explicit SliceExprZ3(z3::context &ctx);

  /// Lower a SliceExpr into a Z3 bitvector expression.
  /// The width of the returned expression matches the width of the root node.
  [[nodiscard]] z3::expr lower(const Expr &e) const;

  /// Clear the symbol cache between independent queries.
  void reset() const;

  private:
  /// Return the cached Z3 symbol for a SymReg (creates it on first call).
  [[nodiscard]] z3::expr symFor(const SymReg &r) const;

  z3::context &ctx_;
  mutable std::unordered_map<std::string, z3::expr> symCache_;
  mutable std::vector<std::pair<Load, z3::expr>> loadCache_;
  // Names handed to Undefs of unknown origin: each gets a fresh variable.
  // Never reset, so a name is not reused within the context.
  mutable std::uint64_t anonymousUndefs_ = 0;
};

} // namespace dragon::slice
