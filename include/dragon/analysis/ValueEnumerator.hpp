#pragma once

#include <dragon/analysis/SliceExpr.hpp>

#include <functional>
#include <vector>
#include <z3++.h>

namespace dragon::slice {

/// Used to configure the maximum number of solutions
struct EnumeratorConfig {
  std::size_t maxValues = 256; ///< Abort after this many solutions
  bool sorted = true; ///< Sort results before returning
  bool requireComplete = false; ///< Return no values unless exhaustion is proved
};

/// Collect concrete 64-bit values that `expr` can take given
/// the extra `constraints` (already expressed as Z3 assertions)
///
/// Each call to Z3 produces one model, then adds a blocking clause
/// (expr != found_value) before asking for the next. The loop ends
/// when the solver returns UNSAT or maxValues is reached.
/// With requireComplete, reaching the limit or UNKNOWN returns an empty vector
/// unless a final UNSAT check proves there are no remaining values.
///
/// Returns an empty vector when unsatisfiable, or when requireComplete cannot
/// be established. A Const returns its one value directly without a solver.
[[nodiscard]] std::vector<std::uint64_t>
enumeratedValues(const Expr &expr, z3::context &ctx,
                 const std::vector<z3::expr> &constraints = {},
                 const EnumeratorConfig &cfg = {});

/// Convenience: solve for a single concrete value.
/// Returns nullopt if unsatisfiable or if the expression is not
/// uniquely determined, including solver UNKNOWN.
[[nodiscard]] std::optional<std::uint64_t>
solveUnique(const Expr &expr, z3::context &ctx,
            const std::vector<z3::expr> &constraints = {});

} // namespace dragon::slice
