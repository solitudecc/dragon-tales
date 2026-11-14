#include "LLVMDispatchCorridor.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
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
  bool integer(Value *v) {
    return v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() <= 128;
  }
  bool observe(Value *v) {
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
    if (!i || i->hasPoisonGeneratingFlags()) return done({});
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
      unsigned width = c->getType()->getIntegerBitWidth();
      switch (c->getOpcode()) {
      case Instruction::ZExt: return done(value->zext(width));
      case Instruction::SExt: return done(value->sext(width));
      case Instruction::Trunc: return done(value->trunc(width));
      default: return done({});
      }
    }
    auto *binary = dyn_cast<BinaryOperator>(i);
    if (!binary || (!isa<ConstantInt>(i->getOperand(0)) &&
                    !isa<ConstantInt>(i->getOperand(1)))) return done({});
    auto a = operand(0);
    auto b = operand(1);
    if (!a || !b) return done({});
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
  StateValues(Plan &p, Work &w) : plan(p), work(w) {}
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
bool preflight(Function &f, const DispatchCorridorEdge &edge,
               DispatchCorridorOptions const &options, Work &work, Plan &plan,
               std::string &reason) {
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
        else if (plan.position.contains(next)) return reject("nonlinear corridor reentry");
      }
      if (!continues) return reject("corridor does not contain an actual continuing edge");
    } else for (auto *next : successors(b))
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
  if (!StateValues(plan, work).run(sw->getCondition())) return reject("selector lacks a complete safe finite state proof");
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
} // namespace
static DispatchCorridorStats run(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, const DispatchCorridorOptions &options,
    unsigned forceCommitFailure) {
  DispatchCorridorStats stats; Work work{stats, options};
  auto rejectAll = [&](const char *message) {
    stats.complete = false; stats.rejected = candidates.size(); stats.message = message; return stats;
  };
  if (!f.getParent() || !f.hasName() || f.isDeclaration() || f.hasPersonalityFn() || f.getSubprogram() ||
      !options.maxCandidates || options.maxCandidates > 512 || !options.maxRewrites || options.maxRewrites > 64 ||
      !options.maxClonedBlocks || options.maxClonedBlocks > 64 || !options.maxClonedInstructions || options.maxClonedInstructions > 16384 ||
      !options.maxRepairCells || options.maxRepairCells > 262144 || !options.maxWork || options.maxWork > 1000000)
    return rejectAll("invalid corridor request or option bounds");
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
  struct Frozen { unsigned source, slot; std::vector<unsigned> path; };
  std::vector<Frozen> frozen;
  for (const auto &edge : candidates) {
    if (!indexes.contains(edge.source)) return rejectAll("candidate does not belong to the original function");
    Frozen item{indexes.at(edge.source), edge.successorSlot, {}};
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
  auto trial = CloneModule(*f.getParent());
  auto *function = trial->getFunction(f.getName());
  if (!function || function->getFunctionType() != f.getFunctionType())
    return rejectAll("original function missing from owned trial");
  std::vector<BasicBlock *> blocks; for (auto &b : *function) blocks.push_back(&b);
  for (const auto &item : frozen) {
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
  removeUnreachableBlocks(*function);
  if (verifyModule(*trial) || !commit(f, *function, forceCommitFailure)) {
    stats.complete = false; stats.changed = false;
    stats.rewrittenEdges = stats.clonedBlocks = stats.clonedInstructions = stats.snapshots = stats.repairPhis = 0;
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
#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
DispatchCorridorStats threadDispatchCorridorCommitFailureForTest(Function &f,
    const std::vector<DispatchCorridorEdge> &candidates, unsigned stage,
    const DispatchCorridorOptions &options) {
  return run(f, candidates, options, stage);
}
#endif
} // namespace dragon::llvmlifter::detail
