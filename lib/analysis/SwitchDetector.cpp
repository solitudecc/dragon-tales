#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

namespace dragon::slice {


// =============================================================================
// Internal helpers
// =============================================================================

using Opc = ignil::Opcode;

static std::uint64_t lowMask(unsigned bits) {
  return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}

static std::optional<std::uint64_t> asConst(const Expr &e) {
  if (const auto *c = std::get_if<Const>(&e.data))
    return c->value & lowMask(c->width);
  return std::nullopt;
}

// Bit width of a slice expression's value.
static unsigned exprWidth(const Expr &e) {
  return e.visit([](const auto &n) -> unsigned {
    using T = std::decay_t<decltype(n)>;
    if constexpr (std::is_same_v<T, Const>)
      return n.width;
    else if constexpr (std::is_same_v<T, SymReg>)
      return n.reg.width;
    else if constexpr (std::is_same_v<T, Load>)
      return n.width;
    else if constexpr (std::is_same_v<T, Undef>)
      return n.width;
    else if constexpr (std::is_same_v<T, UnaryOp>)
      return n.resultWidth ? n.resultWidth
                           : (n.operand ? exprWidth(*n.operand) : 64);
    else { // BinOp: comparisons give 1 bit, the rest the wider operand
      switch (n.op) {
        case Opc::EQ: case Opc::NEQ: case Opc::ULT: case Opc::ULE:
        case Opc::UGT: case Opc::UGE: case Opc::SLT: case Opc::SLE:
        case Opc::SGT: case Opc::SGE:
          return 1;
        default:
          return std::max(n.left ? exprWidth(*n.left) : 0u,
                          n.right ? exprWidth(*n.right) : 0u);
      }
    }
  });
}

// Peel width changes (ZEXT/SEXT/TRUNC) off an index: `mov edi, edi` before
// the dispatch makes the index ZEXT(EDI), `movsxd` makes it SEXT(EDI), and
// `cmp al, N; ja; movzx eax, al` makes it ZEXT(TRUNC(x, 8)). `bits` is
// narrowed to the index's real width - 8 in the last case: the table is
// indexed by an 8-bit value, whatever x is.
static const Expr &stripResize(const Expr &e, unsigned &bits) {
  bits = std::min(bits, exprWidth(e));
  if (const auto *u = std::get_if<UnaryOp>(&e.data))
    if (u->op == Opc::ZEXT || u->op == Opc::SEXT || u->op == Opc::TRUNC)
      return stripResize(*u->operand, bits);
  return e;
}

// The terms of a (nested) ADD.
static void flattenAdd(const Expr &e, std::vector<const Expr *> &out) {
  if (const auto *b = std::get_if<BinOp>(&e.data))
    if (b->op == Opc::ADD && b->left && b->right) {
      flattenAdd(*b->left, out);
      flattenAdd(*b->right, out);
      return;
    }
  out.push_back(&e);
}

// `index * scale` in either of the forms the lifters produce: SHL(x, k) -
// what the x86 lifter emits for a SIB scale - or MUL(x, c) in either order.
static bool asScaledIndex(const Expr &t, const Expr *&index,
                          std::uint64_t &scale) {
  const auto *b = std::get_if<BinOp>(&t.data);
  if (!b || !b->left || !b->right)
    return false;
  if (b->op == Opc::SHL) {
    auto k = asConst(*b->right);
    if (!k || *k > 3)
      return false;
    index = b->left.get();
    scale = std::uint64_t{1} << *k;
    return true;
  }
  if (b->op == Opc::MUL) {
    if (auto c = asConst(*b->right)) {
      index = b->left.get();
      scale = *c;
      return true;
    }
    if (auto c = asConst(*b->left)) {
      index = b->right.get();
      scale = *c;
      return true;
    }
  }
  return false;
}

// -- concrete evaluation of a guard condition --------------------------------
//
// The guard in front of a jump table is a comparison of the index against the
// table size, but its sliced shape depends on the lifter and the compare
// (`cmp; ja` on x86 slices to AND(EQ(ULT(..),0), EQ(EQ(SUB(..),0),0)), AArch64
// goes through NZCV). Rather than pattern-match every such shape, the
// condition is evaluated with the index set to 0, 1, 2, ...: the case count is
// the first index for which the branch leaves the dispatch block. Anything the
// condition depends on besides the index (another register, memory) makes it
// unevaluable, and no bound is reported.

struct EvalValue {
  std::uint64_t v;
  unsigned width;
};

class GuardEvaluator {
  public:
  explicit GuardEvaluator(const Expr &index) {
    if (const auto *r = std::get_if<SymReg>(&index.data))
      indexReg_ = &r->reg;
    else
      indexText_ = toString(index);
  }

