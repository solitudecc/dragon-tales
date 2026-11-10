#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <dragon/lifters/llvm/CallEffectSummary.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Analysis/ValueTracking.h>

#include <algorithm>
#include <limits>
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

bool onlyPureInstructionsAndLoad(const llvm::BasicBlock &block,
                                 const llvm::LoadInst &load) {
  bool sawLoad = false;
  for (const auto &instruction : block) {
    if (&instruction == block.getTerminator()) break;
    if (&instruction == &load) {
      if (sawLoad) return false;
      sawLoad = true;
      continue;
    }
    if (llvm::isa<llvm::PHINode>(&instruction)) continue;
    if (instruction.mayHaveSideEffects()) return false;
  }
  return sawLoad;
}

llvm::StoreInst *immediateStateStore(llvm::BasicBlock &block,
                                     const llvm::Value *pointer) {
  auto iterator = block.getTerminator()->getIterator();
  if (iterator == block.begin()) return nullptr;
  --iterator;
  auto *store = llvm::dyn_cast<llvm::StoreInst>(&*iterator);
  if (!store || store->isVolatile() || store->isAtomic() ||
      store->getPointerOperand() != pointer)
    return nullptr;
  return store;
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

std::optional<std::uint64_t> stateSlotOffset(
    const llvm::Value *pointer, const llvm::Function &function,
    const llvm::DataLayout &layout,
    std::unordered_set<const llvm::Value *> &active) {
  if (!pointer || !active.insert(pointer).second) return std::nullopt;
  auto done = [&](std::optional<std::uint64_t> result) {
    active.erase(pointer);
    return result;
  };
  auto *value = pointer->stripPointerCasts();
  if (value != pointer) return done(stateSlotOffset(value, function, layout, active));
  if (function.arg_size() == 0) return done(std::nullopt);
  if (value == function.getArg(0)) return done(std::uint64_t{0});
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
    auto base = stateSlotOffset(gep->getPointerOperand(), function, layout, active);
    if (!base) return done(std::nullopt);
    llvm::APInt offset(64, 0);
    if (!gep->accumulateConstantOffset(layout, offset) || offset.isNegative())
      return done(std::nullopt);
    const auto delta = offset.getZExtValue();
    if (*base > std::numeric_limits<std::uint64_t>::max() - delta)
      return done(std::nullopt);
    return done(*base + delta);
  }
  // The lifted state object is commonly loaded from state+24 before register
  // slots are addressed.  Once that load is proven to originate at arg0, it
  // becomes the zero point for subsequent state GEPs.
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
    if (load->getType()->isPointerTy() &&
        stateSlotOffset(load->getPointerOperand(), function, layout, active))
      return done(std::uint64_t{0});
  }
  return done(std::nullopt);
}

std::optional<std::uint64_t> stateSlotOffset(const llvm::Value *pointer,
                                             const llvm::Function &function,
                                             const llvm::DataLayout &layout) {
  std::unordered_set<const llvm::Value *> active;
  return stateSlotOffset(pointer, function, layout, active);
}

bool rangesOverlap(std::uint64_t leftOffset, std::uint32_t leftBytes,
                   std::uint64_t rightOffset, std::uint32_t rightBytes) {
  if (!leftBytes || !rightBytes) return false;
  if (leftOffset > std::numeric_limits<std::uint64_t>::max() - leftBytes ||
      rightOffset > std::numeric_limits<std::uint64_t>::max() - rightBytes)
    return true;
  return leftOffset < rightOffset + rightBytes &&
         rightOffset < leftOffset + leftBytes;
}

std::optional<unsigned> integerBytes(const llvm::Type *type) {
  if (!type || !type->isIntegerTy()) return std::nullopt;
  const unsigned bits = type->getIntegerBitWidth();
  if (!bits || bits % 8 || bits > 64) return std::nullopt;
  return bits / 8;
}

