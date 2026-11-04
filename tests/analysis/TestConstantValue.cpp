#include <catch2/catch_test_macros.hpp>
#include "../../lib/analysis/ConstantValue.hpp"

#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/SliceExprZ3.hpp>
#endif

#include <array>
#include <random>

namespace {
using namespace dragon;
using namespace dragon::slice;
using O = ignil::Opcode;
Expr c(std::uint64_t value, ignil::Width width) { return Expr{Const{value, width}}; }
Expr unary(O op, Expr operand, ignil::Width width = 0) {
  return Expr{UnaryOp{op, std::make_shared<Expr>(std::move(operand)), width}};
}
Expr binary(O op, Expr left, Expr right) {
  return Expr{BinOp{op, std::make_shared<Expr>(std::move(left)),
                   std::make_shared<Expr>(std::move(right))}};
}
} // namespace

TEST_CASE("Constant targets preserve widths and wide intermediates",
          "[constant-target][constant-value]") {
  auto value = detail::constantValue(unary(O::SEXT, c(0x80, 8), 64));
  REQUIRE(value);
  CHECK(value->value == 0xffffffffffffff80ULL);
  CHECK(value->width == 64);
  value = detail::constantValue(unary(O::ZEXT, c(~0ULL, 8), 64));
  REQUIRE(value);
  CHECK(value->value == 255);
  value = detail::constantValue(binary(O::SLT, c(255, 8), c(65535, 16)));
  REQUIRE(value);
  CHECK(value->value == 0); // common i16: 255 < -1 is false
  CHECK(value->width == 1);
  auto wide = binary(O::MUL, unary(O::ZEXT, c(~0ULL, 64), 128),
                             unary(O::ZEXT, c(~0ULL, 64), 128));
  CHECK_FALSE(detail::constantValue(wide)); // never silently truncate a target
  auto high = unary(O::TRUNC, binary(O::SHR, std::move(wide), c(64, 128)), 64);
  value = detail::constantValue(high);
  REQUIRE(value);
  CHECK(value->value == 0xfffffffffffffffeULL);
}

TEST_CASE("Constant targets reject unknowns invalid operations and exhausted trees",
          "[constant-target][constant-value]") {
  CHECK_FALSE(detail::constantValue(c(0, 0)));
  CHECK_FALSE(detail::constantValue(Expr{Undef{64}}));
  CHECK_FALSE(detail::constantValue(Expr{Load{std::make_shared<Expr>(c(0x3000, 64)), 64}}));
  auto reg = Expr{SymReg{ignil::Reg{0, 64, "RAX"}}};
  CHECK_FALSE(detail::constantValue(binary(O::XOR, reg, reg)));
  for (auto op : {O::UDIV, O::SDIV, O::UMOD, O::SMOD})
    CHECK_FALSE(detail::constantValue(binary(op, c(42, 64), c(0, 64))));
  for (auto op : {O::SDIV, O::SMOD})
    CHECK_FALSE(detail::constantValue(binary(op, c(1ULL << 63, 64), c(~0ULL, 64))));
  CHECK_FALSE(detail::constantValue(binary(O::FADD, c(0, 64), c(0, 64))));
  CHECK_FALSE(detail::constantValue(Expr{UnaryOp{O::ZEXT, {}, 64}}));
  CHECK_FALSE(detail::constantValue(Expr{BinOp{O::ADD, {}, {}}}));
  auto deep = c(1, 64);
  for (unsigned i = 0; i < 130; ++i) deep = unary(O::NOT, std::move(deep));
  CHECK_FALSE(detail::constantValue(deep));
  auto tree = std::make_shared<Expr>(c(0, 64));
  for (unsigned i = 0; i < 13; ++i)
    tree = std::make_shared<Expr>(BinOp{O::OR, tree, tree});
  CHECK_FALSE(detail::constantValue(*tree));
}

#ifdef DRAGON_WITH_Z3
TEST_CASE("Constant target arithmetic agrees with the independent Z3 lowerer",
          "[constant-target][constant-value]") {
  z3::context context;
  SliceExprZ3 lowerer{context};
  auto check = [&](const Expr &expr) {
    INFO(slice::toString(expr));
    const auto expected = lowerer.lower(expr).simplify();
    const auto actual = detail::constantValue(expr);
    REQUIRE(actual);
    CHECK(actual->width == expected.get_sort().bv_size());
    CHECK(actual->value == expected.get_numeral_uint64());
  };
  const std::array<ignil::Width, 6> widths{1, 8, 16, 32, 64, 128};
  const std::array<std::uint64_t, 8> edges{0, 1, 2, 127, 128, 255, 1ULL << 63, ~0ULL};
  const std::array ops{O::ADD, O::SUB, O::MUL, O::AND, O::OR, O::XOR,
                       O::SHL, O::SHR, O::SAR, O::EQ, O::NEQ,
                       O::ULT, O::ULE, O::UGT, O::UGE, O::SLT, O::SLE, O::SGT, O::SGE};
  std::mt19937_64 random{19014242};
  for (auto op : ops) {
    for (unsigned i = 0; i < 96; ++i) {
      auto left = c(i < edges.size() ? edges[i] : random(), widths[i % widths.size()]);
      auto right = c(i < edges.size() ? edges[edges.size() - i - 1] : random(),
                     widths[(i / widths.size()) % widths.size()]);
      auto expr = unary(O::TRUNC, binary(op, std::move(left), std::move(right)), 64);
      check(expr);
    }
  }
  for (auto op : {O::UDIV, O::SDIV, O::UMOD, O::SMOD}) {
    for (auto width : widths) {
      for (auto value : edges) {
        auto expr = unary(O::TRUNC, binary(op, c(value, width), c(3, width)), 64);
        // i1's divisor is -1; avoid its signed minimum / -1 overflow.
        if (width == 1 && (op == O::SDIV || op == O::SMOD) && (value & 1)) continue;
        check(expr);
      }
    }
  }
  for (auto width : widths) {
    // Include each width's signed boundary and exact shift boundaries rather
    // than relying on random inputs to encounter them.
    const auto sign = std::uint64_t{1} << (std::min<unsigned>(width, 64) - 1);
    for (auto value : {std::uint64_t{0}, std::uint64_t{1}, sign - 1, sign,
                       ~std::uint64_t{0}}) {
      for (auto op : {O::NEG, O::NOT})
        check(unary(O::TRUNC, unary(op, c(value, width)), 64));
      for (auto to : widths)
        for (auto op : {O::ZEXT, O::SEXT, O::TRUNC})
          check(unary(O::TRUNC, unary(op, c(value, width), to), 64));
      for (auto count : {0u, 1u, unsigned(width - 1), unsigned(width), unsigned(width + 1)})
        for (auto op : {O::SHL, O::SHR, O::SAR})
          check(unary(O::TRUNC, binary(op, c(value, width), c(count, width)), 64));
    }
    auto signBit = binary(O::SHL, c(1, width), c(width - 1, width));
    for (auto op : {O::SLT, O::SLE, O::SGT, O::SGE})
      check(binary(op, signBit, c(0, width)));
  }
}
#endif
