#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/ValueSetAccumulator.hpp>

using namespace dragon::analysis;

TEST_CASE("finite value accumulator grows monotonically and normalizes",
          "[analysis][value-set]") {
  FiniteValueAccumulator values{8, 4};
  CHECK(values.merge({ValueSetKind::Finite, 8, {2, 1, 2}}));
  CHECK(values.result() == FiniteValues{ValueSetKind::Finite, 8, {1, 2}});
  CHECK(values.merge({ValueSetKind::Finite, 8, {2, 3}}));
  CHECK(values.result() == FiniteValues{ValueSetKind::Finite, 8, {1, 2, 3}});
  CHECK_FALSE(values.merge({ValueSetKind::Unreachable, 8, {}}));
  CHECK(values.result() == FiniteValues{ValueSetKind::Finite, 8, {1, 2, 3}});
}

TEST_CASE("finite value accumulator refuses width and budget guesses",
          "[analysis][value-set]") {
  FiniteValueAccumulator width{8, 4};
  CHECK(width.merge({ValueSetKind::Finite, 16, {1}}));
  CHECK(width.result().kind == ValueSetKind::Unknown);
  CHECK_FALSE(width.complete());

  FiniteValueAccumulator budget{8, 2};
  CHECK(budget.merge({ValueSetKind::Finite, 8, {1, 2}}));
  CHECK(budget.merge({ValueSetKind::Finite, 8, {3}}));
  CHECK(budget.result().kind == ValueSetKind::Unknown);
  CHECK(budget.result().values.empty());
}

TEST_CASE("indirect target accumulator reports path completeness",
          "[analysis][value-set]") {
  IndirectTargetAccumulator targets{4};
  CHECK(targets.mergePath({ValueSetKind::Finite, 64, {0x1000, 0x2000}}));
  CHECK(targets.mergePath({ValueSetKind::Finite, 64, {0x2000, 0x3000}}));
  auto complete = targets.result();
  CHECK(complete.complete);
  CHECK(complete.targets == std::vector<std::uint64_t>{0x1000, 0x2000, 0x3000});

  IndirectTargetAccumulator incomplete{4};
  CHECK(incomplete.mergePath({ValueSetKind::Finite, 64, {0x1000}}, false));
  auto result = incomplete.result();
  CHECK_FALSE(result.complete);
  CHECK(result.targets.empty());
  CHECK(result.reason == "path_incomplete");
}
