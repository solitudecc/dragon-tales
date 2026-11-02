#include "GuardProof.hpp"
#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/SliceExprZ3.hpp>
#endif

#include "utils/FloatOps.hpp"

namespace dragon::slice {
namespace {
using Opc = ignil::Opcode;
using U64 = std::uint64_t;
U64 mask(unsigned bits) { return bits >= 64 ? ~U64{0} : (U64{1} << bits) - 1; }
bool comparison(Opc op) { return op >= Opc::EQ && op <= Opc::SGE; }
unsigned width(const Expr &e) {
  return e.visit([](const auto &n) -> unsigned {
    using T = std::decay_t<decltype(n)>;
    if constexpr (std::is_same_v<T, SymReg>) return n.reg.width;
    else if constexpr (std::is_same_v<T, Const> || std::is_same_v<T, Load> ||
                       std::is_same_v<T, Undef>) return n.width;
    else if constexpr (std::is_same_v<T, UnaryOp>)
      return n.resultWidth ? n.resultWidth : width(*n.operand);
    else return comparison(n.op) || ignil::detail::floatComparisonOp(n.op)
                    ? 1 : std::max(width(*n.left), width(*n.right));
  });
}
std::optional<U64> constant(const Expr &e) {
  if (auto *c = std::get_if<Const>(&e.data)) return c->value & mask(c->width);
  return std::nullopt;
}
struct Range { U64 lo, hi; bool operator==(const Range &) const = default; };
using Ranges = std::vector<Range>;
Ranges unite(Ranges a, const Ranges &b) {
  a.insert(a.end(), b.begin(), b.end());
  std::ranges::sort(a, {}, &Range::lo);
  Ranges result;
  for (auto r: a) {
    if (!result.empty() && (r.lo <= result.back().hi ||
        (result.back().hi != ~U64{0} && r.lo == result.back().hi + 1)))
      result.back().hi = std::max(result.back().hi, r.hi);
    else result.push_back(r);
  }
  return result;
}
Ranges intersect(const Ranges &a, const Ranges &b) {
  Ranges result;
  for (auto x: a) for (auto y: b)
    if (auto lo = std::max(x.lo, y.lo), hi = std::min(x.hi, y.hi); lo <= hi)
      result.push_back({lo, hi});
  return unite(std::move(result), {});
}
Ranges invert(const Ranges &a, U64 maximum) {
  Ranges result;
  U64 next = 0;
  for (auto r: a) {
    if (next < r.lo) result.push_back({next, r.lo - 1});
    if (r.hi == maximum) return result;
    next = r.hi + 1;
  }
  result.push_back({next, maximum});
  return result;
}
Ranges truncate(const Ranges &a, unsigned bits) {
  if (bits >= 64) return a;
  const U64 domain = U64{1} << bits, m = domain - 1;
  Ranges result;
  for (auto r: a) {
    if (r.hi - r.lo >= m) return {{0, m}};
    const U64 lo = r.lo & m, hi = r.hi & m;
    if ((r.lo >> bits) == (r.hi >> bits)) result.push_back({lo, hi});
    else {
      result.push_back({lo, m});
      result.push_back({0, hi});
    }
  }
  return unite(std::move(result), {});
}

// An exact set of accepted scalar inputs for compiler-shaped comparisons and
// boolean combinations. Unrecognised operations go to SMT (or remain unknown
// without Z3). No sampled points are used as a proof.
class RangeProof {
  const Expr &core_;
  unsigned bits_, indexBits_;
  int budget_ = 4096;
  int widthBudget_ = 4096;
  U64 maximum() const { return mask(bits_); }
  bool sameCore(const Expr &e) const {
    if (auto *a = std::get_if<SymReg>(&core_.data)) {
      auto *b = std::get_if<SymReg>(&e.data);
      return b && a->reg.offset == b->reg.offset && a->origin == b->origin;
    }
    return sameValue(core_, e);
  }
  void collectWidth(const Expr &e, unsigned cap = 64) {
    if (--widthBudget_ < 0) return;
    if (auto *u = std::get_if<UnaryOp>(&e.data)) {
      collectWidth(*u->operand, std::min(cap, std::min(width(e), width(*u->operand))));
    } else if (sameCore(e)) {
      bits_ = std::max(bits_, std::min(cap, width(e)));
    } else if (auto *b = std::get_if<BinOp>(&e.data)) {
      collectWidth(*b->left, comparison(b->op) ? 64 : cap);
      collectWidth(*b->right, comparison(b->op) ? 64 : cap);
    }
  }
  bool input(const Expr &e) const {
    const Expr *p = &e;
    bool numeric = width(e) >= bits_;
    while (auto *u = std::get_if<UnaryOp>(&p->data)) {
      if ((u->op != Opc::ZEXT && u->op != Opc::TRUNC) ||
          width(*u->operand) < bits_) return false;
      p = u->operand.get();
    }
    return numeric && width(*p) >= bits_ && sameCore(*p) &&
           (width(*p) == bits_ || width(e) == bits_);
  }
  Ranges unsignedCmp(Opc op, U64 c) const {
    const auto m = maximum();
    switch (op) {
      case Opc::EQ: return c <= m ? Ranges{{c, c}} : Ranges{};
      case Opc::NEQ: return invert(unsignedCmp(Opc::EQ, c), m);
      case Opc::ULT: return c == 0 ? Ranges{} : Ranges{{0, std::min(m, c - 1)}};
      case Opc::ULE: return {{0, std::min(m, c)}};
      case Opc::UGT: return c >= m ? Ranges{} : Ranges{{c + 1, m}};
      case Opc::UGE: return c > m ? Ranges{} : Ranges{{c, m}};
      default: return {};
    }
  }
  std::optional<Ranges> primitive(const BinOp &b) const {
    const Expr *x = b.left.get(), *c = b.right.get();
    Opc op = b.op;
    if (constant(*x) && !constant(*c)) {
      std::swap(x, c);
      switch (op) {
        case Opc::ULT: op = Opc::UGT; break; case Opc::ULE: op = Opc::UGE; break;
        case Opc::UGT: op = Opc::ULT; break; case Opc::UGE: op = Opc::ULE; break;
        case Opc::SLT: op = Opc::SGT; break; case Opc::SLE: op = Opc::SGE; break;
        case Opc::SGT: op = Opc::SLT; break; case Opc::SGE: op = Opc::SLE; break;
        default: break;
      }
    }
    auto value = constant(*c);
    if (!value) return std::nullopt;
    // Modular subtraction is zero exactly when its equal-width operands agree.
    if ((op == Opc::EQ || op == Opc::NEQ) && *value == 0) {
      if (auto *sub = std::get_if<BinOp>(&x->data); sub && sub->op == Opc::SUB &&
          input(*sub->left) && constant(*sub->right)) {
        return primitive(BinOp{op, sub->left, sub->right});
      }
    }
    if (!input(*x)) return std::nullopt;
    const auto w = std::max(width(*x), width(*c));
    if (w > 64) return std::nullopt;
    if (op < Opc::SLT || op > Opc::SGE) return unsignedCmp(op, *value);
    Opc uop = op == Opc::SLT ? Opc::ULT : op == Opc::SLE ? Opc::ULE :
              op == Opc::SGT ? Opc::UGT : Opc::UGE;
    const U64 sign = U64{1} << (w - 1);
    if (w > bits_) {
      if (*value & sign)
        return (op == Opc::SGT || op == Opc::SGE) ? Ranges{{0, maximum()}} : Ranges{};
      return unsignedCmp(uop, *value);
    }
    auto ordered = unsignedCmp(uop, *value ^ sign);
    Ranges result;
    for (auto r: ordered) {
      if (r.lo < sign)
        result.push_back({r.lo ^ sign, std::min(r.hi, sign - 1) ^ sign});
      if (r.hi >= sign)
        result.push_back({std::max(r.lo, sign) ^ sign, r.hi ^ sign});
    }
    return unite(std::move(result), {});
  }
  std::optional<Ranges> boolean(const Expr &e) {
    if (--budget_ < 0) return std::nullopt;
    if (auto c = constant(e); c && *c <= 1)
      return *c ? Ranges{{0, maximum()}} : Ranges{};
    if (auto *u = std::get_if<UnaryOp>(&e.data)) {
      if (u->op == Opc::ZEXT || u->op == Opc::TRUNC) return boolean(*u->operand);
      return std::nullopt;
    }
    auto *b = std::get_if<BinOp>(&e.data);
    if (!b) return std::nullopt;
    if (comparison(b->op))
      if (auto p = primitive(*b)) return p;
    auto l = boolean(*b->left), r = boolean(*b->right);
    if (!l || !r) return std::nullopt;
    Ranges result;
    switch (b->op) {
      case Opc::AND: result = intersect(*l, *r); break;
      case Opc::OR: result = unite(*l, *r); break;
      case Opc::XOR: case Opc::NEQ:
        result = unite(intersect(*l, invert(*r, maximum())),
                       intersect(*r, invert(*l, maximum()))); break;
      case Opc::EQ:
        result = unite(intersect(*l, *r),
                       intersect(invert(*l, maximum()), invert(*r, maximum()))); break;
      default: return std::nullopt;
    }
    return result.size() <= 256 ? std::optional{std::move(result)} : std::nullopt;
  }
public:
  RangeProof(const Expr &core, unsigned bits, const Expr &condition)
      : core_(core), bits_(bits), indexBits_(bits) { collectWidth(condition); }
  std::optional<bool> prove(const Expr &cond, const Expr &index, unsigned n, bool taken) {
    if (widthBudget_ < 0) return std::nullopt;
    // Arbitrary computed indices need a full SMT model, including masks and
    // their reachable values. A raw register/read supplies the scalar domain.
    if (!std::holds_alternative<SymReg>(core_.data) &&
        !std::holds_alternative<Load>(core_.data)) return std::nullopt;
    for (const Expr *p = &index; auto *u = std::get_if<UnaryOp>(&p->data); p = u->operand.get()) {
      if (u->op == Opc::SEXT) {
        const auto from = width(*u->operand);
        if (from == 0 || from > 64 || n - 1 >= (U64{1} << (from - 1)))
          return std::nullopt;
      } else if (u->op != Opc::ZEXT && u->op != Opc::TRUNC) return std::nullopt;
    }
    if (bits_ == 0 || bits_ > 64 || n > mask(indexBits_)) return std::nullopt;
    auto allowed = boolean(cond);
    if (!allowed) return std::nullopt;
    if (!taken) *allowed = invert(*allowed, maximum());
    // Different wider input ranges can map to the same narrow table index.
    // Project the entire accepted/rejected sets, including all high-bit values.
    const auto rejected = truncate(invert(*allowed, maximum()), indexBits_);
    const auto projected = truncate(*allowed, indexBits_);
    return projected == Ranges{{0, n - 1}} &&
           std::ranges::any_of(rejected, [n](auto r) { return r.hi >= n; });
  }
};

#ifdef DRAGON_WITH_Z3
// Give all views of one physical register at one program point shared bytes.
// High bytes of a wider guard remain free when the table uses a narrow view.
class RegisterBytes {
  std::unordered_map<std::string, ignil::Reg> bytes_;
  int budget_ = 16384;
  std::shared_ptr<Expr> ptr(const Expr &e) { return std::make_shared<Expr>(rewrite(e)); }
public:
  Expr rewrite(const Expr &e) {
    if (--budget_ < 0) throw std::runtime_error("guard expression budget");
    return e.visit([&](const auto &n) -> Expr {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, SymReg>) {
        auto byte = [&](unsigned i) {
          const auto key = n.origin ?
              std::format("{:x}#{}:{}[{}]", n.origin->blockAddress,
                          n.origin->opIndex, n.origin->context, n.reg.offset + i) :
              std::format("input[{}]", n.reg.offset + i);
          auto it = bytes_.find(key);
          if (it == bytes_.end()) {
            const auto id = static_cast<std::uint32_t>(bytes_.size());
            it = bytes_.emplace(key, ignil::Reg{id, 8, std::format("guard_byte_{}", id)}).first;
          }
          auto value = std::make_shared<Expr>(SymReg{it->second});
          if (n.reg.width < 8)
            return std::make_shared<Expr>(UnaryOp{Opc::TRUNC, value, n.reg.width});
          return std::make_shared<Expr>(UnaryOp{Opc::ZEXT, value, n.reg.width});
        };
        auto value = byte(0);
        for (unsigned i = 1; i < (n.reg.width + 7u) / 8u; ++i) {
          auto shifted = std::make_shared<Expr>(BinOp{Opc::SHL, byte(i),
              std::make_shared<Expr>(Const{i * 8u, n.reg.width})});
          value = std::make_shared<Expr>(BinOp{Opc::OR, value, shifted});
        }
        return Expr{static_cast<const Expr &>(*value)};
      } else if constexpr (std::is_same_v<T, BinOp>) {
        if (!comparison(n.op) && n.op != Opc::ADD && n.op != Opc::SUB &&
            n.op != Opc::MUL && n.op != Opc::AND && n.op != Opc::OR &&
            n.op != Opc::XOR && n.op != Opc::SHL && n.op != Opc::SHR &&
            n.op != Opc::SAR && n.op != Opc::SDIV && n.op != Opc::UDIV &&
            n.op != Opc::SMOD && n.op != Opc::UMOD)
          throw std::runtime_error("unsupported guard binary operation");
        return Expr{BinOp{n.op, ptr(*n.left), ptr(*n.right)}};
      } else if constexpr (std::is_same_v<T, UnaryOp>) {
        if (n.op != Opc::ZEXT && n.op != Opc::SEXT && n.op != Opc::TRUNC &&
            n.op != Opc::NEG && n.op != Opc::NOT)
          throw std::runtime_error("unsupported guard unary operation");
        return Expr{UnaryOp{n.op, ptr(*n.operand), n.resultWidth}};
      }
      else if constexpr (std::is_same_v<T, Load>) {
        auto load = n;
        load.addr = ptr(*n.addr);
        return Expr{std::move(load)};
      } else return Expr{n};
    });
  }
};
#endif
} // namespace

