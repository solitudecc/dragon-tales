#pragma once

#include <dragon/analysis/SliceExpr.hpp>
#include <llvm/ADT/APInt.h>

#include <algorithm>
#include <optional>

namespace dragon::slice::detail {

// Evaluate only integer expressions made entirely of constants. Match the
// SliceExpr bit-vector rules: binary operands are zero-extended to their
// common width, comparisons produce one bit, and resizes carry explicit
// widths. APInt also keeps wide intermediates and host signed overflow safe.
class ConstantEvaluator {
 public:
  std::optional<Const> evaluate(const Expr &expr) {
    auto value = visit(expr, 0);
    if (!value || value->getBitWidth() > 64)
      return std::nullopt;
    return Const{value->getZExtValue(),
                 static_cast<ignil::Width>(value->getBitWidth())};
  }

 private:
  std::optional<llvm::APInt> visit(const Expr &expr, unsigned depth) {
    if (depth >= 128 || remaining_ == 0)
      return std::nullopt;
    --remaining_;
    if (const auto *c = std::get_if<Const>(&expr.data)) {
      if (c->width == 0)
        return std::nullopt;
      return llvm::APInt(64, c->value).zextOrTrunc(c->width);
    }
    if (const auto *u = std::get_if<UnaryOp>(&expr.data)) {
      if (!u->operand)
        return std::nullopt;
      auto value = visit(*u->operand, depth + 1);
      if (!value)
        return std::nullopt;
      const unsigned width = u->resultWidth ? u->resultWidth : value->getBitWidth();
      using O = ignil::Opcode;
      switch (u->op) {
        case O::NEG: return -*value;
        case O::NOT: return ~*value;
        case O::ZEXT:
          return width > value->getBitWidth() ? value->zext(width) : *value;
        case O::SEXT:
          return width > value->getBitWidth() ? value->sext(width) : *value;
        case O::TRUNC:
          return width < value->getBitWidth() ? value->trunc(width) : *value;
        default: return std::nullopt;
      }
    }
    const auto *b = std::get_if<BinOp>(&expr.data);
    if (!b || !b->left || !b->right)
      return std::nullopt; // LOAD, SymReg and UNDEF never become constants
    auto left = visit(*b->left, depth + 1);
    auto right = visit(*b->right, depth + 1);
    if (!left || !right)
      return std::nullopt;
    const unsigned width = std::max(left->getBitWidth(), right->getBitWidth());
    const auto l = left->zextOrTrunc(width), r = right->zextOrTrunc(width);
    const auto shift = static_cast<unsigned>(r.getLimitedValue(width));
    const auto bit = [](bool value) { return llvm::APInt(1, value); };
    using O = ignil::Opcode;
    switch (b->op) {
      case O::ADD: return l + r;
      case O::SUB: return l - r;
      case O::MUL: return l * r;
      case O::AND: return l & r;
      case O::OR: return l | r;
      case O::XOR: return l ^ r;
      case O::SHL: return shift == width ? llvm::APInt(width, 0) : l.shl(shift);
      case O::SHR: return shift == width ? llvm::APInt(width, 0) : l.lshr(shift);
      case O::SAR:
        return shift == width ? (l.isNegative() ? llvm::APInt::getAllOnes(width)
                                                : llvm::APInt(width, 0))
                              : l.ashr(shift);
      case O::UDIV:
      case O::UMOD:
        if (r.isZero()) return std::nullopt;
        return b->op == O::UDIV ? l.udiv(r) : l.urem(r);
      case O::SDIV:
      case O::SMOD:
        if (r.isZero() || (l.isMinSignedValue() && r.isAllOnes()))
          return std::nullopt; // no invented result for a faulting division
        return b->op == O::SDIV ? l.sdiv(r) : l.srem(r);
      case O::EQ: return bit(l == r);
      case O::NEQ: return bit(l != r);
      case O::ULT: return bit(l.ult(r));
      case O::ULE: return bit(l.ule(r));
      case O::UGT: return bit(l.ugt(r));
      case O::UGE: return bit(l.uge(r));
      case O::SLT: return bit(l.slt(r));
      case O::SLE: return bit(l.sle(r));
      case O::SGT: return bit(l.sgt(r));
      case O::SGE: return bit(l.sge(r));
      default: return std::nullopt;
    }
  }

  unsigned remaining_ = 4096;
};

inline std::optional<Const> constantValue(const Expr &expr) {
  return ConstantEvaluator{}.evaluate(expr);
}

} // namespace dragon::slice::detail
