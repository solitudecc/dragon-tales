#include "LLVMDispatchDecoderSSA.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace llvm;
namespace dragon::llvmlifter::detail {
namespace {
bool integer(Value *v) {
  return v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() <= 128;
}
bool bad(Value *v) { return isa<UndefValue>(v) || isa<PoisonValue>(v); }
bool total(Instruction &i) {
  switch (i.getOpcode()) {
  case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
  case Instruction::And: case Instruction::Or: case Instruction::Xor:
  case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
  case Instruction::ZExt: case Instruction::SExt: case Instruction::Trunc:
    return integer(&i);
  default: return false;
  }
}
// Each supported operation is evaluated over an exhaustive finite domain.
// In particular, flags are never dropped to obtain an otherwise valid value.
std::optional<APInt> evaluate(Instruction &i, const std::vector<APInt> &v) {
  const auto &a = v[0];
  const unsigned width = i.getType()->getIntegerBitWidth();
  if (auto *cast = dyn_cast<CastInst>(&i)) {
    if (cast->getOpcode() == Instruction::ZExt) {
      if (i.hasNonNeg() && a.isNegative()) return {};
      return a.zext(width);
    }
    if (cast->getOpcode() == Instruction::SExt) return a.sext(width);
    if (cast->getOpcode() == Instruction::Trunc) {
      auto result = a.trunc(width);
      if (i.hasNoUnsignedWrap() && result.zext(a.getBitWidth()) != a) return {};
      if (i.hasNoSignedWrap() && result.sext(a.getBitWidth()) != a) return {};
      return result;
    }
    return {};
  }
  const auto &b = v[1];
  bool u = false, s = false;
  APInt result(a.getBitWidth(), 0);
  switch (i.getOpcode()) {
  case Instruction::Add: result = a.uadd_ov(b, u); (void)a.sadd_ov(b, s); break;
  case Instruction::Sub: result = a.usub_ov(b, u); (void)a.ssub_ov(b, s); break;
  case Instruction::Mul: result = a.umul_ov(b, u); (void)a.smul_ov(b, s); break;
  case Instruction::And: return a & b;
  case Instruction::Or:
    if (auto *op = dyn_cast<PossiblyDisjointInst>(&i); op && op->isDisjoint() && !(a & b).isZero()) return {};
    return a | b;
  case Instruction::Xor: return a ^ b;
  case Instruction::Shl:
    if (b.uge(a.getBitWidth())) return {};
    result = a.ushl_ov(b, u); (void)a.sshl_ov(b, s); break;
  case Instruction::LShr: case Instruction::AShr: {
    if (b.uge(a.getBitWidth())) return {};
    const unsigned shift = static_cast<unsigned>(b.getZExtValue());
    if (i.isExact() && !a.getLoBits(shift).isZero()) return {};
    return i.getOpcode() == Instruction::LShr ? a.lshr(shift) : a.ashr(shift);
  }
  default: return {};
  }
  if ((i.hasNoUnsignedWrap() && u) || (i.hasNoSignedWrap() && s)) return {};
  return result;
}
struct Plan {
  enum Kind { Constant, Phi, Select, Atom } kind = Constant;
  Value *original = nullptr, *condition = nullptr;
  std::optional<APInt> zero, one;
  std::vector<Plan *> arms;
};
class Planner {
public:
  Function &function;
  SwitchInst &selected;
  DispatchDecoderSSAOptions options;
  DispatchDecoderSSAStats stats;
  DominatorTree dom;
  std::vector<Instruction *> decoder;
  std::vector<std::unique_ptr<Plan>> plans;
  std::map<Value *, Plan *> finished;
  std::set<Value *> gray;
  std::set<Value *> expressionNodes;
  bool failed = false;
  bool consumeOriginalUses;
  unsigned resultWidth;
  Planner(Function &f, SwitchInst &s, DispatchDecoderSSAOptions o, bool consume)
      : function(f), selected(s), options(o), dom(f),
        consumeOriginalUses(consume),
        resultWidth(s.getCondition()->getType()->getIntegerBitWidth()) {}
  bool refuse(const char *message, bool budget = false) {
    failed = true; stats.refused = true; stats.budgetExhausted |= budget;
    stats.message = message; return false;
  }
  bool spend(std::size_t amount = 1) {
    if (amount > options.maxWork - stats.work) return refuse("work budget", true);
    stats.work += amount; return true;
  }
  bool count(Value *value, unsigned depth) {
    if (!spend()) return false;
    if (depth > 32) return refuse("depth budget", true);
    if (expressionNodes.insert(value).second && ++stats.nodes > options.maxNodes)
      return refuse("node budget", true);
    return true;
  }
  bool reserve(std::size_t n) {
    if (n > options.maxInstructions - stats.instructions)
      return refuse("instruction budget", true);
    stats.instructions += n; return true;
  }
  Plan *make(Plan::Kind kind, Value *v) {
    auto node = std::make_unique<Plan>(); node->kind = kind; node->original = v;
    auto *result = node.get(); plans.push_back(std::move(node)); return result;
  }
  // Decoder has one variable operand per operation. All other operands must
  // be integer constants, so no new observations are moved across edges.
  Value *extract() {
    Value *value = selected.getCondition();
    for (unsigned depth = 0; depth <= 32; ++depth) {
      if (isa<PHINode>(value) || isa<SelectInst>(value)) {
        std::reverse(decoder.begin(), decoder.end()); return value;
      }
      if (!count(value, depth)) return nullptr;
      auto *i = dyn_cast<Instruction>(value);
      if (!i || !total(*i)) { refuse("unsupported decoder operation"); return nullptr; }
      Value *variable = nullptr;
      for (auto &operand : i->operands()) {
        auto *v = operand.get();
        if (!integer(v) || bad(v)) { refuse("unsafe decoder operand"); return nullptr; }
        if (!isa<ConstantInt>(v)) {
          if (variable) { refuse("non-unary decoder"); return nullptr; }
          variable = v;
        }
      }
      if (!variable) { refuse("decoder has no varying input"); return nullptr; }
      decoder.push_back(i); value = variable;
    }
    refuse("decoder depth budget", true); return nullptr;
  }
  std::optional<APInt> decode(APInt value) {
    for (auto *i : decoder) {
      if (!spend()) return {};
      std::vector<APInt> operands;
      for (auto &op : i->operands()) {
        if (auto *c = dyn_cast<ConstantInt>(op)) operands.push_back(c->getValue());
        else operands.push_back(value);
      }
      auto next = evaluate(*i, operands);
      if (!next) { refuse("unsafe decoder flags or shift"); return {}; }
      value = *next;
    }
    return value;
  }
  bool observation(Value *v) {
    if (bad(v) || !integer(v)) return false;
    if (v->getType()->isIntegerTy(1)) return !isa<Constant>(v);
    auto *i = dyn_cast<BinaryOperator>(v);
    if (!i || i->getOpcode() != Instruction::And) return false;
    auto *c = dyn_cast<ConstantInt>(i->getOperand(1));
    auto *other = i->getOperand(0);
    if (!c) { c = dyn_cast<ConstantInt>(i->getOperand(0)); other = i->getOperand(1); }
    return c && c->isOne() && !bad(other);
  }
  std::optional<APInt> leaf(Value *v, Value *&atom, bool one,
                            unsigned depth, std::set<Value *> &active) {
    if (!count(v, depth)) return {};
    if (bad(v) || !integer(v)) { refuse("unsafe leaf operand"); return {}; }
    if (auto *c = dyn_cast<ConstantInt>(v)) return c->getValue();
    if (observation(v)) {
      if (atom && atom != v) { refuse("multiple SSA observations in leaf"); return {}; }
      atom = v; return APInt(v->getType()->getIntegerBitWidth(), one);
    }
    auto *i = dyn_cast<Instruction>(v);
    if (!i || !total(*i)) { refuse("unsupported leaf operation"); return {}; }
    if (isa<BinaryOperator>(i) && !isa<ConstantInt>(i->getOperand(0)) &&
        !isa<ConstantInt>(i->getOperand(1))) {
      refuse("leaf operation requires a constant side operand"); return {};
    }
    if (!active.insert(v).second) { refuse("leaf cycle"); return {}; }
    std::vector<APInt> operands;
    // Both binary operands are visited; a late poison/second atom is fatal.
    for (auto &operand : i->operands()) {
      auto part = leaf(operand, atom, one, depth + 1, active);
      if (!part) return {};
      operands.push_back(*part);
    }
    active.erase(v);
    auto result = evaluate(*i, operands);
    if (!result) refuse("unsafe leaf flags or shift");
    return result;
  }
  bool covered(PHINode &phi) {
    std::map<BasicBlock *, unsigned> actual, recorded;
    for (auto *predecessor : predecessors(phi.getParent())) { if (!spend()) return false; ++actual[predecessor]; }
    for (unsigned n = 0; n != phi.getNumIncomingValues(); ++n) {
      if (!spend()) return false;
      ++recorded[phi.getIncomingBlock(n)];
      if (!dom.dominates(phi.getIncomingValue(n), phi.getIncomingBlock(n)->getTerminator()))
        return refuse("non-dominating PHI incoming");
    }
    return actual == recorded || refuse("incomplete PHI edge coverage");
  }
  Plan *build(Value *v, unsigned depth = 0) {
    if (!count(v, depth)) return nullptr;
    if (bad(v) || !integer(v)) { refuse("unsafe source operand"); return nullptr; }
    if (auto it = finished.find(v); it != finished.end()) return it->second;
    if (!gray.insert(v).second) { refuse("encoded PHI/select cycle"); return nullptr; }
    Plan *p = nullptr;
    if (auto *c = dyn_cast<ConstantInt>(v)) {
      auto result = decode(c->getValue()); if (!result) return nullptr;
      p = make(Plan::Constant, v); p->zero = *result;
    } else if (auto *phi = dyn_cast<PHINode>(v)) {
      if (!covered(*phi)) return nullptr;
      if (++stats.phis > options.maxPhis || !reserve(1)) { refuse("PHI budget", true); return nullptr; }
      p = make(Plan::Phi, v);
      for (auto &incoming : phi->incoming_values()) {
        auto *arm = build(incoming, depth + 1); if (!arm) return nullptr; p->arms.push_back(arm);
      }
    } else if (auto *select = dyn_cast<SelectInst>(v)) {
      if (bad(select->getCondition()) || !reserve(1)) { refuse("unsafe select condition"); return nullptr; }
      p = make(Plan::Select, v); p->condition = select->getCondition();
      for (unsigned n : {1u, 2u}) {
        auto *arm = build(select->getOperand(n), depth + 1); if (!arm) return nullptr; p->arms.push_back(arm);
      }
    } else {
      Value *atom = nullptr; std::set<Value *> active;
      auto zero = leaf(v, atom, false, depth + 1, active);
      if (!zero || !atom) { if (!failed) refuse("leaf has no finite observation"); return nullptr; }
      Value *again = nullptr; active.clear();
      auto one = leaf(v, again, true, depth + 1, active);
      if (!one || again != atom) return nullptr;
      auto a = decode(*zero), b = decode(*one); if (!a || !b) return nullptr;
      auto *at = dyn_cast<Instruction>(v);
      if (!at || isa<PHINode>(at) || !dom.dominates(atom, at) ||
          !reserve(atom->getType()->isIntegerTy(1) ? 1 : 2)) {
        refuse("unsupported leaf placement"); return nullptr;
      }
      p = make(Plan::Atom, v); p->condition = atom; p->zero = *a; p->one = *b;
    }
    gray.erase(v); finished.emplace(v, p); return p;
  }
  DispatchDecoderSSAStats run() {
    Value *source = extract();
    if (!source || decoder.empty()) { if (!failed) refuse("no scalar decoder"); return stats; }
    auto *root = build(source, static_cast<unsigned>(decoder.size()));
    auto *old = selected.getCondition();
    std::vector<Use *> originalUses;
    if (root && !failed && consumeOriginalUses) {
      for (auto &use : old->uses()) {
        if (!spend()) return stats;
        if (originalUses.size() == 512) { refuse("original use budget", true); return stats; }
        auto *user = dyn_cast<Instruction>(use.getUser());
        if (!user || user->getFunction() != &function) {
          refuse("nonlocal original decoder use"); return stats;
        }
        originalUses.push_back(&use);
      }
    }
    // Reserve publication before mutation. No per-node budget failure below.
    if (!root || failed || !spend(stats.instructions + plans.size() + originalUses.size() * 2)) return stats;
    std::map<Plan *, Value *> values;
    std::vector<Instruction *> created;
    auto remember = [&](Instruction *i) { created.push_back(i); return i; };
    auto constant = [&](const APInt &a) { return ConstantInt::get(function.getContext(), a); };
    // All PHI placeholders first. No original instruction is modified here.
    for (const auto &node : plans) if (node->kind == Plan::Phi) {
      auto *phi = cast<PHINode>(node->original);
      values[node.get()] = remember(PHINode::Create(IntegerType::get(function.getContext(), resultWidth),
          node->arms.size(), "dispatch.decoded", phi->getParent()->getFirstNonPHIIt()));
    }
    std::function<Value *(Plan *)> materialize = [&](Plan *p) -> Value * {
      if (auto it = values.find(p); it != values.end()) return it->second;
      Value *result = nullptr;
      if (p->kind == Plan::Constant) result = constant(*p->zero);
      else {
        auto *at = cast<Instruction>(p->original);
        auto insertion = std::next(at->getIterator());
        Value *condition = p->condition;
        Value *yes = nullptr, *no = nullptr;
        if (p->kind == Plan::Atom) {
          if (!condition->getType()->isIntegerTy(1))
            condition = remember(new ICmpInst(insertion, ICmpInst::ICMP_NE,
                condition, ConstantInt::get(condition->getType(), 0), "dispatch.bit"));
          yes = constant(*p->one); no = constant(*p->zero);
        } else { yes = materialize(p->arms[0]); no = materialize(p->arms[1]); }
        // Direct construction deliberately retains a poison dependency when
        // both constants are equal. We never replace a finite atom by a fact.
        result = remember(SelectInst::Create(condition, yes, no, "dispatch.decoded", insertion));
      }
      values[p] = result; return result;
    };
    for (const auto &node : plans) if (node->kind == Plan::Phi) {
      auto *old = cast<PHINode>(node->original);
      auto *phi = cast<PHINode>(values.at(node.get()));
      for (unsigned n = 0; n != old->getNumIncomingValues(); ++n)
        phi->addIncoming(materialize(node->arms[n]), old->getIncomingBlock(n));
    }
    auto rollback = [&]() {
      for (auto *use : originalUses) use->set(old);
      selected.setCondition(old);
      for (auto *i : created) i->dropAllReferences();
      for (auto it = created.rbegin(); it != created.rend(); ++it) (*it)->eraseFromParent();
    };
    Value *replacement = materialize(root);
    // The CFG is unchanged, so the original dominance tree remains valid.
    // PHI operands are checked at their actual predecessor terminator.
    for (auto *use : originalUses) {
      auto *user = cast<Instruction>(use->getUser());
      auto *phi = dyn_cast<PHINode>(user);
      Instruction *position = phi ? phi->getIncomingBlock(use->getOperandNo())->getTerminator() : user;
      if (!dom.dominates(replacement, position)) {
        rollback(); refuse("decoded value does not dominate every original use"); return stats;
      }
    }
    for (auto *use : originalUses) use->set(replacement);
    selected.setCondition(replacement);
    std::string error; raw_string_ostream stream(error);
    if (verifyModule(*function.getParent(), &stream)) {
      rollback();
      refuse("publication verification failed: " ); return stats;
    }
    stats.changed = true; stats.message = consumeOriginalUses
        ? "finite scalar decoder consumed by all dominated original uses"
        : "finite scalar decoder distributed over complete SSA edges";
    return stats;
  }
};
} // namespace
static DispatchDecoderSSAStats normalize(Function &f, SwitchInst &s,
                                         const DispatchDecoderSSAOptions &o, bool consume) {
  if (s.getFunction() != &f || !f.getParent() || !s.getCondition()->getType()->isIntegerTy() ||
      s.getCondition()->getType()->getIntegerBitWidth() > 128 ||
      !o.maxNodes || o.maxNodes > 256 || !o.maxPhis || o.maxPhis > 128 ||
      !o.maxInstructions || o.maxInstructions > 512 || !o.maxWork || o.maxWork > 16384)
    return {false, true, false, 0, 0, 0, 0, "invalid selected switch or option bounds"};
  return Planner(f, s, o, consume).run();
}
DispatchDecoderSSAStats normalizeDispatchDecoder(Function &f, SwitchInst &s,
                                                 const DispatchDecoderSSAOptions &o) {
  return normalize(f, s, o, false);
}
DispatchDecoderSSAStats normalizeDispatchDecoderUses(Function &f, SwitchInst &s,
                                                     const DispatchDecoderSSAOptions &o) {
  return normalize(f, s, o, true);
}
} // namespace dragon::llvmlifter::detail