  std::optional<EvalValue> eval(const Expr &e, std::uint64_t idx) const {
    idx_ = idx;
    return go(e);
  }

  private:
  // A reference to the index: for a register, any view of it at the same
  // offset (the guard may compare EDI while the table is indexed by RDI).
  std::optional<EvalValue> asIndex(const Expr &e) const {
    if (indexReg_) {
      if (const auto *r = std::get_if<SymReg>(&e.data))
        if (r->reg.offset == indexReg_->offset)
          return EvalValue{idx_ & lowMask(r->reg.width), r->reg.width};
      return std::nullopt;
    }
    if (toString(e) != indexText_)
      return std::nullopt;
    const unsigned w = exprWidth(e);
    return EvalValue{idx_ & lowMask(w), w};
  }

  static std::int64_t sx(std::uint64_t v, unsigned w) {
    if (w == 0 || w >= 64)
      return static_cast<std::int64_t>(v);
    const std::uint64_t sign = std::uint64_t{1} << (w - 1);
    return static_cast<std::int64_t>((v ^ sign) - sign);
  }

  std::optional<EvalValue> go(const Expr &e) const {
    if (auto v = asIndex(e))
      return v;
    if (const auto *c = std::get_if<Const>(&e.data))
      return EvalValue{c->value & lowMask(c->width), c->width};

    if (const auto *u = std::get_if<UnaryOp>(&e.data)) {
      auto a = go(*u->operand);
      if (!a)
        return std::nullopt;
      const unsigned to = u->resultWidth ? u->resultWidth : a->width;
      switch (u->op) {
        case Opc::NOT: return EvalValue{~a->v & lowMask(a->width), a->width};
        case Opc::NEG: return EvalValue{(0 - a->v) & lowMask(a->width), a->width};
        case Opc::ZEXT:
        case Opc::TRUNC: return EvalValue{a->v & lowMask(to), to};
        case Opc::SEXT:
          return EvalValue{static_cast<std::uint64_t>(sx(a->v, a->width)) &
                               lowMask(to),
                       to};
        default: return std::nullopt;
      }
    }

    if (const auto *b = std::get_if<BinOp>(&e.data)) {
      auto l = go(*b->left), r = go(*b->right);
      if (!l || !r)
        return std::nullopt;
      // Same widening as SliceExprZ3: the narrower operand is zero-extended.
      const unsigned w = std::max(l->width, r->width);
      const std::uint64_t x = l->v, y = r->v, m = lowMask(w);
      auto bit = [](bool c) { return EvalValue{c ? 1u : 0u, 1}; };
      switch (b->op) {
        case Opc::ADD: return EvalValue{(x + y) & m, w};
        case Opc::SUB: return EvalValue{(x - y) & m, w};
        case Opc::MUL: return EvalValue{(x * y) & m, w};
        case Opc::AND: return EvalValue{x & y, w};
        case Opc::OR:  return EvalValue{x | y, w};
        case Opc::XOR: return EvalValue{x ^ y, w};
        case Opc::SHL: return EvalValue{y >= w ? 0 : (x << y) & m, w};
        case Opc::SHR: return EvalValue{y >= w ? 0 : x >> y, w};
        case Opc::SAR: {
          const std::int64_t s = sx(x, w);
          return EvalValue{static_cast<std::uint64_t>(y >= w ? (s < 0 ? -1 : 0)
                                                         : s >> y) & m,
                       w};
        }
        case Opc::EQ:  return bit(x == y);
        case Opc::NEQ: return bit(x != y);
        case Opc::ULT: return bit(x < y);
        case Opc::ULE: return bit(x <= y);
        case Opc::UGT: return bit(x > y);
        case Opc::UGE: return bit(x >= y);
        case Opc::SLT: return bit(sx(x, w) < sx(y, w));
        case Opc::SLE: return bit(sx(x, w) <= sx(y, w));
        case Opc::SGT: return bit(sx(x, w) > sx(y, w));
        case Opc::SGE: return bit(sx(x, w) >= sx(y, w));
        default: return std::nullopt;
      }
    }
    return std::nullopt; // another register, memory, undef
  }