std::optional<llvm::APInt> directCallerStateConstant(
    const llvm::CallBase &call, std::uint64_t slot, unsigned bytes) {
  const auto *caller = call.getFunction();
  if (!caller || caller->arg_size() == 0 || !call.arg_size())
    return std::nullopt;
  // The direct-store proof is relative to the caller's outer GuestState
  // argument. Do not normalize a separately loaded pointer to the same zero
  // point: two nested state objects could otherwise be confused.
  if (call.getArgOperand(0)->stripPointerCasts() != caller->getArg(0))
    return std::nullopt;

  auto *block = call.getParent();
  for (auto iterator = call.getIterator(); iterator != block->begin();) {
    --iterator;
    auto &instruction = *iterator;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      if (store->isVolatile() || store->isAtomic()) return std::nullopt;
      const auto storeOffset = stateSlotOffset(
          store->getPointerOperand(), *caller,
          caller->getParent()->getDataLayout());
      const auto storeBytes = integerBytes(store->getValueOperand()->getType());
      if (!storeOffset || !storeBytes) {
        const auto *underlying = llvm::getUnderlyingObject(
            store->getPointerOperand(), 32);
        if (llvm::isa<llvm::AllocaInst>(underlying)) continue;
        return std::nullopt;
      }
      if (!rangesOverlap(*storeOffset, *storeBytes, slot, bytes)) continue;
      if (*storeOffset != slot || *storeBytes != bytes) return std::nullopt;
      const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(
          store->getValueOperand());
      if (!constant || constant->getBitWidth() != bytes * 8)
        return std::nullopt;
      return constant->getValue();
    }
    if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      if (!otherCall->doesNotAccessMemory() && !otherCall->onlyReadsMemory())
        return std::nullopt;
      continue;
    }
    if (instruction.mayWriteToMemory()) return std::nullopt;
  }
  return std::nullopt;
}

bool directCallUsersOnly(const llvm::Function &callee,
                         std::size_t maxCallSites,
                         std::vector<const llvm::CallBase *> &calls) {
  for (const auto &use : callee.uses()) {
    const auto *call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
    if (!call || call->getCalledFunction() != &callee) return false;
    calls.push_back(call);
    if (calls.size() > maxCallSites) return false;
  }
  return !calls.empty();
}

enum class CrossCallProofKind { Summary, DirectStore };

struct CrossCallProof {
  FiniteConstants values;
  CrossCallProofKind kind = CrossCallProofKind::Summary;
};

std::optional<CrossCallProof> crossCallProof(
    const llvm::CallBase &call, std::uint64_t slot, unsigned bytes) {
  if (call.getMetadata("dragon.call.effect")) {
    const auto summary = getCallEffectSummary(call);
    if (!summary || !summary->proven ||
        summary->memory != CallMemoryEffectKind::KnownRanges)
      return std::nullopt;
    std::size_t selectorWrites = 0;
    for (const auto &write : summary->writes) {
      if (write.base != CallMemoryRange::Base::GuestState ||
          !rangesOverlap(write.offset, write.bytes, slot, bytes))
        continue;
      ++selectorWrites;
      if (write.offset != slot || write.bytes != bytes) return std::nullopt;
    }
    if (selectorWrites != 1) return std::nullopt;

    CrossCallProof proof;
    proof.kind = CrossCallProofKind::Summary;
    for (const auto &constant : summary->memoryConstants) {
      if (constant.range.base != CallMemoryRange::Base::GuestState ||
          constant.range.offset != slot || constant.range.bytes != bytes)
        continue;
      if (!constant.complete) return std::nullopt;
      if (!appendFinite(proof.values,
                        llvm::APInt(bytes * 8, constant.value), 2))
        return std::nullopt;
    }
    if (proof.values.size() != 1) return std::nullopt;
    return proof;
  }

  auto constant = directCallerStateConstant(call, slot, bytes);
  if (!constant) return std::nullopt;
  return CrossCallProof{FiniteConstants{*constant},
                        CrossCallProofKind::DirectStore};
}

