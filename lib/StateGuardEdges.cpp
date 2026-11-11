#include "StateGuardEdges.hpp"

#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/SSAUpdater.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <optional>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {

constexpr std::size_t MaxBlockInstructions = 256;
constexpr std::size_t MaxIncomingEdges = 512;
constexpr std::size_t MaxEdgeProofs = 2048;
constexpr std::size_t MaxExpressionDepth = 64;
constexpr std::size_t MaxIntegerBits = 128;
constexpr std::size_t MaxModuleExpressionNodes = 65536;
constexpr std::size_t MaxClones = 64;
constexpr std::size_t MaxClonedInstructions = 16384;
constexpr std::size_t MaxLiveOuts = 128;
constexpr std::size_t MaxExternalUses = 8192;
constexpr std::size_t MaxRepairCells = 262144;

bool supportedSSAType(llvm::Type *type) {
  return type->isPointerTy() ||
         (type->isIntegerTy() && type->getIntegerBitWidth() <= MaxIntegerBits);
}

bool safeBlock(const llvm::BasicBlock &block) {
  if (block.isEHPad() || block.hasAddressTaken()) return false;
  const auto *branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
  if (!branch || !branch->isConditional() ||
      branch->getSuccessor(0) == branch->getSuccessor(1))
    return false;
  for (const auto &instruction : block) {
    if (instruction.isUsedByMetadata() ||
        llvm::isa<llvm::CallBase, llvm::AllocaInst, llvm::FenceInst,
                  llvm::AtomicRMWInst, llvm::AtomicCmpXchgInst>(instruction))
      return false;
    if (!instruction.getType()->isVoidTy() &&
        !supportedSSAType(instruction.getType()))
      return false;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (!load->isSimple()) return false;
      continue;
    }
    if (instruction.isTerminator()) continue;
    if (instruction.mayHaveSideEffects() || instruction.mayReadOrWriteMemory())
      return false;
    // Flags on copied arithmetic remain in the copy. Only the condition
    // proof below rejects them; business and flags live-outs are not folded.
  }
  return true;
}

bool selectedEdge(const llvm::BasicBlock &pred, const llvm::BasicBlock &block,
                  unsigned &index) {
  if (&pred == &block || pred.isEHPad()) return false;
  const auto *branch = llvm::dyn_cast<llvm::BranchInst>(pred.getTerminator());
  if (!branch) return false;
  unsigned found = 0;
  for (unsigned successor = 0; successor < branch->getNumSuccessors(); ++successor)
    if (branch->getSuccessor(successor) == &block) {
      index = successor;
      ++found;
    }
  return found == 1;
}

bool selectedPHIsAvailable(const llvm::BasicBlock &block,
                           const llvm::BasicBlock &pred,
                           llvm::DominatorTree &dom) {
  for (const auto &phi : block.phis()) {
    const auto index = phi.getBasicBlockIndex(&pred);
    if (index < 0) return false;
    auto *incoming = phi.getIncomingValue(index);
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(incoming)) return false;
    if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(incoming)) {
      // Such a value belongs to an earlier execution of the original block,
      // not to this copy. Never remap old LOAD/freeze/loop state to new bits.
      if (instruction->getParent() == &block ||
          !dom.dominates(instruction, pred.getTerminator()))
        return false;
    }
  }
  return true;
}

struct ConditionProof {
  const llvm::BasicBlock &block;
  const llvm::BasicBlock &pred;
  std::size_t maxNodes;
  StateGuardEdgeBudget &budget;
  bool &exhausted;
  std::size_t nodes = 0;
  bool usesIncomingPHI = false;
  std::unordered_set<const llvm::Value *> active;

