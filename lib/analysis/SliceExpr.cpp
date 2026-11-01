#include <dragon/analysis/SliceExpr.hpp>

#include "utils/Utils.hpp"

#include <format>
#include <type_traits>

namespace dragon::slice {

bool sameValue(const Expr &a, const Expr &b) {
  auto children = [](const std::shared_ptr<Expr> &x,
                     const std::shared_ptr<Expr> &y) {
    return x && y && sameValue(*x, *y);
  };
  return a.visit([&](const auto &x) -> bool {
    using T = std::decay_t<decltype(x)>;
    const auto *y = std::get_if<T>(&b.data);
    if (!y)
      return false;
    if constexpr (std::is_same_v<T, Const>)
      return x.value == y->value && x.width == y->width;
    else if constexpr (std::is_same_v<T, SymReg>)
      return x.reg.offset == y->reg.offset && x.reg.width == y->reg.width &&
             x.origin == y->origin;
    else if constexpr (std::is_same_v<T, UnaryOp>)
      return x.op == y->op && x.resultWidth == y->resultWidth &&
             children(x.operand, y->operand);
    else if constexpr (std::is_same_v<T, BinOp>)
      return x.op == y->op && children(x.left, y->left) &&
             children(x.right, y->right);
    else if constexpr (std::is_same_v<T, Load>)
      return x.width == y->width &&
             ((x.opIndex >= 0 && x.opIndex == y->opIndex &&
               x.blockAddress == y->blockAddress && x.context == y->context) ||
              (x.memoryState && x.memoryState == y->memoryState &&
               children(x.addr, y->addr)));
    else // Undef
      return x.opIndex >= 0 && x.opIndex == y->opIndex &&
             x.blockAddress == y->blockAddress && x.width == y->width;
  });
}

std::string toString(const Expr &e) {
  return e.visit(utils::overloaded{
          [](const Const &c) {
            return std::format("0x{:x}:i{}", c.value, c.width);
          },
          [](const SymReg &r) {
            if (!r.reg.name.empty())
              return std::format("{}:i{}", r.reg.name, r.reg.width);
            return std::format("reg[{}]:i{}", r.reg.offset, r.reg.width);
          },
          [](const BinOp &b) {
            return std::format("{}({}, {})", ignil::opcodeName(b.op),
                               toString(*b.left), toString(*b.right));
          },
          [](const UnaryOp &u) {
            return std::format("{}({})", ignil::opcodeName(u.op),
                               toString(*u.operand));
          },
          [](const Load &l) {
            return std::format("MEM[{}]:i{}", toString(*l.addr), l.width);
          },
          [](const Undef &) -> std::string { return "undef"; },
  });
}

} // namespace dragon::slice
