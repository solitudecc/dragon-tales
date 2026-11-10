#include <dragon/lifters/llvm/LLVMDeflattening.hpp>

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
using FiniteConstants = std::vector<llvm::APInt>;

bool appendFinite(FiniteConstants &values, const llvm::APInt &value,
                  std::size_t maxValues) {
  for (const auto &existing : values)
    if (existing == value) return true;
  if (values.size() >= maxValues) return false;
  values.push_back(value);
  return true;
}

std::optional<FiniteConstants> finiteConstants(const llvm::Value *value,
                                               std::size_t &nodes,
                                               std::size_t maxNodes,
                                               std::size_t maxValues,
                                               Active &active) {
  if (!value || ++nodes > maxNodes || !maxValues) return std::nullopt;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
    return FiniteConstants{constant->getValue()};
  if (!active.insert(value).second) return std::nullopt;
  auto done = [&](std::optional<FiniteConstants> result) {
    active.erase(value);
    return result;
  };
  std::optional<FiniteConstants> result;
  if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    result = finiteConstants(freeze->getOperand(0), nodes, maxNodes,
                              maxValues, active);
  } else if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    if (!cast->getType()->isIntegerTy() ||
        !cast->getOperand(0)->getType()->isIntegerTy())
      return done(std::nullopt);
    auto operand = finiteConstants(cast->getOperand(0), nodes, maxNodes,
                                   maxValues, active);
    if (operand) {
      const unsigned width = cast->getType()->getIntegerBitWidth();
      FiniteConstants converted;
      for (const auto &item : *operand) {
        llvm::APInt convertedItem(width, 0);
        switch (cast->getOpcode()) {
          case llvm::Instruction::ZExt:
          case llvm::Instruction::Trunc:
            convertedItem = item.zextOrTrunc(width);
            break;
          case llvm::Instruction::SExt:
            convertedItem = item.sextOrTrunc(width);
            break;
          case llvm::Instruction::BitCast:
            if (item.getBitWidth() != width) return done(std::nullopt);
            convertedItem = item;
            break;
          default:
            return done(std::nullopt);
        }
        if (!appendFinite(converted, convertedItem, maxValues))
          return done(std::nullopt);
      }
      result = std::move(converted);
    }
  } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    // A non-constant/poison condition can make even equal select arms poison.
    // Only a singleton, proven i1 condition is safe to follow.
    auto condition = finiteConstants(select->getCondition(), nodes, maxNodes,
                                     maxValues, active);
    if (condition && condition->size() == 1 &&
        condition->front().getBitWidth() == 1) {
      const auto *chosen = condition->front().isZero()
                               ? select->getFalseValue()
                               : select->getTrueValue();
      result = finiteConstants(chosen, nodes, maxNodes, maxValues, active);
    }
  } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (!phi->getNumIncomingValues()) return done(std::nullopt);
    FiniteConstants merged;
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      auto incoming = finiteConstants(phi->getIncomingValue(index), nodes,
                                     maxNodes, maxValues, active);
      if (!incoming) return done(std::nullopt);
      for (const auto &item : *incoming)
        if (!appendFinite(merged, item, maxValues)) return done(std::nullopt);
    }
    result = std::move(merged);
  } else if (const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    auto left = finiteConstants(binary->getOperand(0), nodes, maxNodes,
                                maxValues, active);
    auto right = finiteConstants(binary->getOperand(1), nodes, maxNodes,
                                 maxValues, active);
    if (left && right && !left->empty() && !right->empty() &&
        left->front().getBitWidth() == right->front().getBitWidth()) {
      const auto width = left->front().getBitWidth();
      FiniteConstants evaluated;
      for (const auto &lhs : *left) {
        for (const auto &rhs : *right) {
          std::optional<llvm::APInt> item;
          switch (binary->getOpcode()) {
            case llvm::Instruction::Add:
              if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
                item = lhs + rhs;
              break;
            case llvm::Instruction::Sub:
              if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
                item = lhs - rhs;
              break;
            case llvm::Instruction::Mul:
              if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
                item = lhs * rhs;
              break;
            case llvm::Instruction::And: item = lhs & rhs; break;
            case llvm::Instruction::Or: item = lhs | rhs; break;
            case llvm::Instruction::Xor: item = lhs ^ rhs; break;
            case llvm::Instruction::Shl:
              if (!binary->hasNoUnsignedWrap() &&
                  !binary->hasNoSignedWrap() && !rhs.uge(width))
                item = lhs.shl(rhs);
              break;
            case llvm::Instruction::LShr:
              if (!binary->isExact() && !rhs.uge(width)) item = lhs.lshr(rhs);
              break;
            default: break;
          }
          if (!item || !appendFinite(evaluated, *item, maxValues))
            return done(std::nullopt);
        }
      }
      result = std::move(evaluated);
    }
  }
  return done(std::move(result));
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
        auto incoming = uniqueConstant(phi->getIncomingValue(index), nodes,
                                       maxNodes, active);
        if (!incoming || *incoming != *result) result.reset();
      }
    }
  } else if (const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    auto left = uniqueConstant(binary->getOperand(0), nodes, maxNodes, active);
    auto right = uniqueConstant(binary->getOperand(1), nodes, maxNodes, active);
    if (left && right && left->getBitWidth() == right->getBitWidth()) {
      const auto width = left->getBitWidth();
      switch (binary->getOpcode()) {
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
        case llvm::Instruction::And:
          result = *left & *right;
          break;
        case llvm::Instruction::Or:
          result = *left | *right;
          break;
        case llvm::Instruction::Xor:
          result = *left ^ *right;
          break;
        case llvm::Instruction::Shl:
          if (!binary->hasNoUnsignedWrap() && !binary->hasNoSignedWrap())
            if (!right->uge(width)) result = left->shl(*right);
          break;
        case llvm::Instruction::LShr:
          if (!binary->isExact())
            if (!right->uge(width)) result = left->lshr(*right);
          break;
        default:
          break;
      }
    }
  }
  active.erase(value);
  return result;
}