bool proveGuardBound(const Expr &condition, const Expr &index, const Expr &core,
                     unsigned indexBits, std::uint32_t cases, bool taken) {
  if (cases == 0) return false;
  if (auto result = RangeProof{core, indexBits, condition}.prove(condition, index, cases, taken))
    return *result;
#ifdef DRAGON_WITH_Z3
  try {
    RegisterBytes inputs;
    const auto cond = inputs.rewrite(condition);
    const auto idx = inputs.rewrite(index);
    z3::context ctx;
    SliceExprZ3 lowerer{ctx};
    const auto c = lowerer.lower(cond), x = lowerer.lower(idx);
    const auto w = x.get_sort().bv_size();
    if (cases > mask(w)) return false;
    const auto allows = (c.extract(0, 0) == ctx.bv_val(taken ? 1 : 0, 1));
    const auto outside = z3::uge(x, ctx.bv_val(cases, w));
    z3::solver solver{ctx};
    solver.set("timeout", 50u);
    solver.add(allows && outside);
    if (solver.check() != z3::unsat) return false;
    solver.reset();
    solver.set("timeout", 50u);
    solver.add(allows && x == ctx.bv_val(cases - 1, w));
    if (solver.check() != z3::sat) return false;
    // Width/masking alone is not evidence of a table's length: the guard
    // itself must exclude some otherwise possible out-of-range index.
    solver.reset();
    solver.set("timeout", 50u);
    solver.add(!allows && outside);
    return solver.check() == z3::sat;
  } catch (const z3::exception &) { return false; }
    catch (const std::runtime_error &) { return false; }
#else
  return false;
#endif
}
} // namespace dragon::slice
