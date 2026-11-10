#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter {
namespace {

using Active = std::unordered_set<const llvm::Value *>;

template <typename Terminator>
void dropUnselectedIncoming(llvm::BasicBlock &block, Terminator *terminator,
                            llvm::BasicBlock *selected) {
  std::vector<llvm::BasicBlock *> removed;
  for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index) {
    auto *successor = terminator->getSuccessor(index);
    if (successor == selected ||
        std::find(removed.begin(), removed.end(), successor) != removed.end())
      continue;
    // removePredecessor updates PHIs while the old terminator still carries
    // the edge. The terminator replacement below then removes the CFG edge.
    successor->removePredecessor(&block);
    removed.push_back(successor);
  }
}

std::optional<llvm::APInt> uniqueConstant(const llvm::Value *value,
                                          std::size_t &nodes,
                                          std::size_t maxNodes,
                                          Active &active) {
  if (!value || ++nodes > maxNodes) return std::nullopt;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
    return constant->getValue();
  if (!active.insert(value).second) return std::nullopt;
  std::optional<llvm::APInt> result;
  if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    result = uniqueConstant(freeze->getOperand(0), nodes, maxNodes, active);
  } else if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    if (const auto *trunc = llvm::dyn_cast<llvm::TruncInst>(cast)) {
      if (trunc->hasNoUnsignedWrap() || trunc->hasNoSignedWrap()) {
        active.erase(value);
        return std::nullopt;
      }
    }
    if (const auto *zext = llvm::dyn_cast<llvm::ZExtInst>(cast)) {
      if (zext->hasNonNeg()) {
        active.erase(value);
        return std::nullopt;
      }
    }
    if (!cast->getType()->isIntegerTy() ||
        !cast->getOperand(0)->getType()->isIntegerTy()) {
      active.erase(value);
      return std::nullopt;
    }
    auto operand = uniqueConstant(cast->getOperand(0), nodes, maxNodes, active);
    if (operand) {
      const unsigned width = cast->getType()->getIntegerBitWidth();
      switch (cast->getOpcode()) {
        case llvm::Instruction::ZExt:
        case llvm::Instruction::Trunc:
          result = operand->zextOrTrunc(width);
          break;
        case llvm::Instruction::SExt:
          result = operand->sextOrTrunc(width);
          break;
        case llvm::Instruction::BitCast:
          if (operand->getBitWidth() == width) result = *operand;
          break;
        default:
          break;
      }
    }
  } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    // A poison condition can poison the select even when both arms have the
    // same bits.  Require a proven, non-poison i1 condition before treating
    // the result as a unique constant.
    auto condition = uniqueConstant(select->getCondition(), nodes, maxNodes, active);
    auto left = uniqueConstant(select->getTrueValue(), nodes, maxNodes, active);
    auto right = uniqueConstant(select->getFalseValue(), nodes, maxNodes, active);
    if (condition && condition->getBitWidth() == 1 && left && right &&
        *left == *right)
      result = left;
  } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (phi->getNumIncomingValues()) {
      result = uniqueConstant(phi->getIncomingValue(0), nodes, maxNodes, active);
      for (unsigned index = 1; result && index < phi->getNumIncomingValues(); ++index) {
        auto incoming = uniqueConstant(phi->getIncomingValue(index), nodes, maxNodes, active);
        if (!incoming || *incoming != *result) result.reset();
      }
    }
  } else if (const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    if (const auto *disjoint = llvm::dyn_cast<llvm::PossiblyDisjointInst>(binary)) {
      if (disjoint->isDisjoint()) {
        active.erase(value);
        return std::nullopt;
      }
    }
    auto left = uniqueConstant(binary->getOperand(0), nodes, maxNodes, active);
    auto right = uniqueConstant(binary->getOperand(1), nodes, maxNodes, active);
    if (left && right && left->getBitWidth() == right->getBitWidth()) {
      const auto width = left->getBitWidth();
      switch (binary->getOpcode()) {
        // A flagged arithmetic operation may produce poison on overflow;
        // modular APInt evaluation would then claim a constant that is not
        // valid on every defined execution. Keep those conditions explicit.
        case llvm::Instruction::Add:
          if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
            result = *left + *right;
          break;
        case llvm::Instruction::Sub:
          if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
            result = *left - *right;
          break;
        case llvm::Instruction::Mul:
          if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
            result = *left * *right;
          break;
        case llvm::Instruction::And: result = *left & *right; break;
        case llvm::Instruction::Or: result = *left | *right; break;
        case llvm::Instruction::Xor: result = *left ^ *right; break;
        case llvm::Instruction::Shl:
          if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
            if (!right->uge(width)) result = left->shl(*right);
          break;
        case llvm::Instruction::LShr:
          if (!binary->isExact())
            if (!right->uge(width)) result = left->lshr(*right);
          break;
        default: break;
      }
    }
  }
  active.erase(value);
  return result;
}

} // namespace

DispatcherSpecializationStats runDispatcherSpecialization(
    llvm::Module &module, const DispatcherSpecializationOptions &options) {
  DispatcherSpecializationStats stats;
  stats.available = true;
  if (!options.maxConditions || !options.maxExpressionNodes) {
    stats.complete = false;
    ++stats.budgetExhausted;
    return stats;
  }

  for (auto &function : module) {
    if (function.isDeclaration()) continue;
    for (auto &block : function) {
      auto *terminator = block.getTerminator();
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
        if (!branch->isConditional()) continue;
        ++stats.conditionsVisited;
        if (stats.conditionsVisited > options.maxConditions) {
          stats.complete = false;
          ++stats.budgetExhausted;
          return stats;
        }
        std::size_t nodes = 0;
        Active active;
        auto condition = uniqueConstant(branch->getCondition(), nodes,
                                        options.maxExpressionNodes, active);
        if (!condition || condition->getBitWidth() != 1) continue;
        ++stats.uniqueConditions;
        auto *target = condition->isZero() ? branch->getSuccessor(1)
                                           : branch->getSuccessor(0);
        dropUnselectedIncoming(*branch->getParent(), branch, target);
        llvm::BranchInst::Create(target, branch);
        branch->eraseFromParent();
        ++stats.branchesRewritten;
        stats.changed = true;
        continue;
      }
      auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator);
      if (!switchInst) continue;
      ++stats.conditionsVisited;
      ++stats.switchesVisited;
      if (stats.conditionsVisited > options.maxConditions) {
        stats.complete = false;
        ++stats.budgetExhausted;
        return stats;
      }
      std::size_t nodes = 0;
      Active active;
      auto selector = uniqueConstant(switchInst->getCondition(), nodes,
                                     options.maxExpressionNodes, active);
      if (!selector) {
        ++stats.unresolvedSwitches;
        continue;
      }
      ++stats.uniqueConditions;
      llvm::BasicBlock *target = switchInst->getDefaultDest();
      for (const auto &caseHandle : switchInst->cases()) {
        if (caseHandle.getCaseValue()->getValue() == *selector) {
          target = caseHandle.getCaseSuccessor();
          break;
        }
      }
      dropUnselectedIncoming(*switchInst->getParent(), switchInst, target);
      llvm::BranchInst::Create(target, switchInst);
      switchInst->eraseFromParent();
      ++stats.switchesRewritten;
      stats.changed = true;
    }
  }
  return stats;
}

} // namespace dragon::llvmlifter