struct EdgePlan {
  llvm::BasicBlock *predecessor = nullptr;
  llvm::Instruction *terminator = nullptr;
  unsigned successorIndex = 0;
  llvm::BasicBlock *target = nullptr;
};

bool hasAnyPHI(const llvm::BasicBlock &block) {
  return llvm::isa<llvm::PHINode>(block.begin());
}

bool onlyPHIsAndTerminator(const llvm::BasicBlock &block) {
  for (const auto &instruction : block) {
    if (&instruction == block.getTerminator()) break;
    if (!llvm::isa<llvm::PHINode>(&instruction)) return false;
  }
  return true;
}

unsigned successorCount(const llvm::Instruction *terminator,
                        const llvm::BasicBlock *target) {
  unsigned count = 0;
  for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index)
    count += terminator->getSuccessor(index) == target;
  return count;
}

std::optional<unsigned> soleSuccessorIndex(const llvm::Instruction *terminator,
                                           const llvm::BasicBlock *target) {
  std::optional<unsigned> result;
  for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index) {
    if (terminator->getSuccessor(index) != target) continue;
    if (result) return std::nullopt;
    result = index;
  }
  return result;
}

llvm::BasicBlock *targetFor(llvm::SwitchInst &switchInst,
                            const llvm::APInt &value) {
  auto *target = switchInst.getDefaultDest();
  for (const auto &caseHandle : switchInst.cases()) {
    if (caseHandle.getCaseValue()->getValue() == value)
      return caseHandle.getCaseSuccessor();
  }
  return target;
}

bool planDispatcher(llvm::BasicBlock &dispatch, llvm::SwitchInst &switchInst,
                    const DeflatteningOptions &options,
                    std::vector<EdgePlan> &plans) {
  if (!onlyPHIsAndTerminator(dispatch)) return false;
  auto *state = llvm::dyn_cast<llvm::PHINode>(switchInst.getCondition());
  if (!state || state->getParent() != &dispatch || !state->hasOneUse())
    return false;
  if (!state->getNumIncomingValues() ||
      state->getNumIncomingValues() > options.maxIncomingEdges)
    return false;

  std::unordered_set<const llvm::BasicBlock *> seen;
  std::vector<EdgePlan> candidate;
  candidate.reserve(state->getNumIncomingValues());
  for (unsigned index = 0; index < state->getNumIncomingValues(); ++index) {
    auto *predecessor = state->getIncomingBlock(index);
    if (!predecessor || predecessor == &dispatch ||
        !seen.insert(predecessor).second)
      return false;
    auto *terminator = predecessor->getTerminator();
    if (!terminator || successorCount(terminator, &dispatch) != 1)
      return false;
    auto edge = soleSuccessorIndex(terminator, &dispatch);
    if (!edge) return false;

    std::size_t nodes = 0;
    Active active;
    auto values = finiteConstants(state->getIncomingValue(index), nodes,
                                  options.maxExpressionNodes,
                                  options.maxValues, active);
    if (!values || values->empty() ||
        values->front().getBitWidth() !=
            switchInst.getCondition()->getType()->getIntegerBitWidth())
      return false;
    auto *target = targetFor(switchInst, values->front());
    for (std::size_t valueIndex = 1; valueIndex < values->size();
         ++valueIndex) {
      if (targetFor(switchInst, (*values)[valueIndex]) != target) return false;
    }
    if (target == &dispatch) continue; // a self-dispatch edge already is direct
    if (hasAnyPHI(*target)) return false;
    candidate.push_back({predecessor, terminator, *edge, target});
  }
  plans = std::move(candidate);
  return !plans.empty();
}

} // namespace

DeflatteningStats runProvableDeflattening(
    llvm::Module &module, const DeflatteningOptions &options) {
  DeflatteningStats stats;
  stats.available = true;
  if (!options.maxDispatchers || !options.maxIncomingEdges ||
      !options.maxExpressionNodes) {
    stats.complete = false;
    ++stats.budgetExhausted;
    return stats;
  }

  for (auto &function : module) {
    if (function.isDeclaration()) continue;
    for (auto &block : function) {
      auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());
      if (!switchInst) continue;
      ++stats.dispatchersVisited;
      if (stats.dispatchersVisited > options.maxDispatchers) {
        stats.complete = false;
        ++stats.budgetExhausted;
        return stats;
      }
      std::vector<EdgePlan> plans;
      if (!planDispatcher(block, *switchInst, options, plans)) {
        ++stats.unresolvedDispatchers;
        continue;
      }
      for (const auto &plan : plans) {
        // Keep the old edge while LLVM removes its PHI incoming values.
        block.removePredecessor(plan.predecessor);
        plan.terminator->setSuccessor(plan.successorIndex, plan.target);
        ++stats.edgesRewritten;
      }
      ++stats.dispatchersRewritten;
      stats.changed = true;
    }
  }
  return stats;
}

} // namespace dragon::llvmlifter