std::optional<llvm::BasicBlock *> crossCallTarget(
    llvm::Function &callee, llvm::BasicBlock &dispatch,
    llvm::SwitchInst &switchInst, const DeflatteningOptions &options,
    std::size_t &summariesConsumed, std::size_t &directStoresConsumed) {
  // An externally visible function may have callers outside this module.  A
  // module-local summary cannot prove those entries, so keep the dispatcher
  // explicit unless linkage itself closes the call graph.
  if (!callee.hasLocalLinkage()) return std::nullopt;
  auto *stateLoad = llvm::dyn_cast<llvm::LoadInst>(switchInst.getCondition());
  if (!options.consumeCallEffectSummaries || !options.maxCrossCallSites ||
      !stateLoad || !onlyPureInstructionsAndLoad(dispatch, *stateLoad))
    return std::nullopt;
  if (!stateLoad || !stateLoad->hasOneUse() ||
      !stateLoad->getType()->isIntegerTy())
    return std::nullopt;
  const auto slot = stateSlotOffset(stateLoad->getPointerOperand(), callee,
                                    callee.getParent()->getDataLayout());
  if (!slot) return std::nullopt;
  const unsigned width = stateLoad->getType()->getIntegerBitWidth();
  if (!width || width % 8) return std::nullopt;
  const auto bytes = width / 8;

  std::vector<const llvm::CallBase *> calls;
  if (!directCallUsersOnly(callee, options.maxCrossCallSites, calls))
    return std::nullopt;

  llvm::BasicBlock *chosenTarget = nullptr;
  std::size_t consumed = 0;
  for (const auto *call : calls) {
    auto proof = crossCallProof(*call, *slot, bytes);
    if (!proof || proof->values.empty()) return std::nullopt;
    if (proof->kind == CrossCallProofKind::Summary)
      ++summariesConsumed;
    else
      ++directStoresConsumed;
    for (const auto &value : proof->values) {
      auto *target = targetFor(switchInst, value);
      if (!chosenTarget)
        chosenTarget = target;
      else if (chosenTarget != target)
        return std::nullopt;
    }
    ++consumed;
  }
  if (!chosenTarget || chosenTarget == &dispatch || hasAnyPHI(*chosenTarget) ||
      consumed != calls.size())
    return std::nullopt;
  if (summariesConsumed + directStoresConsumed != consumed)
    return std::nullopt;
  return chosenTarget;
}

void replaceSwitchWithBranch(llvm::BasicBlock &block,
                             llvm::SwitchInst &switchInst,
                             llvm::BasicBlock &target) {
  std::unordered_set<llvm::BasicBlock *> successors;
  for (unsigned index = 0; index < switchInst.getNumSuccessors(); ++index)
    successors.insert(switchInst.getSuccessor(index));
  for (auto *successor : successors)
    if (successor != &target) successor->removePredecessor(&block);
  llvm::BranchInst::Create(&target, &switchInst);
  switchInst.eraseFromParent();
}

bool planDispatcher(llvm::BasicBlock &dispatch, llvm::SwitchInst &switchInst,
                    const DeflatteningOptions &options,
                    std::vector<EdgePlan> &plans) {
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
    // A load-backed dispatcher is accepted only when each incoming block ends
    // in the exact store feeding the load. We do not chase aliases, calls or
    // stores in other functions; those remain unresolved.
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
  for (unsigned index = 0; index < predecessors.size(); ++index) {
    auto *predecessor = predecessors[index];
    if (!predecessor || predecessor == &dispatch ||
        !seen.insert(predecessor).second)
      return false;
    auto *terminator = predecessor->getTerminator();
    // Updating the destination list of indirectbr/callbr does not update its
    // computed address or exceptional control-transfer semantics.
    if (!terminator ||
        !llvm::isa<llvm::BranchInst, llvm::SwitchInst>(terminator) ||
        successorCount(terminator, &dispatch) != 1)
      return false;
    auto edge = soleSuccessorIndex(terminator, &dispatch);
    if (!edge) return false;

    const llvm::Value *incomingValue = nullptr;
    if (state) {
      incomingValue = state->getIncomingValue(index);
    } else {
      auto *store = immediateStateStore(*predecessor,
                                        stateLoad->getPointerOperand());
      if (!store) return false;
      incomingValue = store->getValueOperand();
    }

    std::size_t nodes = 0;
    Active active;
    auto values = finiteConstants(incomingValue, nodes,
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
        std::size_t summariesConsumed = 0;
        std::size_t directStoresConsumed = 0;
        if (auto target = crossCallTarget(function, block, *switchInst,
                                          options, summariesConsumed,
                                          directStoresConsumed)) {
          replaceSwitchWithBranch(block, *switchInst, **target);
          ++stats.dispatchersRewritten;
          ++stats.crossCallDispatchersRewritten;
          stats.crossCallSummariesConsumed += summariesConsumed;
          stats.crossCallDirectStoresConsumed += directStoresConsumed;
          stats.changed = true;
          continue;
        }
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