  const ignil::Reg *indexReg_ = nullptr;
  std::string indexText_;
  mutable std::uint64_t idx_ = 0;
};

// Largest table the guard search accepts.
constexpr std::uint32_t kMaxCases = 4096;

// The number of index values 0, 1, ..., N-1 for which the guard CJMP falls
// into `dispatch`, provided every larger index tried leaves it. 0 if the
// guard is not such a bound.
static std::uint32_t boundFromGuard(const Expr &cond, const ignil::Op &cjmp,
                                    std::uint64_t dispatch, const Expr &index,
                                    unsigned indexBits) {
  if (cjmp.srcs.size() < 3)
    return 0;
  const auto *t = std::get_if<ignil::Const>(&cjmp.srcs[1]);
  const auto *f = std::get_if<ignil::Const>(&cjmp.srcs[2]);
  if (!t || !f || (t->value == dispatch) == (f->value == dispatch))
    return 0;
  const bool dispatchWhenTrue = t->value == dispatch;

  GuardEvaluator ev(index);
  auto inBounds = [&](std::uint64_t idx) -> std::optional<bool> {
    auto v = ev.eval(cond, idx);
    if (!v)
      return std::nullopt;
    return ((v->v & 1) != 0) == dispatchWhenTrue;
  };

  // Only values the index can take are tried: an index that went through
  // an 8-bit register has 256 possible values, and probing it with
  // 0x80000000 would wrap to 0 and look like a hole in the bound.
  const std::uint64_t domain = lowMask(std::min(indexBits, 63u)) + 1;
  const std::uint64_t limit = std::min<std::uint64_t>(kMaxCases, domain);
  std::uint32_t n = 0;
  for (; n < limit; ++n) {
    auto in = inBounds(n);
    if (!in)
      return 0;
    if (!*in)
      break;
  }
  if (n == 0 || n == kMaxCases)
    return 0;
  // A real bound keeps every larger index out, including the ones a signed
  // compare would let through (the top half of the index's range).
  const std::uint64_t m = lowMask(indexBits);
  const std::uint64_t probes[] = {n + 1ull,      n + 2ull,         n + 15ull,
                                  m >> 1,        (m >> 1) + 1,     m,
                                  0x7fffffff,    0x80000000,       ~0ull};
  for (std::uint64_t raw: probes) {
    const std::uint64_t p = raw & m;
    if (p < n)
      continue; // wrapped into the accepted range: not a value to probe
    auto in = inBounds(p);
    if (!in || *in)
      return 0;
  }
  return n;
}

// Search the graph for the unique predecessor block of blockAddr whose IGNIL
// representation ends with a CJMP.  Returns nullptr when no such predecessor
// exists or when there are multiple candidates (ambiguous / join node).
static const ignil::Block *findCJMPPredecessor(std::uint64_t blockAddr,
                                               const Graph &graph) {
  const ignil::Block *found = nullptr;

  for (const BasicBlock &b: graph.blocks()) {
    if (b.startAddress() == blockAddr)
      continue;
    if (!b.successors().count(blockAddr))
      continue;

    const ignil::Block *ib = graph.getIGNILBlock(b.startAddress());
    if (!ib || ib->empty())
      continue;
    if (ib->ops().back().opcode != ignil::Opcode::CJMP)
      continue;

    // More than one CJMP predecessor -- give up, not a simple switch guard.
    if (found)
      return nullptr;
    found = ib;
  }

  return found;
}

// =============================================================================
// Public API
// =============================================================================

std::optional<SwitchCandidate> detectPossibleSwitch(const Expr &jumpTarget,
                                                    std::uint64_t blockAddr,
                                                    const Graph &graph) {
  // Phase 1: the target is LOAD(base + index * scale [+ more constants]).
  // The address is matched as a sum of terms rather than one tree shape, so
  // both a hand-built ADD(base, MUL(idx, scale)) and the x86 lifter's
  // ADD(ADD(0, SHL(idx, 3)), base) are recognised.
  const auto *load = std::get_if<Load>(&jumpTarget.data);
  if (!load || !load->addr)
    return std::nullopt;

  std::vector<const Expr *> terms;
  flattenAdd(*load->addr, terms);
  std::uint64_t tableBase = 0, scale = 0;
  const Expr *index = nullptr;
  for (const Expr *t: terms) {
    if (auto c = asConst(*t)) {
      tableBase += *c;
      continue;
    }
    const Expr *idx = nullptr;
    std::uint64_t sc = 0;
    if (index || !asScaledIndex(*t, idx, sc))
      return std::nullopt; // a second variable term: a base register, etc.
    index = idx;
    scale = sc;
  }
  if (!index || tableBase == 0)
    return std::nullopt;

  // Only 4-byte and 8-byte pointer tables are supported.
  if (scale != 4 && scale != 8)
    return std::nullopt;

  unsigned indexBits = 64;
  const Expr &core = stripResize(*index, indexBits);
  SwitchCandidate sw{tableBase, static_cast<std::uint32_t>(scale), 0,
                     ignil::Reg{0, 0, ""}};
  if (const auto *r = std::get_if<SymReg>(&core.data))
    sw.indexReg = r->reg;

  // Phase 2: the case count, from the bounds check that guards the block.
  // The guard block is the dispatch block's predecessor, so its condition is
  // sliced with one block less than the jump target was (callers use 5, as
  // the disassembler does): the index then reads the same in both slices.
  if (const ignil::Block *predBlock = findCJMPPredecessor(blockAddr, graph)) {
    const ignil::Op &cjmp = predBlock->ops().back();
    BackwardSlicer slicer{4};
    Expr condExpr =
            slicer.slice(cjmp.srcs[0], predBlock->startAddress(), graph);
    sw.numCases = boundFromGuard(condExpr, cjmp, blockAddr, core, indexBits);
  }

  // Without a guard there is no bound. A masked index (`and eax, 0x7f`) is
  // not one either: Lua's opcode dispatch masks to 128 values but its table
  // has 83 entries - the compiler drops the guard because the default case
  // is unreachable, and the bytes after the table are other data.

  return sw;
}

std::vector<std::uint64_t> readTableTargets(const SwitchCandidate &sw,
                                            std::span<const std::uint8_t> bytes,
                                            std::uint64_t baseAddr) {
  if (sw.numCases == 0)
    return {};
  if (sw.tableBase < baseAddr)
    return {};

  const std::size_t tableOffset =
          sw.tableBase - baseAddr;
  const std::size_t tableBytes = sw.numCases * sw.entrySize;

  if (tableOffset >= bytes.size() || tableBytes > bytes.size() - tableOffset)
    return {};

  std::vector<std::uint64_t> targets;
  targets.reserve(sw.numCases);

  for (std::uint32_t i = 0; i < sw.numCases; ++i) {
    const std::size_t offset = tableOffset + i * sw.entrySize;
    std::uint64_t target = 0;
    std::memcpy(&target, bytes.data() + offset, sw.entrySize);
    targets.push_back(target);
  }

  return targets;
}

} // namespace dragon::slice