  std::optional<llvm::APInt> evaluate(const llvm::Value *value,
                                     std::size_t depth = 0,
                                     bool incomingContext = true) {
    if (!value || !value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() > MaxIntegerBits)
      return std::nullopt;
    if (nodes == maxNodes || depth == MaxExpressionDepth ||
        budget.expressionNodes == MaxModuleExpressionNodes) {
      exhausted = true;
      if (budget.expressionNodes == MaxModuleExpressionNodes)
        budget.stopped = true;
      return std::nullopt;
    }
    ++nodes;
    ++budget.expressionNodes;
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
      return constant->getValue();
    const auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    // This includes samesign ICMP, nneg ZEXT, disjoint OR and nowrap/exact
    // arithmetic. Modular evaluation of these instructions could hide poison.
    if (!instruction || instruction->hasPoisonGeneratingFlags() ||
        (!incomingContext && instruction->getParent() == &block) ||
        !active.insert(value).second)
      return std::nullopt;
    auto result = evaluateInstruction(*instruction, depth,
        incomingContext && instruction->getParent() == &block);
    active.erase(value);
    return result;
  }

  std::optional<llvm::APInt> evaluateInstruction(
      const llvm::Instruction &instruction, std::size_t depth,
      bool incomingContext) {
    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
      if (phi->getParent() != &block) return std::nullopt;
      const auto index = phi->getBasicBlockIndex(&pred);
      if (index < 0) return std::nullopt;
      usesIncomingPHI = true;
      // An incoming expression may refer to an earlier loop execution. Once
      // outside this block, never substitute this invocation's PHI arms.
      return evaluate(phi->getIncomingValue(index), depth + 1, false);
    }
    if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction))
      // Only a genuinely proved constant passes through freeze. In particular,
      // two dynamic freezes, or a freeze of poison, are never correlated.
      return evaluate(freeze->getOperand(0), depth + 1, incomingContext);
    if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
      const auto condition = evaluate(select->getCondition(), depth + 1, incomingContext);
      if (!condition || condition->getBitWidth() != 1) return std::nullopt;
      // LLVM selects only this arm. The other arm may be unknown or poison.
      return evaluate(condition->isZero() ? select->getFalseValue()
                                           : select->getTrueValue(), depth + 1, incomingContext);
    }
    if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
      auto operand = evaluate(cast->getOperand(0), depth + 1, incomingContext);
      if (!operand) return std::nullopt;
      const auto width = cast->getType()->getIntegerBitWidth();
      switch (cast->getOpcode()) {
        case llvm::Instruction::ZExt:
        case llvm::Instruction::Trunc: return operand->zextOrTrunc(width);
        case llvm::Instruction::SExt: return operand->sextOrTrunc(width);
        case llvm::Instruction::BitCast:
          if (operand->getBitWidth() == width) return operand;
          break;
        default: break;
      }
      return std::nullopt;
    }
    if (const auto *compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
      const auto left = evaluate(compare->getOperand(0), depth + 1, incomingContext);
      const auto right = evaluate(compare->getOperand(1), depth + 1, incomingContext);
      if (!left || !right || left->getBitWidth() != right->getBitWidth())
        return std::nullopt;
      bool result;
      switch (compare->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ: result = *left == *right; break;
        case llvm::CmpInst::ICMP_NE: result = *left != *right; break;
        case llvm::CmpInst::ICMP_UGT: result = left->ugt(*right); break;
        case llvm::CmpInst::ICMP_UGE: result = left->uge(*right); break;
        case llvm::CmpInst::ICMP_ULT: result = left->ult(*right); break;
        case llvm::CmpInst::ICMP_ULE: result = left->ule(*right); break;
        case llvm::CmpInst::ICMP_SGT: result = left->sgt(*right); break;
        case llvm::CmpInst::ICMP_SGE: result = left->sge(*right); break;
        case llvm::CmpInst::ICMP_SLT: result = left->slt(*right); break;
        case llvm::CmpInst::ICMP_SLE: result = left->sle(*right); break;
        default: return std::nullopt;
      }
      return llvm::APInt(1, result);
    }
    const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction);
    if (!binary) return std::nullopt;
    const auto left = evaluate(binary->getOperand(0), depth + 1, incomingContext);
    const auto right = evaluate(binary->getOperand(1), depth + 1, incomingContext);
    if (!left || !right || left->getBitWidth() != right->getBitWidth())
      return std::nullopt;
    switch (binary->getOpcode()) {
      case llvm::Instruction::Add: return *left + *right;
      case llvm::Instruction::Sub: return *left - *right;
      case llvm::Instruction::Mul: return *left * *right;
      case llvm::Instruction::And: return *left & *right;
      case llvm::Instruction::Or: return *left | *right;
      case llvm::Instruction::Xor: return *left ^ *right;
      case llvm::Instruction::Shl:
        if (right->ult(left->getBitWidth())) return left->shl(*right);
        break;
      case llvm::Instruction::LShr:
        if (right->ult(left->getBitWidth())) return left->lshr(*right);
        break;
      case llvm::Instruction::AShr:
        if (right->ult(left->getBitWidth())) return left->ashr(*right);
        break;
      default: break;
    }
    return std::nullopt;
  }
};

