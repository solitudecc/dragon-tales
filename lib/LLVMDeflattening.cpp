#include <dragon/lifters/llvm/LLVMDeflattening.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/ValueTracking.h>

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
    if (const auto *trunc = llvm::dyn_cast<llvm::TruncInst>(cast))
      if (trunc->hasNoUnsignedWrap() || trunc->hasNoSignedWrap())
        return done(std::nullopt);
    if (const auto *zext = llvm::dyn_cast<llvm::ZExtInst>(cast))
      if (zext->hasNonNeg()) return done(std::nullopt);
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
    if (const auto *disjoint = llvm::dyn_cast<llvm::PossiblyDisjointInst>(binary))
      if (disjoint->isDisjoint()) return done(std::nullopt);
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
        auto incoming = uniqueConstant(phi->getIncomingValue(index), nodes,
                                       maxNodes, active);
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
  llvm::Value *condition = nullptr;
  llvm::BasicBlock *falseTarget = nullptr;
};

bool hasAnyPHI(const llvm::BasicBlock &block) {
  return llvm::isa<llvm::PHINode>(block.begin());
}

bool hasEscapingDefinitions(const llvm::BasicBlock &block) {
  for (const auto &instruction : block) {
    if (&instruction == block.getTerminator()) break;
    for (const auto *user : instruction.users()) {
      const auto *use = llvm::dyn_cast<llvm::Instruction>(user);
      if (!use || use->getParent() != &block) return true;
    }
  }
  return false;
}

bool onlyPHIsAndTerminator(const llvm::BasicBlock &block) {
  for (const auto &instruction : block) {
    if (&instruction == block.getTerminator()) break;
    if (!llvm::isa<llvm::PHINode>(&instruction)) return false;
  }
  return true;
}

bool onlyPHIsAndOneLoad(const llvm::BasicBlock &block,
                        const llvm::LoadInst &load) {
  bool sawLoad = false;
  for (const auto &instruction : block) {
    if (&instruction == block.getTerminator()) break;
    if (&instruction == &load) {
      if (sawLoad) return false;
      sawLoad = true;
      continue;
    }
    if (!llvm::isa<llvm::PHINode>(&instruction)) return false;
  }
  return sawLoad;
}

llvm::StoreInst *reachingStateStore(llvm::BasicBlock &block,
                                   const llvm::LoadInst &load,
                                   std::size_t &nodes, std::size_t maxNodes) {
  auto iterator = block.getTerminator()->getIterator();
  while (iterator != block.begin()) {
    if (++nodes > maxNodes) return nullptr;
    auto &instruction = *--iterator;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      // Stop at the nearest write. In particular a partial/unknown write must
      // never let an older constant definition leak through the scan.
      if (!store->isSimple() ||
          store->getPointerOperand() != load.getPointerOperand() ||
          store->getValueOperand()->getType() != load.getType())
        return nullptr;
      return store;
    }
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      if (call->isInlineAsm() || call->isConvergent() ||
          call->hasFnAttr(llvm::Attribute::ReturnsTwice) ||
          (!call->doesNotAccessMemory() && !call->onlyReadsMemory()))
        return nullptr;
      // The call still executes at its original location. These LLVM memory
      // effects justify only retaining the prior state value on normal return.
      continue;
    }
    if (const auto *otherLoad = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (!otherLoad->isSimple()) return nullptr;
      continue;
    }
    if (llvm::isa<llvm::FenceInst, llvm::AtomicRMWInst,
                  llvm::AtomicCmpXchgInst>(&instruction))
      return nullptr;
    if (instruction.mayHaveSideEffects()) return nullptr;
  }
  return nullptr;
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

llvm::BasicBlock *singleTargetFor(const llvm::Value *value,
                                 llvm::SwitchInst &switchInst,
                                 const DeflatteningOptions &options,
                                 std::size_t &nodes) {
  Active active;
  auto values = finiteConstants(value, nodes, options.maxExpressionNodes,
                                options.maxValues, active);
  if (!values || values->empty() ||
      values->front().getBitWidth() !=
          switchInst.getCondition()->getType()->getIntegerBitWidth())
    return nullptr;
  auto *target = targetFor(switchInst, values->front());
  for (const auto &item : *values)
    if (targetFor(switchInst, item) != target) return nullptr;
  return target;
}

