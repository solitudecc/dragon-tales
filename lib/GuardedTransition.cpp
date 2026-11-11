#include "GuardedTransition.hpp"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/SSAUpdater.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {
constexpr std::size_t MaxLiveOuts = 128, MaxUses = 8192, MaxRepairCells = 262144;
constexpr std::size_t MaxTransactionBlocks = 65536, MaxTransactionInstructions = 1000000;
constexpr std::size_t MaxSuccessors = 512;
bool fail(GuardedTransitionResult &r, const char *message, bool budget = false) {
  r.complete = false; r.rejected = true; r.budgetExhausted = budget; r.message = message;
  return false;
}
bool supportedType(llvm::Type *t) {
  return t->isPointerTy() || (t->isIntegerTy() && t->getIntegerBitWidth() <= 128);
}
// StateGuardEdges' original/fresh use classification: original region uses
// in other blocks must be repaired too. PHI operands execute on their edges.
bool needsRepair(const llvm::Use &use, const llvm::BasicBlock &block) {
  auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
  if (!user || user->getParent() != &block) return true;
  auto *phi = llvm::dyn_cast<llvm::PHINode>(user);
  return phi && phi->getIncomingBlock(use.getOperandNo()) != &block;
}
bool supportedIdentity(llvm::Function &f, GuardedTransitionResult &r) {
  if (f.isDeclaration() || f.hasPersonalityFn() || f.getSubprogram())
    return fail(r, "declaration or EH/debug identity unsupported");
  if (f.size() > MaxTransactionBlocks)
    return fail(r, "transaction block budget exhausted", true);
  std::size_t count = 0;
  for (auto &b : f) {
    if (b.isEHPad() || b.hasAddressTaken())
      return fail(r, "EH or address-taken block unsupported");
    if (b.size() > MaxTransactionInstructions - count)
      return fail(r, "transaction instruction budget exhausted", true);
    count += b.size();
    for (auto &i : b)
      if (i.getDebugLoc() || i.hasDbgRecords() || i.isUsedByMetadata())
        return fail(r, "debug or local metadata identity unsupported");
  }
  return true;
}
bool safeBlock(const llvm::BasicBlock &b, GuardedTransitionResult &r) {
  auto *term = b.getTerminator();
  if (!llvm::isa<llvm::BranchInst, llvm::SwitchInst>(term) ||
      term->getNumSuccessors() > MaxSuccessors)
    return fail(r, "unsupported path terminator");
  for (auto &i : b) {
    if (llvm::isa<llvm::CallBase, llvm::AllocaInst, llvm::FenceInst,
                  llvm::AtomicRMWInst, llvm::AtomicCmpXchgInst>(i))
      return fail(r, "path contains call allocation or synchronization");
    if (!i.getType()->isVoidTy() && !supportedType(i.getType()))
      return fail(r, "unsupported path SSA type");
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&i)) {
      if (!load->isSimple()) return fail(r, "non-simple path LOAD");
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&i)) {
      auto *type = llvm::dyn_cast<llvm::IntegerType>(store->getValueOperand()->getType());
      if (!store->isSimple() || store->getPointerAddressSpace() != 0 || !type)
        return fail(r, "unsupported path STORE");
      auto bits = type->getBitWidth();
      if ((bits != 8 && bits != 16 && bits != 32 && bits != 64 && bits != 128) ||
          llvm::isa<llvm::UndefValue, llvm::PoisonValue>(store->getValueOperand()) ||
          llvm::isa<llvm::UndefValue, llvm::PoisonValue>(store->getPointerOperand()))
        return fail(r, "unsupported path STORE value");
      continue; // The write is copied; no memory/no-alias fact follows.
    }
    if (!i.isTerminator() && (i.mayHaveSideEffects() || i.mayReadOrWriteMemory()))
      return fail(r, "unsupported path effect");
  }
  return true;
}
struct Plan {
  GuardedTransitionRequest request;
  std::vector<llvm::Value *> snapshots;
  std::vector<llvm::Instruction *> definitions;
  std::size_t copyCost = 0;
  llvm::BasicBlock *caseTarget = nullptr;
  std::vector<std::pair<llvm::ConstantInt *,llvm::BasicBlock *>> fastCases;
  bool setMode = false;
};
bool prepare(llvm::Function &f, const GuardedTransitionRequest &q,
             const GuardedTransitionOptions &o, Plan &p, GuardedTransitionResult &r,
             const std::vector<llvm::ConstantInt *> *candidates) {
  if (!q.seed || !q.seedTarget || q.blocks.empty() || !q.candidate ||
      q.forwardSuccessors.size() + 1 != q.blocks.size())
    return fail(r, "malformed explicit path");
  if (q.seed->getParent() != &f || q.seedTarget != q.blocks.front() ||
      &q.candidate->getContext() != &f.getContext())
    return fail(r, "foreign function or context");
  auto *seedTerm = q.seed->getTerminator();
  if (!llvm::isa<llvm::BranchInst, llvm::SwitchInst>(seedTerm) ||
      q.seedSuccessor >= seedTerm->getNumSuccessors() ||
      seedTerm->getSuccessor(q.seedSuccessor) != q.seedTarget)
    return fail(r, "seed edge no longer matches");
  llvm::DominatorTree dom(f);
  if (!dom.isReachableFromEntry(q.seed)) return fail(r, "unreachable seed");
  std::unordered_map<llvm::BasicBlock *, std::size_t> positions;
  std::size_t instructions = 0;
  for (std::size_t n = 0; n < q.blocks.size(); ++n) {
    auto *b = q.blocks[n];
    if (!b || b->getParent() != &f || b == q.seed ||
        !positions.emplace(b, n).second || !dom.isReachableFromEntry(b))
      return fail(r, "invalid repeated or seed path block");
    if (!safeBlock(*b, r)) return false;
    if (b->size() > o.maxInstructions - std::min(instructions, o.maxInstructions))
      return fail(r, "path instruction budget exhausted", true);
    instructions += b->size();
    if (n + 1 < q.blocks.size() &&
        (q.forwardSuccessors[n] >= b->getTerminator()->getNumSuccessors() ||
         b->getTerminator()->getSuccessor(q.forwardSuccessors[n]) != q.blocks[n + 1]))
      return fail(r, "selected path edge no longer matches");
  }
  auto *jump = llvm::dyn_cast<llvm::SwitchInst>(q.blocks.back()->getTerminator());
  auto *load = jump ? llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()) : nullptr;
  if (!load || !load->isSimple() || load->getParent() != q.blocks.back() ||
      load->getType() != q.candidate->getType() ||
      !load->getType()->isIntegerTy() || load->getType()->getIntegerBitWidth() > 128)
    return fail(r, "switch must consume a fresh same-block simple integer LOAD");
  auto chosen = jump->findCaseValue(q.candidate);
  if (chosen == jump->case_default()) return fail(r, "candidate is not an original case");
  p.caseTarget = chosen->getCaseSuccessor();
  if(candidates) {
    if(candidates->empty() || candidates->size()>16)return fail(r,"target set must contain 1 to 16 candidates");
    p.setMode=true;
    for(auto *candidate:*candidates) {
      if(!candidate || &candidate->getContext()!=&f.getContext() || candidate->getType()!=load->getType())
        return fail(r,"target set contains a foreign or mismatched candidate");
      for(const auto &[old,target]:p.fastCases) {
        (void)target;
        if(old->getValue()==candidate->getValue())return fail(r,"target set contains duplicate candidates");
      }
      auto item=jump->findCaseValue(candidate);
      if(item==jump->case_default())return fail(r,"target set contains a non-case candidate");
      p.fastCases.emplace_back(candidate,item->getCaseSuccessor());
    }
  }
  for (std::size_t n = 0; n < q.blocks.size(); ++n) {
    auto *b = q.blocks[n], *pred = n ? q.blocks[n - 1] : q.seed;
    for (auto &i : *b) {
      auto check = [&](llvm::Value *value, bool phiInput) {
        if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value))
          return fail(r, "explicit undef or poison operand");
        auto *def = llvm::dyn_cast<llvm::Instruction>(value);
        if (!def) return true;
        auto location = positions.find(def->getParent());
        if (phiInput && n == 0) {
          if (!dom.dominates(def, pred->getTerminator()))
            return fail(r, "root PHI input does not dominate seed");
          if (location != positions.end() &&
              std::find(p.snapshots.begin(), p.snapshots.end(), value) == p.snapshots.end())
            p.snapshots.push_back(value);
          return true;
        }
        if (location != positions.end()) {
          if (location->second > n || (phiInput && location->second == n))
            return fail(r, "operand observes an earlier region execution");
          return true;
        }
        if (!dom.dominates(def, q.seed->getTerminator()))
          return fail(r, "external operand does not dominate seed");
        return true;
      };
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&i)) {
        int incoming = phi->getBasicBlockIndex(pred);
        if (incoming < 0 || (n == 0 && phi->getNumIncomingValues() <= 1))
          return fail(r, "missing selected PHI arm or empty original PHI");
        if (!check(phi->getIncomingValue(incoming), true)) return false;
      } else {
        for (auto &operand : i.operands()) if (!check(operand, false)) return false;
      }
      if (!i.getType()->isVoidTy()) p.definitions.push_back(&i);
    }
  }
  auto blocks = q.blocks.size() + 1 + !p.snapshots.empty();
  p.copyCost = instructions + (p.setMode ? 1 : 2) + (p.snapshots.empty() ? 0 : p.snapshots.size() + 1);
  if (blocks > o.maxBlocks || p.copyCost > o.maxInstructions)
    return fail(r, "guard and snapshot copy budget exhausted", true);
  std::size_t liveOuts = 0, uses = p.snapshots.size();
  for (auto *def : p.definitions) {
    bool live = false;
    for (auto &use : def->uses()) {
      if (!needsRepair(use, *def->getParent())) continue;
      auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
      if (!user || user->getFunction() != &f || user->getParent()->isEHPad())
        return fail(r, "unsupported live-out user");
      live = true;
      if (++uses > MaxUses) return fail(r, "live-out use budget exhausted", true);
    }
    if (live && ++liveOuts > MaxLiveOuts)
      return fail(r, "live-out value budget exhausted", true);
  }
  if (liveOuts && f.size() + blocks > MaxRepairCells / liveOuts)
    return fail(r, "live-out repair cell budget exhausted", true);
  p.request = q;
  return true;
}
bool apply(Plan &p, llvm::Function &f, GuardedTransitionResult &r) {
  const auto &q = p.request;
  llvm::ValueToValueMapTy map;
  llvm::DenseMap<llvm::Value *, llvm::Value *> oldInputs;
  llvm::BasicBlock *snapshots = nullptr;
  if (!p.snapshots.empty()) {
    snapshots = llvm::BasicBlock::Create(f.getContext(), "transition.inputs", &f);
    for (auto *value : p.snapshots) {
      auto *phi = llvm::PHINode::Create(value->getType(), 1, "transition.old", snapshots);
      phi->addIncoming(value, q.seed);
      oldInputs[value] = phi;
    }
  }
  auto mapped = [&](llvm::Value *value) -> llvm::Value * {
    if (llvm::Value *copy = map.lookup(value)) return copy;
    return value;
  };
  std::vector<llvm::BasicBlock *> clones;
  for (auto *block : q.blocks)
    clones.push_back(llvm::CloneBasicBlock(block, map, ".transition", &f));
  // Deliberately do not map BasicBlocks globally: only the selected successor
  // slot enters the next fresh copy. Every other edge targets original code.
  for (std::size_t n = 0; n < q.blocks.size(); ++n) {
    auto *pred = n ? q.blocks[n - 1] : q.seed;
    for (auto &phi : q.blocks[n]->phis()) {
      auto *incoming = phi.getIncomingValueForBlock(pred);
      map[&phi] = n ? mapped(incoming)
                   : (oldInputs.lookup(incoming) ? oldInputs.lookup(incoming) : incoming);
    }
    for (auto it = clones[n]->begin();
         it != clones[n]->end() && llvm::isa<llvm::PHINode>(*it);)
      (it++)->eraseFromParent();
  }
  for (std::size_t n = 0; n < clones.size(); ++n) {
    for (auto &i : *clones[n])
      if (!i.isTerminator())
        llvm::RemapInstruction(&i, map,
            llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges);
    auto *term = clones[n]->getTerminator();
    if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(term)) {
      if (branch->isConditional()) branch->setCondition(mapped(branch->getCondition()));
    } else {
      auto *jump = llvm::cast<llvm::SwitchInst>(term);
      jump->setCondition(mapped(jump->getCondition()));
    }
    if (n + 1 < clones.size())
      term->setSuccessor(q.forwardSuccessors[n], clones[n + 1]);
  }
  auto *last = clones.back();
  auto *fallback = llvm::BasicBlock::Create(f.getContext(), "transition.fallback", &f);
  auto *jump = llvm::cast<llvm::SwitchInst>(last->getTerminator());
  jump->removeFromParent();
  jump->insertInto(fallback, fallback->end());
  llvm::IRBuilder<> builder(last);
  if(p.setMode) {
    auto *fast=builder.CreateSwitch(jump->getCondition(),fallback,p.fastCases.size());
    for(const auto &[value,target]:p.fastCases)fast->addCase(value,target);
  } else {
    auto *matches = builder.CreateICmpEQ(jump->getCondition(), q.candidate, "transition.matches");
    builder.CreateCondBr(matches, p.caseTarget, fallback);
  }
  // One incoming per actual successor slot, including repeated targets.
  auto addExit = [&](llvm::BasicBlock *original, llvm::BasicBlock *copy,
                     llvm::BasicBlock *target) {
    for (auto &phi : target->phis())
      phi.addIncoming(mapped(phi.getIncomingValueForBlock(original)), copy);
    ++r.originalExitEdges;
  };
  for (std::size_t n = 0; n < q.blocks.size(); ++n) {
    auto *original = q.blocks[n];
    auto *term = original->getTerminator();
    for (unsigned edge = 0; edge < term->getNumSuccessors(); ++edge) {
      if (n + 1 < q.blocks.size() && edge == q.forwardSuccessors[n]) continue;
      addExit(original, n + 1 == q.blocks.size() ? fallback : clones[n],
              term->getSuccessor(edge));
    }
  }
  if(p.setMode) {
    for(const auto &[value,target]:p.fastCases) {(void)value;addExit(q.blocks.back(),last,target);}
  } else addExit(q.blocks.back(), last, p.caseTarget);
  if (snapshots) llvm::BranchInst::Create(clones.front(), snapshots);
  q.seed->getTerminator()->setSuccessor(q.seedSuccessor, snapshots ? snapshots : clones.front());
  for (auto &phi : q.blocks.front()->phis())
    phi.removeIncomingValue(phi.getBasicBlockIndex(q.seed), false);

  // Adapted from StateGuardEdges' repair discipline. Snapshot uses are included
  // so their predecessor value remains from the previous execution.
  std::size_t actualUses = 0;
  for (auto *def : p.definitions) {
    llvm::SmallVector<llvm::Use *, 32> uses;
    for (auto &use : def->uses()) {
      if (!needsRepair(use, *def->getParent())) continue;
      if (++actualUses > MaxUses)
        return fail(r, "actual live-out use budget exhausted", true);
      uses.push_back(&use);
    }
    if (uses.empty()) continue;
    auto position = std::find(q.blocks.begin(), q.blocks.end(), def->getParent()) - q.blocks.begin();
    llvm::SmallVector<llvm::PHINode *, 8> phis;
    llvm::SSAUpdater updater(&phis);
    updater.Initialize(def->getType(), "transition.live");
    updater.AddAvailableValue(def->getParent(), def);
    updater.AddAvailableValue(clones[position], mapped(def));
    for (auto *use : uses) updater.RewriteUse(*use);
    r.repairPhis += phis.size();
  }
  r.snapshotPhis = p.snapshots.size();
  r.clonedBlocks = clones.size() + 1 + (snapshots != nullptr);
  r.clonedInstructions = p.copyCost + r.repairPhis;
  return true;
}
llvm::Function *makeTrial(llvm::Function &source, llvm::ValueToValueMapTy &map) {
  auto *trial = llvm::Function::Create(source.getFunctionType(),
      llvm::GlobalValue::InternalLinkage, source.getAddressSpace(),
      source.getName() + ".transition.trial", source.getParent());
  trial->copyAttributesFrom(&source);
  auto argument = trial->arg_begin();
  for (auto &old : source.args()) {
    argument->setName(old.getName()); map[&old] = &*argument++;
  }
  llvm::SmallVector<llvm::ReturnInst *, 8> returns;
  // References to the original function, including recursion, are not mapped.
  llvm::CloneFunctionInto(trial, &source, map,
      llvm::CloneFunctionChangeType::LocalChangesOnly, returns);
  trial->setLinkage(llvm::GlobalValue::InternalLinkage);
  trial->setVisibility(llvm::GlobalValue::DefaultVisibility);
  trial->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  trial->setComdat(nullptr);
  return trial;
}
bool verify(llvm::Function &f, std::string &error) {
  llvm::raw_string_ostream stream(error);
  return !llvm::verifyFunction(f, &stream);
}
bool commit(llvm::Function &source, llvm::Function &trial, std::string &error) {
  auto *backup = llvm::Function::Create(source.getFunctionType(),
      llvm::GlobalValue::InternalLinkage, source.getAddressSpace(),
      source.getName() + ".transition.rollback", source.getParent());
  backup->splice(backup->end(), &source);
  auto argument = source.arg_begin();
  for (auto &copy : trial.args()) copy.replaceAllUsesWith(&*argument++);
  source.splice(source.end(), &trial);
  if (!verify(source, error)) {
    trial.splice(trial.end(), &source);
    source.splice(source.end(), backup);
    backup->eraseFromParent();
    return false;
  }
  backup->eraseFromParent();
  return true;
}
} // namespace
static GuardedTransitionResult cloneTransitionImpl(
    llvm::Function &f, const GuardedTransitionRequest &q,
    const GuardedTransitionOptions &o,
    const std::vector<llvm::ConstantInt *> *candidates) {
  GuardedTransitionResult r;
  if (!f.getParent()) { fail(r, "function has no module"); return r; }
  std::string error;
  llvm::raw_string_ostream stream(error);
  if (llvm::verifyModule(*f.getParent(), &stream)) {
    r.complete = false; r.rejected = true; r.message = "invalid input module: " + error;
    return r;
  }
  if (o.maxBlocks > 4096 || o.maxInstructions > 65536) {
    fail(r, "options exceed hard limits 4096/65536", true); return r;
  }
  if (!supportedIdentity(f, r)) return r;
  Plan original;
  if (!prepare(f, q, o, original, r, candidates)) return r;
  llvm::ValueToValueMapTy map;
  auto *trial = makeTrial(f, map);
  auto copied = q;
  copied.seed = llvm::cast<llvm::BasicBlock>(map.lookup(q.seed));
  copied.seedTarget = llvm::cast<llvm::BasicBlock>(map.lookup(q.seedTarget));
  for (auto &block : copied.blocks) block = llvm::cast<llvm::BasicBlock>(map.lookup(block));
  Plan plan;
  if (!prepare(*trial, copied, o, plan, r, candidates)) { trial->eraseFromParent(); return r; }
  if (!apply(plan, *trial, r)) {
    trial->eraseFromParent();
    r.clonedBlocks = r.clonedInstructions = r.snapshotPhis = r.repairPhis = r.originalExitEdges = 0;
    return r;
  }
  error.clear();
  if (r.clonedInstructions > o.maxInstructions) {
    fail(r, "SSA repair instruction budget exhausted", true);
  } else if (!verify(*trial, error)) {
    r.complete = false; r.rejected = true; r.message = "invalid trial: " + error;
  } else if (!commit(f, *trial, error)) {
    r.complete = false; r.rejected = true; r.message = "commit verification failed: " + error;
  } else {
    r.changed = true;
  }
  trial->eraseFromParent();
  if (!r.changed)
    r.clonedBlocks = r.clonedInstructions = r.snapshotPhis = r.repairPhis = r.originalExitEdges = 0;
  return r;
}
GuardedTransitionResult cloneGuardedTransition(
    llvm::Function &f,const GuardedTransitionRequest &q,const GuardedTransitionOptions &o) {
  return cloneTransitionImpl(f,q,o,nullptr);
}
GuardedTransitionResult cloneGuardedTransitionSet(
    llvm::Function &f,const GuardedTransitionSetRequest &q,const GuardedTransitionOptions &o) {
  GuardedTransitionResult result;
  if(q.candidates.empty() || q.candidates.size()>16) {
    fail(result,"target set must contain 1 to 16 candidates");return result;
  }
  GuardedTransitionRequest path{q.seed,q.seedSuccessor,q.seedTarget,q.blocks,q.forwardSuccessors,q.candidates.front()};
  return cloneTransitionImpl(f,path,o,&q.candidates);
}
} // namespace dragon::llvmlifter::detail