bool needsRepair(const llvm::Use &use, const llvm::BasicBlock &block) {
  const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
  if (!user || user->getParent() != &block) return true;
  // PHI uses are on edges, including loop uses inside the original block.
  const auto *phi = llvm::dyn_cast<llvm::PHINode>(user);
  return phi && phi->getIncomingBlock(use.getOperandNo()) != &block;
}

std::optional<std::size_t> preflightRepair(
    const llvm::BasicBlock &block, const llvm::Function &function,
    bool &exhausted) {
  std::size_t liveOuts = 0;
  std::size_t externalUses = 0;
  for (const auto &instruction : block) {
    bool live = false;
    for (const auto &use : instruction.uses()) {
      if (!needsRepair(use, block)) continue;
      const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
      if (!user || user->getFunction() != &function || user->getParent()->isEHPad() ||
          !supportedSSAType(instruction.getType()))
        return std::nullopt;
      live = true;
      if (++externalUses > MaxExternalUses) {
        exhausted = true;
        return std::nullopt;
      }
    }
    if (live && ++liveOuts > MaxLiveOuts) {
      exhausted = true;
      return std::nullopt;
    }
  }
  if (liveOuts && function.size() + 1 > MaxRepairCells / liveOuts) {
    exhausted = true;
    return std::nullopt;
  }
  return liveOuts * (function.size() + 1);
}

void cloneEdge(llvm::BranchInst &branch, llvm::BasicBlock &pred,
                unsigned successorIndex, llvm::BasicBlock &target) {
  auto &block = *branch.getParent();
  auto &function = *block.getParent();
  llvm::ValueToValueMapTy map;
  std::vector<llvm::Instruction *> definitions;
  for (auto &instruction : block)
    if (!instruction.getType()->isVoidTy()) definitions.push_back(&instruction);
  auto *clone = llvm::CloneBasicBlock(&block, map, ".state.edge", &function);
  for (auto &phi : block.phis())
    map[&phi] = phi.getIncomingValueForBlock(&pred);
  for (auto iterator = clone->begin();
       iterator != clone->end() && llvm::isa<llvm::PHINode>(*iterator);)
    (iterator++)->eraseFromParent();
  for (auto &instruction : *clone)
    if (!instruction.isTerminator())
      llvm::RemapInstruction(&instruction, map,
          llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges);

  // A self successor deliberately points back to the original block: each
  // new iteration must evaluate the original PHIs, LOADs and freezes afresh.
  auto *clonedBranch = clone->getTerminator();
  llvm::BranchInst::Create(&target, clonedBranch->getIterator());
  clonedBranch->eraseFromParent();
  for (auto &phi : target.phis()) {
    auto *incoming = phi.getIncomingValueForBlock(&block);
    // MapValue returns null for an unmapped external SSA instruction even
    // with RF_IgnoreMissingLocals. Such a value belongs to the unchanged
    // surrounding function and must retain its original identity.
    llvm::Value *mapped = map.lookup(incoming);
    if (!mapped) mapped = incoming;
    phi.addIncoming(mapped, clone);
  }
  pred.getTerminator()->setSuccessor(successorIndex, clone);
  // Retain the original PHI objects until every outside/loop use is repaired.
  for (auto &phi : block.phis()) phi.removeIncomingValue(&pred, false);

  for (auto *definition : definitions) {
    llvm::SmallVector<llvm::Use *, 32> uses;
    for (auto &use : definition->uses())
      if (needsRepair(use, block)) uses.push_back(&use);
    if (uses.empty()) continue;
    llvm::SSAUpdater updater;
    updater.Initialize(definition->getType(), "state.edge.live");
    updater.AddAvailableValue(&block, definition);
    updater.AddAvailableValue(clone, map[definition]);
    for (auto *use : uses) updater.RewriteUse(*use);
  }
}

