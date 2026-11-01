#include <dragon/analysis/SliceExprZ3.hpp>

#include "utils/Utils.hpp"

#include <cassert>
#include <format>

namespace dragon::slice {

SliceExprZ3::SliceExprZ3(z3::context &ctx) : ctx_(ctx) {}

void SliceExprZ3::reset() const { symCache_.clear(); }

z3::expr SliceExprZ3::symFor(const SymReg &r) const {
  std::string key =
          r.reg.name.empty()
                  ? std::format("reg_{}:i{}", r.reg.offset, r.reg.width)
                  : std::format("{}:{}", r.reg.name, r.reg.width);

  auto it = symCache_.find(key);
  if (it != symCache_.end()) {
    return it->second;
  }
  z3::expr sym = ctx_.bv_const(key.c_str(), r.reg.width);
  symCache_.emplace(key, sym);
  return sym;
}

static z3::expr widenTo(z3::expr e, unsigned targetWidth) {
  unsigned w = e.get_sort().bv_size();
  if (w >= targetWidth) {
    return e;
  }
  return z3::zext(e, targetWidth - w);
}

z3::expr SliceExprZ3::lower(const Expr &e) const {
  return e.visit(utils::overloaded{
          // Generator of Const, simply generate
          // a bit-vector value with the value and width
          [&](const Const &c) -> z3::expr {
            return ctx_.bv_val(c.value, c.width);
          },
          // Generator for a register, create a bit-vector
          // constant
          [&](const SymReg &r) -> z3::expr { return symFor(r); },
          [&](const BinOp &b) -> z3::expr {
            // First lower both sides from the expression
            z3::expr l = lower(*b.left);
            z3::expr r = lower(*b.right);

            // Operands must have the same width to operate
            // between them
            unsigned w =
                    std::max(l.get_sort().bv_size(), r.get_sort().bv_size());
            l = widenTo(l, w);
            r = widenTo(r, w);

            // Construction of different expressions for Z3
            using Opc = ignil::Opcode;
            switch (b.op) {
              case Opc::ADD:
                return l + r;
              case Opc::SUB:
                return l - r;
              case Opc::MUL:
                return l * r;
              case Opc::AND:
                return l & r;
              case Opc::OR:
                return l | r;
              case Opc::XOR:
                return l ^ r;
              case Opc::SHL:
                return z3::shl(l, r);
              case Opc::SHR:
                return z3::lshr(l, r);
              case Opc::SAR:
                return z3::ashr(l, r);
              case Opc::SDIV:
                return l / r;
              case Opc::UDIV:
                return z3::udiv(l, r);
              case Opc::SMOD:
                return z3::srem(l, r);
              case Opc::UMOD:
                return z3::urem(l, r);
              // Comparisons produce a 1-bit result
              case Opc::EQ:
                return z3::ite(l == r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              case Opc::NEQ:
                return z3::ite(l != r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              case Opc::ULT:
                return z3::ite(z3::ult(l, r), ctx_.bv_val(1, 1),
                               ctx_.bv_val(0, 1));
              case Opc::ULE:
                return z3::ite(z3::ule(l, r), ctx_.bv_val(1, 1),
                               ctx_.bv_val(0, 1));
              case Opc::UGT:
                return z3::ite(z3::ugt(l, r), ctx_.bv_val(1, 1),
                               ctx_.bv_val(0, 1));
              case Opc::UGE:
                return z3::ite(z3::uge(l, r), ctx_.bv_val(1, 1),
                               ctx_.bv_val(0, 1));
              case Opc::SLT:
                return z3::ite(l < r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              case Opc::SLE:
                return z3::ite(l <= r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              case Opc::SGT:
                return z3::ite(l > r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              case Opc::SGE:
                return z3::ite(l >= r, ctx_.bv_val(1, 1), ctx_.bv_val(0, 1));
              default:
                assert(false && "unhandled BinOp opcode in SliceExprZ3");
                return ctx_.bv_val(0, w);
            }
          },
          // Unary operators
          [&](const UnaryOp &u) -> z3::expr {
            z3::expr v = lower(*u.operand);
            unsigned srcW = v.get_sort().bv_size();
            unsigned dstW = u.resultWidth ? u.resultWidth : srcW;

            using Opc = ignil::Opcode;
            switch (u.op) {
              case Opc::NEG:
                return -v;
              case Opc::NOT:
                return ~v;
              case Opc::ZEXT:
                return dstW > srcW ? z3::zext(v, dstW - srcW) : v;
              case Opc::SEXT:
                return dstW > srcW ? z3::sext(v, dstW - srcW) : v;
              case Opc::TRUNC:
                return dstW < srcW ? v.extract(dstW - 1, 0) : v;
              default:
                assert(false && "unhandled UnaryOp opcode in SliceExprZ3");
                return ctx_.bv_val(0, srcW);
            }
          },
          [&](const Load &l) -> z3::expr {
            // Model memory as an uninterpreted symbol keyed by address
            // expression. Two loads from the same symbolic address get the same
            // symbol.
            z3::expr addrExpr = lower(*l.addr);
            std::string key =
                    std::format("mem[{}]:i{}", addrExpr.to_string(), l.width);
            auto it = symCache_.find(key);
            if (it != symCache_.end())
              return it->second;
            z3::expr sym = ctx_.bv_const(key.c_str(), l.width);
            symCache_.emplace(key, sym);
            return sym;
          },
          // Every unknown is its own variable, except references to the
          // same producing op, which share one. (A single shared "undef"
          // made unrelated unknowns equal, so SUB(undef, undef) of two
          // different unlifted instructions solved uniquely to 0.)
          [&](const Undef &u) -> z3::expr {
            if (u.opIndex >= 0) {
              const std::string key = std::format(
                      "undef@{:x}#{}:i{}", u.blockAddress, u.opIndex, u.width);
              auto it = symCache_.find(key);
              if (it != symCache_.end())
                return it->second;
              z3::expr sym = ctx_.bv_const(key.c_str(), u.width);
              symCache_.emplace(key, sym);
              return sym;
            }
            const std::string name =
                    std::format("undef!{}", anonymousUndefs_++);
            return ctx_.bv_const(name.c_str(), u.width);
          }});
}

} // namespace dragon::slice
