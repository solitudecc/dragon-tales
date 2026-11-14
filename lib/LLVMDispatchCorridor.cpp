#include "LLVMDispatchCorridor.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/SSAUpdater.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace llvm;
namespace dragon::llvmlifter::detail {
namespace {
constexpr std::size_t MaxPathBlocks = 6, MaxPathInstructions = 256;
constexpr std::size_t MaxLiveOuts = 128, MaxExternalUses = 8192;
constexpr std::size_t MaxFunctionBlocks = 4096, MaxFunctionInstructions = 65536;
bool literalBad(Value *v) { return isa<UndefValue, PoisonValue>(v); }
bool scalar(Type *t) { return t->isIntegerTy() || t->isPointerTy(); }
struct Work {
  DispatchCorridorStats &stats;
  const DispatchCorridorOptions &options;
  bool spend(std::size_t n = 1) {
    if (n > options.maxWork - stats.work) {
      stats.complete = false; stats.budgetExhausted = true;
      stats.message = "dispatch corridor work exhausted"; return false;
    }
    stats.work += n; return true;
  }
};
struct HistoricalUse { Instruction *instruction; unsigned operand; Value *value; };
struct FrozenEdge { unsigned source, slot; std::vector<unsigned> path; };
struct Plan {
  DispatchCorridorEdge edge;
  std::map<BasicBlock *, unsigned> position;
  std::vector<Instruction *> definitions;
  std::vector<Value *> snapshots;
  std::vector<HistoricalUse> historicalUses;
  std::map<PHINode *, Value *> phiInputs;
  std::set<PHINode *> historicalPhiInputs;
  std::map<std::pair<BasicBlock *, PHINode *>, std::pair<Value *, bool>> exitInputs;
  std::vector<APInt> states;
  std::size_t instructions = 0, repairCells = 0;
  bool mayPoison = false;
  bool boundsSelfEdge = false;
};
Value *incoming(PHINode &phi, BasicBlock &from) {
  Value *value = nullptr;
  for (unsigned n = 0; n != phi.getNumIncomingValues(); ++n)
    if (phi.getIncomingBlock(n) == &from) {
      if (value && value != phi.getIncomingValue(n)) return nullptr;
      value = phi.getIncomingValue(n);
    }
  return value;
}
bool repairUse(const Use &use, const BasicBlock &definitionBlock) {
  auto *user = dyn_cast<Instruction>(use.getUser());
  if (!user || user->getParent() != &definitionBlock) return true;
  auto *phi = dyn_cast<PHINode>(user);
  return phi && phi->getIncomingBlock(use.getOperandNo()) != &definitionBlock;
}
// Batch-only concrete finite-domain evaluation. Supported flags are checked
// for EVERY atom assignment; the copied instructions retain all original
// flags. No range/definedness fact is obtained by silently dropping a flag.
std::optional<APInt> batchCast(CastInst &i, const APInt &a) {
  const unsigned width = i.getType()->getIntegerBitWidth();
  switch (i.getOpcode()) {
  case Instruction::ZExt:
    if (i.hasNonNeg() && a.isNegative()) return {};
    return a.zext(width);
  case Instruction::SExt: return a.sext(width);
  case Instruction::Trunc: {
    auto result = a.trunc(width);
    if (i.hasNoUnsignedWrap() && result.zext(a.getBitWidth()) != a) return {};
    if (i.hasNoSignedWrap() && result.sext(a.getBitWidth()) != a) return {};
    return result;
  }
  default: return {};
  }
}
std::optional<APInt> batchBinary(BinaryOperator &i, const APInt &a, const APInt &b) {
  bool unsignedOverflow = false, signedOverflow = false;
  APInt result(a.getBitWidth(), 0);
  switch (i.getOpcode()) {
  case Instruction::Add:
    result = a.uadd_ov(b, unsignedOverflow); (void)a.sadd_ov(b, signedOverflow); break;
  case Instruction::Sub:
    result = a.usub_ov(b, unsignedOverflow); (void)a.ssub_ov(b, signedOverflow); break;
  case Instruction::Mul:
    result = a.umul_ov(b, unsignedOverflow); (void)a.smul_ov(b, signedOverflow); break;
  case Instruction::And: return a & b;
  case Instruction::Or:
    if (auto *op = dyn_cast<PossiblyDisjointInst>(&i);
        op && op->isDisjoint() && !(a & b).isZero()) return {};
    return a | b;
  case Instruction::Xor: return a ^ b;
  case Instruction::Shl:
    if (b.uge(a.getBitWidth())) return {};
    result = a.ushl_ov(b, unsignedOverflow); (void)a.sshl_ov(b, signedOverflow); break;
  case Instruction::LShr: case Instruction::AShr: {
    if (b.uge(a.getBitWidth())) return {};
    const unsigned shift = static_cast<unsigned>(b.getZExtValue());
    if (i.isExact() && !a.getLoBits(shift).isZero()) return {};
    return i.getOpcode() == Instruction::LShr ? a.lshr(shift) : a.ashr(shift);
  }
  default: return {};
  }
  if ((i.hasNoUnsignedWrap() && unsignedOverflow) ||
      (i.hasNoSignedWrap() && signedOverflow)) return {};
  return result;
}
// Exact full one-bit domain, not sample execution. The original selector is
// still consumed at its old switch point, so this exports no definedness fact.
class StateValues {
  Plan &plan;
  Work &work;
  enum class View { Current, Historical };
  using ViewedValue = std::pair<Value *, View>;
  std::set<ViewedValue> active, nodes;
  std::optional<ViewedValue> atom;
  bool failed = false;
  bool batch = false;
  bool integer(Value *v) {
    return v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() <= 128;
  }
  bool observe(Value *v) {
    // A flagged i1 cast/binary is not an opaque observation: treating it as
    // the atom would skip the very flag proof requested for this operation.
    // The legacy admission path is intentionally unchanged.
    if (batch)
      if (auto *i = dyn_cast<Instruction>(v); i && i->hasPoisonGeneratingFlags()) return false;
    if (v->getType()->isIntegerTy(1) && !isa<Constant>(v)) return true;
    auto *i = dyn_cast<BinaryOperator>(v);
    if (!i || i->getOpcode() != Instruction::And) return false;
    auto *a = dyn_cast<ConstantInt>(i->getOperand(0));
    auto *b = dyn_cast<ConstantInt>(i->getOperand(1));
    return (a && a->isOne() && !literalBad(i->getOperand(1))) ||
           (b && b->isOne() && !literalBad(i->getOperand(0)));
  }
  std::optional<APInt> get(Value *v, bool bit, unsigned depth,
                           View view = View::Current) {
    if (depth > 32) {
      work.stats.budgetExhausted = true; work.stats.complete = false;
      failed = true; return {};
    }
    if (!work.spend() || !integer(v) || literalBad(v)) {
      failed = true; return {};
    }
    // A pre-seed instruction outside the copied region is an observation of
    // the old execution. Never reach through it to a future header's PHI arm.
    if (auto *i = dyn_cast<Instruction>(v); i && !plan.position.contains(i->getParent()))
      view = View::Historical;
    ViewedValue key{v, view};
    if (nodes.insert(key).second && nodes.size() > 256) {
      work.stats.budgetExhausted = true; work.stats.complete = false;
      failed = true; return {};
    }
    if (auto *c = dyn_cast<ConstantInt>(v)) return c->getValue();
    if (!active.insert(key).second) { failed = true; return {}; }
    auto done = [&](std::optional<APInt> result) {
      active.erase(key); if (!result) failed = true; return result;
    };
    // Exhaustive AND1/i1 domains do not need a numeric value for their input.
    // Current and historical executions of the same SSA instruction are
    // distinct observations and cannot share an assumed bit assignment.
    if (observe(v)) {
      if (atom && *atom != key) return done({});
      atom = key; plan.mayPoison = true;
      return done(APInt(v->getType()->getIntegerBitWidth(), bit));
    }
    if (auto *phi = dyn_cast<PHINode>(v)) {
      // Conservative historical boundary: old numeric PHIs are not expanded
      // using the selected incoming of their fresh, later cloned execution.
      if (view == View::Historical) return done({});
      auto it = plan.phiInputs.find(phi);
      return done(it == plan.phiInputs.end() ? std::optional<APInt>{}
          : get(it->second, bit, depth + 1, plan.historicalPhiInputs.contains(phi)
                                                ? View::Historical : View::Current));
    }
    auto *i = dyn_cast<Instruction>(v);
    // The first prototype deliberately does not infer safety of flagged
    // arithmetic. Original flags/ops are retained by the corridor copy.
    if (!i || (!batch && i->hasPoisonGeneratingFlags())) return done({});
    auto operand = [&](unsigned n) {
      View nextView = view;
      if (view == View::Current)
        for (const auto &use : plan.historicalUses)
          if (use.instruction == i && use.operand == n) { nextView = View::Historical; break; }
      return get(i->getOperand(n), bit, depth + 1, nextView);
    };
    if (auto *s = dyn_cast<SelectInst>(i)) {
      auto condition = operand(0);
      auto yes = operand(1);
      auto no = operand(2);
      if (!condition || !yes || !no) return done({});
      return done(condition->isZero() ? no : yes);
    }
    if (auto *c = dyn_cast<CastInst>(i)) {
      auto value = operand(0);
      if (!value) return done({});
      if (batch) return done(batchCast(*c, *value));
      unsigned width = c->getType()->getIntegerBitWidth();
      switch (c->getOpcode()) {
      case Instruction::ZExt: return done(value->zext(width));
      case Instruction::SExt: return done(value->sext(width));
      case Instruction::Trunc: return done(value->trunc(width));
      default: return done({});
      }
    }
    auto *binary = dyn_cast<BinaryOperator>(i);
    if (!binary || (!batch && !isa<ConstantInt>(i->getOperand(0)) &&
                              !isa<ConstantInt>(i->getOperand(1)))) return done({});
    auto a = operand(0);
    auto b = operand(1);
    if (!a || !b) return done({});
    if (batch) return done(batchBinary(*binary, *a, *b));
    switch (i->getOpcode()) {
    case Instruction::Add: return done(*a + *b);
    case Instruction::Sub: return done(*a - *b);
    case Instruction::Mul: return done(*a * *b);
    case Instruction::And: return done(*a & *b);
    case Instruction::Or: return done(*a | *b);
    case Instruction::Xor: return done(*a ^ *b);
    case Instruction::Shl:
      return done(b->uge(a->getBitWidth()) ? std::optional<APInt>{} : a->shl(*b));
    case Instruction::LShr:
      return done(b->uge(a->getBitWidth()) ? std::optional<APInt>{} : a->lshr(*b));
    default: return done({});
    }
  }
public:
  StateValues(Plan &p, Work &w, bool batchMode = false) : plan(p), work(w), batch(batchMode) {}
  bool run(Value *selector) {
    auto a = get(selector, false, 0);
    auto b = get(selector, true, 0);
    if (failed || !a || !b) return false;
    plan.states.push_back(*a);
    if (*a != *b) plan.states.push_back(*b);
    // A singleton obtained through a possibly poison observation cannot drop
    // the original control dependency. Keep that candidate's original switch.
    return plan.states.size() == 2 || !plan.mayPoison;
  }
};
BasicBlock *destination(SwitchInst &, const APInt &);
bool preflight(Function &f, const DispatchCorridorEdge &edge,
               DispatchCorridorOptions const &options, Work &work, Plan &plan,
               std::string &reason, bool batch = false) {
  auto reject = [&](const char *message) { reason = message; return false; };
  if (!edge.source || edge.source->getParent() != &f || edge.path.empty())
    return reject("invalid original corridor path");
  if (edge.path.size() > MaxPathBlocks) {
    work.stats.budgetExhausted = true; return reject("corridor block cap");
  }
  auto *seedTerm = edge.source->getTerminator();
  if (!isa<BranchInst, SwitchInst>(seedTerm) || edge.successorSlot >= seedTerm->getNumSuccessors() ||
      seedTerm->getSuccessor(edge.successorSlot) != edge.path.front())
    return reject("original incoming edge no longer matches");
  plan.edge = edge;
  for (unsigned n = 0; n != edge.path.size(); ++n) {
    auto *b = edge.path[n];
    if (!b || b->getParent() != &f || b == edge.source || !plan.position.emplace(b, n).second)
      return reject("non-simple original corridor path");
  }
  auto *sw = dyn_cast<SwitchInst>(edge.path.back()->getTerminator());
  if (!sw || !sw->getCondition()->getType()->isIntegerTy() ||
      sw->getCondition()->getType()->getIntegerBitWidth() > 128)
    return reject("last original block is not a scalar switch");
  if (sw->getNumCases() > 512) {
    work.stats.budgetExhausted = true; return reject("original switch case cap");
  }
  DominatorTree dom(f);
  if (!dom.isReachableFromEntry(edge.source)) return reject("unreachable original source edge");
  for (unsigned n = 0; n != edge.path.size(); ++n) {
    auto *b = edge.path[n];
    if (b->hasAddressTaken() || b->isEHPad()) return reject("unsupported corridor block identity");
    if (b->size() > MaxPathInstructions - plan.instructions) {
      work.stats.budgetExhausted = true; return reject("corridor instruction cap");
    }
    plan.instructions += b->size();
    auto *term = b->getTerminator();
    if (n + 1 != edge.path.size()) {
      auto *branch = dyn_cast<BranchInst>(term);
      if (!branch) return reject("intermediate corridor terminator is not a branch");
      bool continues = false;
      for (auto *next : successors(b)) {
        if (!work.spend()) return false;
        if (next == edge.path[n + 1]) continues = true;
        else if (plan.position.contains(next)) {
          if (!batch || n || next != b || !branch->isConditional())
            return reject("nonlinear corridor reentry");
          plan.boundsSelfEdge = true;
        }
      }
      if (!continues) return reject("corridor does not contain an actual continuing edge");
    } else if (!batch) for (auto *next : successors(b))
      if (plan.position.contains(next)) return reject("switch successor reenters copied region");
    BasicBlock *from = n ? edge.path[n - 1] : edge.source;
    for (auto &phi : b->phis()) {
      auto *value = incoming(phi, *from);
      if (!value || literalBad(value)) return reject("missing or unsafe selected PHI input");
      plan.phiInputs[&phi] = value;
    }
    for (auto &i : *b) {
      if (!work.spend(1 + i.getNumOperands())) return false;
      if (i.isUsedByMetadata() || isa<CallBase, AllocaInst, FenceInst, AtomicRMWInst,
                                     AtomicCmpXchgInst, FreezeInst>(i) || i.getType()->isTokenTy())
        return reject("unsupported corridor observation or effect");
      if (auto *load = dyn_cast<LoadInst>(&i); load && !load->isSimple())
        return reject("nonsimple corridor LOAD");
      if (auto *store = dyn_cast<StoreInst>(&i); store && !store->isSimple())
        return reject("nonsimple corridor STORE");
      if (!isa<LoadInst, StoreInst>(i) && !i.isTerminator() &&
          (i.mayReadOrWriteMemory() || i.mayHaveSideEffects()))
        return reject("unknown corridor side effect");
      if (!i.getType()->isVoidTy()) {
        if (!scalar(i.getType())) return reject("unsupported corridor SSA type");
        plan.definitions.push_back(&i);
      }
      if (auto *phi = dyn_cast<PHINode>(&i)) {
        auto *v = plan.phiInputs.at(phi);
        if (auto *definition = dyn_cast<Instruction>(v)) {
          auto pos = plan.position.find(definition->getParent());
          const bool fresh = n && pos != plan.position.end() && pos->second < n;
          if (!fresh) {
            if (!dom.dominates(definition, seedTerm)) return reject("historical PHI input does not dominate seed");
            plan.historicalPhiInputs.insert(phi);
            if (std::find(plan.snapshots.begin(), plan.snapshots.end(), v) == plan.snapshots.end())
              plan.snapshots.push_back(v);
          }
        }
        continue;
      }
      for (auto &use : i.operands()) {
        auto *v = use.get();
        if (literalBad(v)) return reject("literal undef or poison corridor operand");
        auto *definition = dyn_cast<Instruction>(v);
        if (!definition) continue;
        auto pos = plan.position.find(definition->getParent());
        const bool fresh = pos != plan.position.end() &&
            (pos->second < n || (pos->second == n && definition->comesBefore(&i)));
        if (fresh) continue;
        if (!dom.dominates(definition, seedTerm)) return reject("external or historical use does not dominate seed");
        // Explicit per-Use history protects an old later-header value from a
        // global VMap that also maps its fresh, later execution.
        if (pos != plan.position.end()) {
          plan.historicalUses.push_back({&i, use.getOperandNo(), v});
          if (std::find(plan.snapshots.begin(), plan.snapshots.end(), v) == plan.snapshots.end())
            plan.snapshots.push_back(v);
        }
      }
    }
  }
  // A fault before a later header must carry the OLD header value. Mapping
  // its exit PHI through the global fresh VMap would silently use the next
  // iteration's value while still producing verifier-valid IR.
  for (unsigned n = 0; n != edge.path.size(); ++n) {
    auto *block = edge.path[n];
    for (auto *target : successors(block)) {
      if (plan.position.contains(target)) continue;
      for (auto &phi : target->phis()) {
        if (!work.spend()) return false;
        auto *value = incoming(phi, *block);
        if (!value || literalBad(value)) return reject("missing or unsafe external exit PHI input");
        bool historical = false;
        if (auto *definition = dyn_cast<Instruction>(value)) {
          auto pos = plan.position.find(definition->getParent());
          const bool fresh = pos != plan.position.end() &&
              (pos->second < n || (pos->second == n && definition->comesBefore(block->getTerminator())));
          if (!fresh && !dom.dominates(definition, seedTerm))
            return reject("external exit PHI input does not dominate seed");
          historical = !fresh && pos != plan.position.end();
          if (historical && std::find(plan.snapshots.begin(), plan.snapshots.end(), value) == plan.snapshots.end())
            plan.snapshots.push_back(value);
        }
        plan.exitInputs[{block, &phi}] = {value, historical};
      }
    }
  }
  if (!StateValues(plan, work, batch).run(sw->getCondition())) return reject("selector lacks a complete safe finite state proof");
  if (batch) for (const auto &state : plan.states) {
    if (!work.spend(1 + sw->getNumCases())) return false;
    if (plan.position.contains(destination(*sw, state)))
      return reject("proved switch destination reenters copied region");
  }
  if (plan.boundsSelfEdge) {
    auto *head = edge.path.front();
    auto *branch = cast<BranchInst>(head->getTerminator());
    auto *compare = dyn_cast<ICmpInst>(branch->getCondition());
    if (!compare || compare->getParent() != head || compare->hasPoisonGeneratingFlags())
      return reject("bounds self-edge lacks an original pure integer comparison");
    for (auto &i : *head) {
      if (!work.spend(1 + i.getNumOperands())) return false;
      if (&i == branch) continue;
      if ((!isa<PHINode, BinaryOperator, ICmpInst, ZExtInst, SExtInst, TruncInst, SelectInst>(i)) ||
          !i.getType()->isIntegerTy() || i.getType()->getIntegerBitWidth() > 128)
        return reject("bounds self-edge head is not pure integer code");
      if (auto *phi = dyn_cast<PHINode>(&i)) {
        auto *self = incoming(*phi, *head);
        if (!self || literalBad(self)) return reject("unsafe bounds self-edge PHI input");
      }
    }
    auto *selector = sw->getCondition();
    unsigned stateOperand = 0;
    auto *bound = dyn_cast<ConstantInt>(compare->getOperand(1));
    if (compare->getOperand(0) != selector || !bound) {
      stateOperand = 1; bound = dyn_cast<ConstantInt>(compare->getOperand(0));
      if (compare->getOperand(1) != selector || !bound)
        return reject("bounds state differs from the proved original selector");
    }
    // The same Value* may mean an old later-header execution at the bounds
    // use and its fresh execution at the switch. Such a proof is not shared.
    for (const auto &use : plan.historicalUses)
      if (use.instruction == compare && use.operand == stateOperand)
        return reject("bounds state is a different historical observation");
    for (const auto &state : plan.states) {
      if (state.getBitWidth() != bound->getBitWidth()) return reject("bounds state width mismatch");
      const APInt &a = stateOperand ? bound->getValue() : state;
      const APInt &b = stateOperand ? state : bound->getValue();
      bool selected;
      switch (compare->getPredicate()) {
      case ICmpInst::ICMP_ULT: selected = a.ult(b); break;
      case ICmpInst::ICMP_ULE: selected = a.ule(b); break;
      case ICmpInst::ICMP_UGT: selected = a.ugt(b); break;
      case ICmpInst::ICMP_UGE: selected = a.uge(b); break;
      default: return reject("unsupported bounds self-edge predicate");
      }
      if (!work.spend() || branch->getSuccessor(selected ? 0 : 1) != edge.path[1])
        return reject("proved starting state does not select the bounds continuation");
    }
  }
  std::size_t live = 0, externalUses = plan.snapshots.size();
  for (auto *definition : plan.definitions) {
    bool used = false;
    for (auto &use : definition->uses()) {
      if (!work.spend()) return false;
      if (!repairUse(use, *definition->getParent())) continue;
      auto *user = dyn_cast<Instruction>(use.getUser());
      if (!user || user->getFunction() != &f || user->getParent()->isEHPad())
        return reject("unsupported external SSA use");
      used = true;
      if (++externalUses > MaxExternalUses) {
        work.stats.budgetExhausted = true; return reject("external use cap");
      }
    }
    if (used && ++live > MaxLiveOuts) {
      work.stats.budgetExhausted = true; return reject("liveout value cap");
    }
  }
  const auto blocks = edge.path.size() + !plan.snapshots.empty();
  plan.repairCells = live * (f.size() + blocks);
  // A batch prepays one union repair footprint after every plan is proved.
  // Per-edge repair/growth accounting remains exactly the legacy path below.
  if (batch) return true;
  const auto instructions = plan.instructions + plan.snapshots.size() +
                            !plan.snapshots.empty() + (plan.states.size() == 2);
  auto &stats = work.stats;
  if (stats.rewrittenEdges == options.maxRewrites || blocks > options.maxClonedBlocks - stats.clonedBlocks ||
      instructions > options.maxClonedInstructions - stats.clonedInstructions ||
      plan.repairCells > options.maxRepairCells - stats.repairCells) {
    stats.budgetExhausted = true; return reject("shared corridor growth or repair budget");
  }
  // Prepay the complete bounded SSA repair footprint before touching even the
  // owned trial. No lazy retry raises these caps after copying.
  if (!work.spend(instructions + plan.repairCells)) return false;
  return true;
}
BasicBlock *destination(SwitchInst &sw, const APInt &state) {
  for (auto item : sw.cases()) if (item.getCaseValue()->getValue() == state) return item.getCaseSuccessor();
  return sw.getDefaultDest();
}
bool apply(Plan &plan, DispatchCorridorStats &stats) {
  auto &f = *plan.edge.source->getParent();
  ValueToValueMapTy map;
  std::map<Value *, Value *> old;
  BasicBlock *bridge = nullptr;
  if (!plan.snapshots.empty()) {
    bridge = BasicBlock::Create(f.getContext(), "dispatch.history", &f);
    for (auto *value : plan.snapshots) {
      auto *snapshot = PHINode::Create(value->getType(), 1, "dispatch.old", bridge);
      snapshot->addIncoming(value, plan.edge.source); old[value] = snapshot;
      map[snapshot] = snapshot;
    }
  }
  std::vector<BasicBlock *> copies;
  std::map<Instruction *, Instruction *> copiedInstruction;
  for (auto *b : plan.edge.path) {
    auto *copy = CloneBasicBlock(b, map, ".dispatch.edge", &f);
    map[b] = copy; copies.push_back(copy);
    auto origin = b->begin();
    for (auto &i : *copy) copiedInstruction[&*origin++] = &i;
  }
  auto selectedInput = [&](Value *value, unsigned block) -> Value * {
    // A chosen PHI input from an earlier copied block is fresh. Everything
    // snapshotted for this edge belongs to the old execution instead.
    if (auto *instruction = dyn_cast<Instruction>(value)) {
      auto pos = plan.position.find(instruction->getParent());
      if (block && pos != plan.position.end() && pos->second < block)
        return map.lookup(value);
    }
    if (auto it = old.find(value); it != old.end()) return it->second;
    return value;
  };
  for (unsigned n = 0; n != plan.edge.path.size(); ++n)
    for (auto &phi : plan.edge.path[n]->phis()) map[&phi] = selectedInput(plan.phiInputs.at(&phi), n);
  for (auto *copy : copies)
    while (auto *phi = dyn_cast<PHINode>(&copy->front())) phi->eraseFromParent();
  for (const auto &use : plan.historicalUses) {
    auto *copy = copiedInstruction.at(use.instruction);
    copy->setOperand(use.operand, old.at(use.value));
  }
  for (auto *copy : copies) for (auto &i : *copy)
    RemapInstruction(&i, map, RF_IgnoreMissingLocals | RF_NoModuleLevelChanges);
  auto *originalSwitch = cast<SwitchInst>(plan.edge.path.back()->getTerminator());
  auto *newSwitch = cast<SwitchInst>(copies.back()->getTerminator());
  auto *selector = newSwitch->getCondition();
  auto *first = destination(*originalSwitch, plan.states.front());
  if (plan.states.size() == 1) BranchInst::Create(first, newSwitch);
  else {
    auto *second = destination(*originalSwitch, plan.states.back());
    auto *condition = new ICmpInst(newSwitch, ICmpInst::ICMP_EQ, selector,
        ConstantInt::get(f.getContext(), plan.states.front()), "dispatch.choose");
    BranchInst::Create(first, second, condition, newSwitch);
  }
  newSwitch->eraseFromParent();
  // Only actual NEW successor slots count. A switch may have many old slots
  // to one destination while the replacement branch has one or two.
  for (unsigned n = 0; n != copies.size(); ++n) {
    auto *original = plan.edge.path[n]; auto *copy = copies[n];
    for (auto *next : successors(copy)) {
      if (std::find(copies.begin(), copies.end(), next) != copies.end()) continue;
      for (auto &phi : next->phis()) {
        auto expected = plan.exitInputs.find({original, &phi});
        if (expected == plan.exitInputs.end()) return false;
        auto *value = expected->second.first;
        if (expected->second.second) value = old.at(value);
        else if (auto found = map.find(value); found != map.end()) value = found->second;
        phi.addIncoming(value, copy);
      }
    }
  }
  if (bridge) BranchInst::Create(copies.front(), bridge);
  plan.edge.source->getTerminator()->setSuccessor(plan.edge.successorSlot, bridge ? bridge : copies.front());
  for (auto &phi : plan.edge.path.front()->phis()) {
    int index = phi.getBasicBlockIndex(plan.edge.source); if (index < 0) return false;
    phi.removeIncomingValue(static_cast<unsigned>(index), false);
  }
  for (auto *definition : plan.definitions) {
    SmallVector<Use *, 32> uses;
    for (auto &use : definition->uses()) {
      auto *user = dyn_cast<Instruction>(use.getUser());
      // Copied current uses were already remapped; history snapshots remain
      // original-definition users and must follow the actual old execution.
      if (user && std::find(copies.begin(), copies.end(), user->getParent()) != copies.end()) continue;
      if (repairUse(use, *definition->getParent())) uses.push_back(&use);
    }
    if (uses.empty()) continue;
    SmallVector<PHINode *, 8> phis;
    SSAUpdater updater(&phis);
    updater.Initialize(definition->getType(), "dispatch.live");
    updater.AddAvailableValue(definition->getParent(), definition);
    auto *copyBlock = cast<BasicBlock>(map.lookup(definition->getParent()));
    Value *copyValue = map.lookup(definition); if (!copyBlock || !copyValue) return false;
    updater.AddAvailableValue(copyBlock, copyValue);
    for (auto *use : uses) updater.RewriteUse(*use);
    // SSAUpdater may use undef for a missing reaching definition. Such an
    // invented value is never an admitted repair, even if verify would pass.
    for (auto *phi : phis) for (auto &incoming : phi->incoming_values())
      if (literalBad(incoming)) return false;
    stats.repairPhis += phis.size();
  }
  stats.clonedBlocks += copies.size() + (bridge != nullptr);
  stats.clonedInstructions += plan.instructions + plan.snapshots.size() + (bridge != nullptr) + (plan.states.size() == 2);
  stats.snapshots += plan.snapshots.size(); stats.repairCells += plan.repairCells;
  ++stats.rewrittenEdges;
  return true;
}
void rebind(Function &body, Function &from, Function &to) {
  for (unsigned n = 0; n != from.arg_size(); ++n) {
    auto *value = from.getArg(n);
    for (auto it = value->use_begin(); it != value->use_end();) {
      auto &use = *it++; auto *i = dyn_cast<Instruction>(use.getUser());
      if (i && i->getFunction() == &body) use.set(to.getArg(n));
    }
  }
}
struct NamedMetadataSnapshot {
  Module &module;
  std::map<NamedMDNode *, std::vector<MDNode *>> operands;
  explicit NamedMetadataSnapshot(Module &m) : module(m) {
    for (auto &named : m.named_metadata()) {
      auto &saved = operands[&named];
      for (auto *node : named.operands()) saved.push_back(node);
    }
  }
  void restore() {
    for (auto it = module.named_metadata_begin(); it != module.named_metadata_end();) {
      auto *named = &*it++;
      if (!operands.contains(named)) named->eraseFromParent();
    }
    for (const auto &[named, saved] : operands) {
      named->clearOperands();
      for (auto *node : saved) named->addOperand(node);
    }
  }
};
bool commit(Function &original, Function &trial, unsigned forceFailure) {
  Module &module = *original.getParent();
  NamedMetadataSnapshot metadata(module);
  ValueToValueMapTy map;
  for (auto &global : trial.getParent()->global_values()) {
    auto *binding = module.getNamedValue(global.getName());
    if (!binding || binding->getType() != global.getType()) return false;
    map[&global] = binding;
  }
  auto *copy = Function::Create(original.getFunctionType(), GlobalValue::ExternalLinkage,
                                "dispatch.commit", module);
  unsigned n = 0; for (auto &arg : trial.args()) map[&arg] = copy->getArg(n++);
  SmallVector<ReturnInst *, 8> returns;
  CloneFunctionInto(copy, &trial, map, CloneFunctionChangeType::DifferentModule, returns);
  copy->copyAttributesFrom(&original);
  copy->setCallingConv(original.getCallingConv()); copy->setAttributes(original.getAttributes());
  copy->setLinkage(GlobalValue::ExternalLinkage); copy->setComdat(nullptr);
  // DifferentModule can create/append llvm.dbg.cu even without a Subprogram.
  // This pass exports no new debug information or module metadata.
  metadata.restore();
  if (forceFailure == 1 || verifyModule(module)) {
    copy->eraseFromParent(); metadata.restore(); return false;
  }
  auto *backup = Function::Create(original.getFunctionType(), GlobalValue::ExternalLinkage,
                                  "dispatch.rollback", module);
  backup->copyAttributesFrom(&original);
  backup->setCallingConv(original.getCallingConv()); backup->setAttributes(original.getAttributes());
  backup->setLinkage(GlobalValue::ExternalLinkage); backup->setComdat(nullptr);
  backup->splice(backup->end(), &original); rebind(*backup, original, *backup);
  for (unsigned a = 0; a != copy->arg_size(); ++a) copy->getArg(a)->replaceAllUsesWith(original.getArg(a));
  original.splice(original.end(), copy);
  if (forceFailure == 2 || verifyModule(module) || !copy->use_empty() ||
      !backup->use_empty() || copy->isUsedByMetadata() || backup->isUsedByMetadata()) {
    copy->splice(copy->end(), &original); original.splice(original.end(), backup);
    rebind(original, *backup, original); backup->eraseFromParent(); copy->eraseFromParent();
    metadata.restore(); return false;
  }
  // Both unreferenced carriers are valid external functions, including the
  // now-empty copy declaration. Function/argument/local-metadata identities
  // were checked before trial; neither carrier has escaping users.
  // Verification and every fallible check occur while the original body is
  // still available. Erasing unused functions with no escaping local identity
  // cannot invalidate the already-verified remaining module. Do not introduce
  // a late failure return after destroying the rollback body.
  backup->eraseFromParent(); copy->eraseFromParent();
  return true;
}
bool unsupportedBlockAddress(Value *value, Work &work, std::set<Value *> &seen,
                              unsigned depth = 0) {
  if (!isa<Constant>(value) || !seen.insert(value).second) return false;
  if (!work.spend() || depth > 32) {
    work.stats.budgetExhausted = true; return true;
  }
  if (isa<BlockAddress>(value)) return true;
  if (isa<GlobalValue>(value)) return false;
  auto *constant = cast<Constant>(value);
  for (auto &operand : constant->operands())
    if (unsupportedBlockAddress(operand, work, seen, depth + 1)) return true;
  return false;
}
bool unsupportedMetadata(Metadata *metadata, Work &work, std::set<Metadata *> &seen,
                           std::set<Value *> &constants, unsigned depth = 0) {
  if (!metadata || !seen.insert(metadata).second) return false;
  if (!work.spend() || depth > 32) { work.stats.budgetExhausted = true; return true; }
  if (auto *value = dyn_cast<ValueAsMetadata>(metadata)) {
    auto *v = value->getValue();
    return isa<Instruction, Argument, BasicBlock>(v) ||
           unsupportedBlockAddress(v, work, constants);
  }
  if (auto *node = dyn_cast<MDNode>(metadata))
    for (auto &operand : node->operands()) {
      if (!work.spend() || unsupportedMetadata(operand.get(), work, seen, constants, depth + 1))
        return true;
    }
  return false;
}
struct BatchCopy {
  Plan *plan = nullptr;
  ValueToValueMapTy map;
  std::map<Value *, Value *> old;
  std::vector<BasicBlock *> blocks;
  BasicBlock *bridge = nullptr;
};
// No original instruction/edge is changed until all plans and the complete
// shared repair footprint have passed. All maps refer to this one owned trial.
bool applyBatch(Function &f, const std::vector<FrozenEdge> &frozen,
                const std::vector<BasicBlock *> &originalBlocks,
                const DispatchCorridorOptions &options, Work &work) {
  auto &stats = work.stats;
  auto fail = [&](const std::string &message) {
    stats.changed = false; stats.complete = false; stats.rejected = frozen.size();
    stats.rewrittenEdges = stats.clonedBlocks = stats.clonedInstructions = 0;
    stats.snapshots = stats.repairPhis = stats.repairCells = 0;
    stats.message = message; return false;
  };
  std::vector<std::unique_ptr<Plan>> plans;
  std::vector<Instruction *> definitions;
  std::set<Instruction *> seenDefinitions;
  std::size_t blocks = 0, instructions = 0, snapshots = 0;
  for (const auto &item : frozen) {
    ++stats.candidatesVisited;
    DispatchCorridorEdge edge{originalBlocks.at(item.source), item.slot, {}};
    for (auto index : item.path) edge.path.push_back(originalBlocks.at(index));
    auto plan = std::make_unique<Plan>();
    std::string reason;
    if (!preflight(f, edge, options, work, *plan, reason, true))
      return fail(reason.empty() ? stats.message : reason);
    const auto addedBlocks = edge.path.size() + !plan->snapshots.empty();
    const auto addedInstructions = plan->instructions + plan->snapshots.size() +
                                   !plan->snapshots.empty() + (plan->states.size() == 2);
    if (plans.size() == options.maxRewrites || addedBlocks > options.maxClonedBlocks - blocks ||
        addedInstructions > options.maxClonedInstructions - instructions) {
      stats.budgetExhausted = true;
      return fail("shared batch copy growth budget");
    }
    blocks += addedBlocks; instructions += addedInstructions; snapshots += plan->snapshots.size();
    for (auto *definition : plan->definitions)
      if (seenDefinitions.insert(definition).second) definitions.push_back(definition);
    plans.push_back(std::move(plan));
  }
  if (plans.empty()) return true;
  // Prepay every original definition, conservatively including definitions
  // whose external use disappears after the simultaneous edge removals. This
  // is one union footprint, never one footprint per candidate/copy.
  const auto finalBlocks = f.size() + blocks;
  if (definitions.size() > options.maxRepairCells / finalBlocks) {
    stats.budgetExhausted = true; return fail("shared batch SSA repair budget");
  }
  const auto repairCells = definitions.size() * finalBlocks;
  if (!work.spend(instructions + repairCells)) return fail(stats.message);
  std::vector<std::unique_ptr<BatchCopy>> copies;
  std::set<BasicBlock *> addedBlocks;
  for (auto &ownedPlan : plans) {
    auto &plan = *ownedPlan;
    auto ownedCopy = std::make_unique<BatchCopy>();
    auto &copy = *ownedCopy; copy.plan = &plan;
    if (!plan.snapshots.empty()) {
      copy.bridge = BasicBlock::Create(f.getContext(), "dispatch.history", &f);
      addedBlocks.insert(copy.bridge);
      for (auto *value : plan.snapshots) {
        auto *snapshot = PHINode::Create(value->getType(), 1, "dispatch.old", copy.bridge);
        snapshot->addIncoming(value, plan.edge.source); copy.old[value] = snapshot;
        copy.map[snapshot] = snapshot;
      }
    }
    std::map<Instruction *, Instruction *> instructionCopies;
    for (auto *block : plan.edge.path) {
      auto *cloned = CloneBasicBlock(block, copy.map, ".dispatch.edge", &f);
      copy.map[block] = cloned; copy.blocks.push_back(cloned); addedBlocks.insert(cloned);
      auto origin = block->begin();
      for (auto &instruction : *cloned) instructionCopies[&*origin++] = &instruction;
    }
    auto selectedInput = [&](Value *value, unsigned block) -> Value * {
      if (auto *instruction = dyn_cast<Instruction>(value)) {
        auto pos = plan.position.find(instruction->getParent());
        if (block && pos != plan.position.end() && pos->second < block)
          return copy.map.lookup(value);
      }
      if (auto it = copy.old.find(value); it != copy.old.end()) return it->second;
      return value;
    };
    for (unsigned n = 0; n != plan.edge.path.size(); ++n)
      for (auto &phi : plan.edge.path[n]->phis())
        copy.map[&phi] = selectedInput(plan.phiInputs.at(&phi), n);
    for (auto *block : copy.blocks)
      while (auto *phi = dyn_cast<PHINode>(&block->front())) phi->eraseFromParent();
    for (const auto &use : plan.historicalUses)
      instructionCopies.at(use.instruction)->setOperand(use.operand, copy.old.at(use.value));
    for (auto *block : copy.blocks) for (auto &instruction : *block)
      RemapInstruction(&instruction, copy.map, RF_IgnoreMissingLocals | RF_NoModuleLevelChanges);
    if (plan.boundsSelfEdge) {
      auto *branch = cast<BranchInst>(copy.blocks.front()->getTerminator());
      if (plan.states.size() == 1) {
        BranchInst::Create(copy.blocks[1], branch); branch->eraseFromParent();
      } else {
        // Do not introduce an early equality/freeze of the selector. Preserve
        // the ORIGINAL condition consumption here, before any later effects.
        // Both complete numeric states take the original continuation; the
        // duplicate new slots merely eliminate its impossible structural loop.
        branch->setSuccessor(0, copy.blocks[1]);
        branch->setSuccessor(1, copy.blocks[1]);
      }
    }
    auto *originalSwitch = cast<SwitchInst>(plan.edge.path.back()->getTerminator());
    auto *newSwitch = cast<SwitchInst>(copy.blocks.back()->getTerminator());
    auto *first = destination(*originalSwitch, plan.states.front());
    if (plan.states.size() == 1) BranchInst::Create(first, newSwitch);
    else {
      auto *condition = new ICmpInst(newSwitch, ICmpInst::ICMP_EQ, newSwitch->getCondition(),
          ConstantInt::get(f.getContext(), plan.states.front()), "dispatch.choose");
      BranchInst::Create(first, destination(*originalSwitch, plan.states.back()), condition, newSwitch);
    }
    newSwitch->eraseFromParent();
    if (copy.bridge) BranchInst::Create(copy.blocks.front(), copy.bridge);
    copies.push_back(std::move(ownedCopy));
  }
  // All paths have been cloned from the unchanged originals. Now add exactly
  // one PHI incoming per actual NEW successor slot, including duplicate slots.
  for (const auto &ownedCopy : copies) {
    auto &copy = *ownedCopy; auto &plan = *copy.plan;
    for (unsigned n = 0; n != copy.blocks.size(); ++n) {
      auto *original = plan.edge.path[n]; auto *block = copy.blocks[n];
      for (auto *next : successors(block)) {
        if (addedBlocks.contains(next)) continue;
        for (auto &phi : next->phis()) {
          auto expected = plan.exitInputs.find({original, &phi});
          if (expected == plan.exitInputs.end()) return fail("batch external exit PHI lacks a proved input");
          auto *value = expected->second.first;
          if (expected->second.second) value = copy.old.at(value);
          else if (auto found = copy.map.find(value); found != copy.map.end()) value = found->second;
          phi.addIncoming(value, block);
        }
      }
    }
  }
  for (const auto &ownedCopy : copies) {
    auto &copy = *ownedCopy; auto &plan = *copy.plan;
    auto *source = plan.edge.source;
    source->getTerminator()->setSuccessor(plan.edge.successorSlot,
                                         copy.bridge ? copy.bridge : copy.blocks.front());
    for (auto &phi : plan.edge.path.front()->phis()) {
      int index = phi.getBasicBlockIndex(source);
      if (index < 0) return fail("batch source slot lacks its original PHI incoming");
      phi.removeIncomingValue(static_cast<unsigned>(index), false);
    }
  }
  std::map<Instruction *, std::vector<std::pair<BasicBlock *, Value *>>> available;
  for (auto *definition : definitions) for (const auto &ownedCopy : copies) {
    auto &copy = *ownedCopy;
    if (!copy.plan->position.contains(definition->getParent())) continue;
    auto *block = dyn_cast_or_null<BasicBlock>(copy.map.lookup(definition->getParent()));
    Value *value = copy.map.lookup(definition);
    if (!block || !value || literalBad(value))
      return fail("batch copy lacks a safe available definition");
    // An updater may create new PHIs after the all-use snapshot below. Its
    // available values must not introduce a new use of ANOTHER original
    // definition which that definition's snapshot could have missed. Fresh
    // inputs are copies; historical instruction inputs must have a bridge.
    if (auto *instruction = dyn_cast<Instruction>(value);
        instruction && !addedBlocks.contains(instruction->getParent()))
      return fail("batch available value is an unversioned original instruction");
    available[definition].push_back({block, value});
  }
  // Structural mutation is finished. Snapshot EVERY remaining original-value
  // use before any updater runs; PHI operand arrays will no longer be resized.
  // Cross-copy and history uses are intentional users and must not be skipped
  // merely because their instruction happens to live in a copied block.
  std::map<Instruction *, SmallVector<Use *, 32>> uses;
  for (auto *definition : definitions) for (auto &use : definition->uses()) {
    if (!work.spend()) return fail(stats.message);
    if (repairUse(use, *definition->getParent())) uses[definition].push_back(&use);
  }
  for (auto *definition : definitions) {
    auto foundUses = uses.find(definition);
    if (foundUses == uses.end() || foundUses->second.empty()) continue;
    SmallVector<PHINode *, 8> phis;
    SSAUpdater updater(&phis);
    updater.Initialize(definition->getType(), "dispatch.live");
    updater.AddAvailableValue(definition->getParent(), definition);
    for (const auto &[block, value] : available.at(definition))
      updater.AddAvailableValue(block, value);
    for (auto *use : foundUses->second) updater.RewriteUse(*use);
    for (auto *phi : phis) for (auto &value : phi->incoming_values())
      if (literalBad(value)) return fail("batch SSA repair invented an unsafe incoming");
    stats.repairPhis += phis.size();
  }
  stats.rewrittenEdges = plans.size(); stats.clonedBlocks = blocks;
  stats.clonedInstructions = instructions; stats.snapshots = snapshots; stats.repairCells = repairCells;
  return true;
}
// LLVM's removeUnreachableBlocks also constant-folds reachable terminators.
// In particular it erases a conditional br with identical successor slots,
// dropping precisely the bounds condition consumption retained by the batch.
// Here deletion is based only on the actual CFG, without folding instructions.
bool removeBatchUnreachable(Function &f, Work &work) {
  std::set<BasicBlock *> reachable;
  std::vector<BasicBlock *> pending{&f.getEntryBlock()};
  reachable.insert(pending.front());
  while (!pending.empty()) {
    auto *block = pending.back(); pending.pop_back();
    if (!work.spend(1 + block->getTerminator()->getNumSuccessors())) return false;
    for (auto *next : successors(block))
      if (reachable.insert(next).second) pending.push_back(next);
  }
  std::vector<BasicBlock *> dead;
  for (auto &block : f) {
    if (!work.spend()) return false;
    if (!reachable.contains(&block)) dead.push_back(&block);
  }
  // Remove one incoming for each actual dead predecessor slot. Do not invoke
  // removePredecessor's PHI simplification, or any terminator simplification.
  for (auto *block : dead) for (auto *next : successors(block)) {
    if (!work.spend()) return false;
    if (!reachable.contains(next)) continue;
    for (auto &phi : next->phis()) {
      if (!work.spend()) return false;
      int index = phi.getBasicBlockIndex(block);
      if (index < 0) {
        work.stats.message = "dead batch edge lacks its actual PHI slot"; return false;
      }
      phi.removeIncomingValue(static_cast<unsigned>(index), false);
    }
  }
  // A missed reaching definition is a failed trial, never an invented undef
  // or poison. Only after checking all surviving users may dead SCCs be freed.
  for (auto *block : dead) for (auto &instruction : *block) {
    if (!work.spend()) return false;
    for (auto &use : instruction.uses()) {
      if (!work.spend()) return false;
      auto *user = dyn_cast<Instruction>(use.getUser());
      if (!user || reachable.contains(user->getParent())) {
        work.stats.message = "dead batch definition still has a surviving use"; return false;
      }
    }
  }
  for (auto *block : dead) block->dropAllReferences();
  for (auto *block : dead) block->eraseFromParent();
  return true;
}
} // namespace
static DispatchCorridorStats run(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, const DispatchCorridorOptions &options,
    unsigned forceCommitFailure, bool batch = false) {
  DispatchCorridorStats stats; Work work{stats, options};
  auto rejectAll = [&](const char *message) {
    stats.complete = false; stats.rejected = candidates.size(); stats.message = message; return stats;
  };
  if (!f.getParent() || !f.hasName() || f.isDeclaration() || f.hasPersonalityFn() || f.getSubprogram() ||
      !options.maxCandidates || options.maxCandidates > 512 || !options.maxRewrites || options.maxRewrites > (batch ? 512 : 64) ||
      !options.maxClonedBlocks || options.maxClonedBlocks > (batch ? 4096 : 64) ||
      !options.maxClonedInstructions || options.maxClonedInstructions > (batch ? 262144 : 16384) ||
      !options.maxRepairCells || options.maxRepairCells > (batch ? 4194304 : 262144) ||
      !options.maxWork || options.maxWork > (batch ? 16777216 : 1000000))
    return rejectAll("invalid corridor request or option bounds");
  if (batch && candidates.empty()) return stats;
  if (candidates.size() > options.maxCandidates || f.size() > MaxFunctionBlocks) {
    stats.budgetExhausted = true; return rejectAll("original candidate or function block cap");
  }
  std::vector<BasicBlock *> originals;
  std::map<BasicBlock *, unsigned> indexes;
  std::size_t instructions = 0;
  std::set<Value *> constants;
  std::set<Metadata *> metadata;
  SmallVector<std::pair<unsigned, MDNode *>, 8> attachments;
  f.getAllMetadata(attachments);
  for (auto [kind, node] : attachments)
    if (unsupportedMetadata(node, work, metadata, constants))
      return rejectAll("function metadata identity unsupported");
  for (auto &argument : f.args()) if (argument.isUsedByMetadata())
    return rejectAll("local argument metadata identity unsupported");
  for (auto &b : f) {
    if (!work.spend(1 + b.size())) return rejectAll("function scan work cap");
    if (b.hasAddressTaken() || b.isEHPad() || b.isUsedByMetadata())
      return rejectAll("unsupported function block identity");
    if (b.size() > MaxFunctionInstructions - instructions) {
      stats.budgetExhausted = true; return rejectAll("function transaction instruction cap");
    }
    instructions += b.size(); indexes[&b] = originals.size(); originals.push_back(&b);
    for (auto &i : b) {
      if (i.isUsedByMetadata() || i.getDebugLoc()) return rejectAll("local instruction metadata identity unsupported");
      attachments.clear(); i.getAllMetadata(attachments);
      for (auto [kind, node] : attachments)
        if (unsupportedMetadata(node, work, metadata, constants))
          return rejectAll("instruction metadata identity unsupported");
      for (auto &operand : i.operands())
        if (unsupportedBlockAddress(operand, work, constants))
          return rejectAll("blockaddress or constant-identity traversal unsupported");
    }
  }
  if (verifyModule(*f.getParent())) return rejectAll("invalid input module");
  std::size_t namedOperands = 0;
  for (auto &named : f.getParent()->named_metadata()) {
    if (named.getNumOperands() > 65536 - namedOperands ||
        !work.spend(1 + named.getNumOperands())) {
      stats.budgetExhausted = true; return rejectAll("named metadata transaction cap");
    }
    namedOperands += named.getNumOperands();
  }
  std::size_t moduleInstructions = 0, moduleBlocks = 0;
  for (auto &function : *f.getParent()) for (auto &b : function) {
    if (++moduleBlocks > 65536 || b.size() > 1000000 - moduleInstructions || !work.spend()) {
      stats.budgetExhausted = true; return rejectAll("complete-module trial cap");
    }
    moduleInstructions += b.size();
  }
  std::vector<FrozenEdge> frozen;
  for (const auto &edge : candidates) {
    if (!indexes.contains(edge.source)) return rejectAll("candidate does not belong to the original function");
    FrozenEdge item{indexes.at(edge.source), edge.successorSlot, {}};
    for (auto *block : edge.path) {
      if (!indexes.contains(block)) return rejectAll("path does not belong to the original function");
      item.path.push_back(indexes.at(block));
    }
    frozen.push_back(std::move(item));
  }
  std::stable_sort(frozen.begin(), frozen.end(), [](const auto &a, const auto &b) {
    return std::pair(a.source, a.slot) < std::pair(b.source, b.slot);
  });
  for (unsigned n = 1; n < frozen.size(); ++n)
    if (frozen[n - 1].source == frozen[n].source && frozen[n - 1].slot == frozen[n].slot)
      return rejectAll("duplicate original candidate edge");
  if (batch) {
    std::set<unsigned> sources;
    for (const auto &item : frozen) sources.insert(item.source);
    for (const auto &item : frozen) for (auto block : item.path)
      if (sources.contains(block)) return rejectAll("batch candidate source intersects an original copied path");
  }
  auto trial = CloneModule(*f.getParent());
  auto *function = trial->getFunction(f.getName());
  if (!function || function->getFunctionType() != f.getFunctionType())
    return rejectAll("original function missing from owned trial");
  std::vector<BasicBlock *> blocks; for (auto &b : *function) blocks.push_back(&b);
  if (batch) {
    if (!applyBatch(*function, frozen, blocks, options, work)) return stats;
  } else for (const auto &item : frozen) {
    ++stats.candidatesVisited;
    DispatchCorridorEdge edge{blocks.at(item.source), item.slot, {}};
    for (auto index : item.path) edge.path.push_back(blocks.at(index));
    Plan plan; std::string reason;
    if (!preflight(*function, edge, options, work, plan, reason)) {
      stats.complete = false; ++stats.rejected;
      if (!reason.empty()) stats.message = reason;
      if (stats.budgetExhausted) break;
      continue;
    }
    if (!apply(plan, stats)) {
      stats.changed = false; stats.complete = false;
      stats.rewrittenEdges = stats.clonedBlocks = stats.clonedInstructions = stats.snapshots = stats.repairPhis = 0;
      stats.message = "owned trial SSA application failed; original module unchanged"; return stats;
    }
  }
  if (!stats.rewrittenEdges) return stats;
  if (batch) {
    if (!removeBatchUnreachable(*function, work)) {
      stats.changed = false; stats.complete = false; stats.rejected = candidates.size();
      stats.rewrittenEdges = stats.clonedBlocks = stats.clonedInstructions = 0;
      stats.snapshots = stats.repairPhis = stats.repairCells = 0;
      return stats;
    }
  } else removeUnreachableBlocks(*function);
  if (verifyModule(*trial) || !commit(f, *function, forceCommitFailure)) {
    stats.complete = false; stats.changed = false;
    stats.rewrittenEdges = stats.clonedBlocks = stats.clonedInstructions = stats.snapshots = stats.repairPhis = 0;
    if (batch) { stats.repairCells = 0; stats.rejected = candidates.size(); }
    stats.message = "owned trial verification or body commit failed; original unchanged"; return stats;
  }
  stats.changed = true;
  if (stats.message.empty()) stats.message = "original scalar-state edges threaded at their original switch point";
  return stats;
}
DispatchCorridorStats threadDispatchCorridor(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, const DispatchCorridorOptions &options) {
  return run(f, candidates, options, 0);
}
static DispatchCorridorOptions batchLimits(const DispatchCorridorBatchOptions &options) {
  DispatchCorridorOptions limits;
  limits.maxCandidates = options.maxCandidates; limits.maxRewrites = options.maxRewrites;
  limits.maxClonedBlocks = options.maxClonedBlocks;
  limits.maxClonedInstructions = options.maxClonedInstructions;
  limits.maxRepairCells = options.maxRepairCells; limits.maxWork = options.maxWork;
  return limits;
}
DispatchCorridorStats threadDispatchCorridorBatch(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, const DispatchCorridorBatchOptions &options) {
  return run(f, candidates, batchLimits(options), 0, true);
}
#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
DispatchCorridorStats threadDispatchCorridorCommitFailureForTest(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, unsigned stage,
    const DispatchCorridorOptions &options) {
  return run(f, candidates, options, stage);
}
DispatchCorridorStats threadDispatchCorridorBatchCommitFailureForTest(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, unsigned stage,
    const DispatchCorridorBatchOptions &options) {
  return run(f, candidates, batchLimits(options), stage, true);
}
#endif
} // namespace dragon::llvmlifter::detail