void reportBudget(DispatcherSpecializationStats &stats,
                   StateGuardEdgeBudget &budget) {
  if (budget.reported) return;
  stats.complete = false;
  ++stats.budgetExhausted;
  budget.reported = true;
}

} // namespace

bool specializeStateGuardEdge(
    llvm::BranchInst &branch, llvm::DominatorTree &dom,
    const DispatcherSpecializationOptions &options,
    DispatcherSpecializationStats &stats, StateGuardEdgeBudget &budget) {
  if (budget.stopped) return false;
  auto &block = *branch.getParent();
  auto &function = *block.getParent();
  if (!dom.isReachableFromEntry(&block) || block.phis().empty() ||
      llvm::pred_size(&block) < 2)
    return false;
  if (block.size() > MaxBlockInstructions ||
      llvm::pred_size(&block) > MaxIncomingEdges) {
    reportBudget(stats, budget);
    return false;
  }
  if (!safeBlock(block)) return false;
  std::vector<llvm::BasicBlock *> predecessors;
  // PHI order is stable and favors a seed over a later loop backedge.
  for (unsigned index = 0; index < block.phis().begin()->getNumIncomingValues(); ++index)
    predecessors.push_back(block.phis().begin()->getIncomingBlock(index));
  std::unordered_set<llvm::BasicBlock *> seen;
  bool exhausted = false;
  bool changed = false;
  for (auto *pred : predecessors) {
    unsigned successorIndex = 0;
    if (!seen.insert(pred).second || !dom.isReachableFromEntry(pred) ||
        !selectedEdge(*pred, block, successorIndex) ||
        !selectedPHIsAvailable(block, *pred, dom))
      continue;
    if (budget.edgeProofs == MaxEdgeProofs) {
      exhausted = true;
      budget.stopped = true;
      break;
    }
    ++budget.edgeProofs;
    ConditionProof proof{block, *pred, options.maxExpressionNodes, budget,
                         exhausted};
    const auto condition = proof.evaluate(branch.getCondition());
    if (budget.stopped) break;
    if (!condition || condition->getBitWidth() != 1 || !proof.usesIncomingPHI)
      continue;
    const auto repair = preflightRepair(block, function, exhausted);
    if (!repair) continue;
    if (budget.clones == MaxClones ||
        block.size() > MaxClonedInstructions - budget.clonedInstructions ||
        *repair > MaxRepairCells - budget.repairCells) {
      exhausted = true;
      budget.stopped = true;
      break;
    }
    const auto instructions = block.size();
    auto *target = branch.getSuccessor(condition->isZero() ? 1 : 0);
    cloneEdge(branch, *pred, successorIndex, *target);
    ++budget.clones;
    budget.clonedInstructions += instructions;
    budget.repairCells += *repair;
    ++stats.uniqueConditions;
    ++stats.branchesRewritten;
    stats.changed = true;
    dom.recalculate(function);
    changed = true;
    break;
  }
  if (exhausted) reportBudget(stats, budget);
  return changed;
}

} // namespace dragon::llvmlifter::detail