bool planDispatcher(llvm::BasicBlock &dispatch, llvm::SwitchInst &switchInst,
                    const DeflatteningOptions &options,
                    std::vector<EdgePlan> &plans, bool &unresolved,
                    bool &budgetExhausted) {
  unresolved = true;
  budgetExhausted = false;
  // Threading skips this block. Even an otherwise pure PHI can carry a
  // business value to a successor; removing one incoming edge would change
  // that value (or make it cease to dominate its uses). Keep such blocks until
  // we can rebuild every live-out definition for the new CFG.
  if (hasEscapingDefinitions(dispatch)) return false;
  auto *state = llvm::dyn_cast<llvm::PHINode>(switchInst.getCondition());
  auto *stateLoad = llvm::dyn_cast<llvm::LoadInst>(switchInst.getCondition());
  if (state) {
    if (!onlyPHIsAndTerminator(dispatch) || state->getParent() != &dispatch ||
        !state->hasOneUse() || !state->getNumIncomingValues() ||
        state->getNumIncomingValues() > options.maxIncomingEdges)
      return false;
  } else if (stateLoad) {
    // A load-backed dispatcher requires an exact reaching store in the same
    // predecessor, with no intervening unknown write or observable barrier.
    if (stateLoad->getParent() != &dispatch || !stateLoad->hasOneUse() ||
        !stateLoad->isSimple() ||
        !onlyPHIsAndOneLoad(dispatch, *stateLoad))
      return false;
  } else {
    return false;
  }

  std::vector<llvm::BasicBlock *> predecessors;
  if (state) {
    predecessors.reserve(state->getNumIncomingValues());
    for (unsigned index = 0; index < state->getNumIncomingValues(); ++index)
      predecessors.push_back(state->getIncomingBlock(index));
  } else {
    for (auto *predecessor : llvm::predecessors(&dispatch))
      predecessors.push_back(predecessor);
    if (predecessors.empty() || predecessors.size() > options.maxIncomingEdges)
      return false;
  }

  std::unordered_set<const llvm::BasicBlock *> seen;
  std::vector<EdgePlan> candidate;
  candidate.reserve(predecessors.size());
  bool blocked = false;
  for (unsigned index = 0; index < predecessors.size(); ++index) {
    auto *predecessor = predecessors[index];
    if (!predecessor || predecessor == &dispatch ||
        !seen.insert(predecessor).second) {
      blocked = true;
      continue;
    }
    auto *terminator = predecessor->getTerminator();
    // Updating the destination list of indirectbr/callbr does not update its
    // computed address or exceptional control-transfer semantics.
    if (!terminator ||
        !llvm::isa<llvm::BranchInst, llvm::SwitchInst>(terminator) ||
        successorCount(terminator, &dispatch) != 1) {
      blocked = true;
      continue;
    }
    auto edge = soleSuccessorIndex(terminator, &dispatch);
    if (!edge) {
      blocked = true;
      continue;
    }

    std::size_t nodes = 0;
    llvm::Value *incomingValue = nullptr;
    if (state) {
      incomingValue = state->getIncomingValue(index);
    } else {
      auto *store = reachingStateStore(*predecessor, *stateLoad, nodes,
                                       options.maxExpressionNodes);
      if (!store) {
        if (nodes > options.maxExpressionNodes) budgetExhausted = true;
        blocked = true;
        continue;
      }
      incomingValue = store->getValueOperand();
    }

    llvm::BasicBlock *target = nullptr;
    llvm::BasicBlock *falseTarget = nullptr;
    llvm::Value *condition = nullptr;
    auto *select = llvm::dyn_cast<llvm::SelectInst>(incomingValue);
    if (select) {
      ++nodes;
      Active active;
      auto choices = finiteConstants(select->getCondition(), nodes,
                                     options.maxExpressionNodes,
                                     options.maxValues, active);
      if (choices && choices->size() == 1 &&
          choices->front().getBitWidth() == 1) {
        // A proved condition evaluates only the chosen arm, including when
        // the unchosen arm is poison. This also keeps computed constants.
        target = singleTargetFor(choices->front().isZero()
                                     ? select->getFalseValue()
                                     : select->getTrueValue(),
                                 switchInst, options, nodes);
      } else if (nodes <= options.maxExpressionNodes &&
                 llvm::isGuaranteedNotToBeUndefOrPoison(select->getCondition())) {
        // Reuse the same defined condition. Never strip a freeze on the STATE
        // to expose a poison select, or invent a choice from only a value set.
        target = singleTargetFor(select->getTrueValue(), switchInst, options, nodes);
        falseTarget = singleTargetFor(select->getFalseValue(), switchInst, options, nodes);
        if (target && falseTarget && target != falseTarget)
          condition = select->getCondition();
        else if (target != falseTarget)
          target = nullptr;
      }
    } else {
      target = singleTargetFor(incomingValue, switchInst, options, nodes);
    }
    if (nodes > options.maxExpressionNodes) budgetExhausted = true;
    if (!target || hasAnyPHI(*target) ||
        (condition && (!falseTarget || hasAnyPHI(*falseTarget)))) {
      blocked = true;
      continue;
    }
    if (condition) {
      auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator);
      if (!branch || branch->isConditional() || target == &dispatch ||
          falseTarget == &dispatch) {
        blocked = true;
        continue;
      }
    }
    if (target == &dispatch) continue; // a self-dispatch edge already is direct
    candidate.push_back({predecessor, terminator, *edge, target,
                          condition, falseTarget});
  }
  // A fact authorizes only its own edge. Unknown CALL results, unknown values
  // or unsupported incoming paths continue through the original dispatcher;
  // they do not invalidate independent proofs on other predecessors.
  plans = std::move(candidate);
  unresolved = blocked;
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
      bool unresolved = false;
      bool expressionBudget = false;
      const bool planned = planDispatcher(block, *switchInst, options, plans,
                                          unresolved, expressionBudget);
      if (expressionBudget) {
        stats.complete = false;
        ++stats.budgetExhausted;
      }
      if (!planned) {
        ++stats.unresolvedDispatchers;
        continue;
      }
      for (const auto &plan : plans) {
        // Keep the old edge while LLVM removes its PHI incoming values.
        block.removePredecessor(plan.predecessor);
        if (plan.condition) {
          auto *branch = llvm::BranchInst::Create(
              plan.target, plan.falseTarget, plan.condition, plan.terminator);
          branch->setDebugLoc(plan.terminator->getDebugLoc());
          plan.terminator->eraseFromParent();
        } else {
          plan.terminator->setSuccessor(plan.successorIndex, plan.target);
        }
        ++stats.edgesRewritten;
      }
      ++stats.dispatchersRewritten;
      if (unresolved) ++stats.unresolvedDispatchers;
      stats.changed = true;
    }
  }
  return stats;
}

} // namespace dragon::llvmlifter
