#include <dragon/analysis/ValueEnumerator.hpp>

#include <algorithm>

#include <dragon/analysis/SliceExprZ3.hpp>

std::vector<std::uint64_t>
dragon::slice::enumeratedValues(const Expr &expr, z3::context &ctx,
                                const std::vector<z3::expr> &constraints,
                                const EnumeratorConfig &cfg) {
  // Fast path: constant expression doesn't need a solver
  if (const auto *c = std::get_if<Const>(&expr.data))
    return {c->value};

  // We need the lowerer for transforming IGNIL -> Z3 expressions
  const SliceExprZ3 lowerer{ctx};
  // Obtain all the expressions from lowering
  const z3::expr formula = lowerer.lower(expr);
  // Lower based in context
  z3::solver solver{ctx};

  // Add all the constraints, they can be provided by the user
  // or finally they will be retrieved from the expressions.
  for (const auto &c: constraints) {
    solver.add(c);
  }

  std::vector<std::uint64_t> results;
  // Obtain as many values as given by the user in the configuration
  while (results.size() < cfg.maxValues) {
    if (solver.check() != z3::sat)
      break;
    z3::model m = solver.get_model();
    // Evaluate the expression in the current model.
    z3::expr val = m.eval(formula, /*model_completion*/ true);
    std::uint64_t concrete = val.get_numeral_uint64();
    results.push_back(concrete);
    // Block this solution so next call finds a different one
    // we create a new constraint.
    solver.add(formula != ctx.bv_val(concrete, formula.get_sort().bv_size()));
  }

  if (cfg.requireComplete && solver.check() != z3::unsat)
    return {};

  if (cfg.sorted)
    std::sort(results.begin(), results.end());

  return results;
}
std::optional<std::uint64_t>
dragon::slice::solveUnique(const Expr &expr, z3::context &ctx,
                           const std::vector<z3::expr> &constraints) {
  EnumeratorConfig config{2};
  config.requireComplete = true;
  if (auto result = enumeratedValues(expr, ctx, constraints, config);
      result.size() == 1)
    return result[0];
  return std::nullopt;
}
