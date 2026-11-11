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

#include <algorithm>
#include <optional>
#include <unordered_map>
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
constexpr std::size_t MaxContextBlocks = 6;
constexpr std::size_t MaxContextInstructions = 256;

bool supportedSSAType(llvm::Type *type) {
  return type->isPointerTy() ||
         (type->isIntegerTy() && type->getIntegerBitWidth() <= MaxIntegerBits);
}

bool safeBlock(const llvm::BasicBlock &block) {
  if (block.isEHPad() || block.hasAddressTaken()) return false;
  const auto *terminator = block.getTerminator();
  if (!llvm::isa<llvm::BranchInst, llvm::SwitchInst>(terminator) ||
      terminator->getNumSuccessors() > MaxIncomingEdges)
    return false;
  std::unordered_set<const llvm::BasicBlock *> successors;
  for (auto *successor : llvm::successors(&block))
    if (!successors.insert(successor).second) return false;
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
  const std::unordered_map<const llvm::Value *, llvm::APInt> *contextConstants = nullptr;
  const std::unordered_set<const llvm::BasicBlock *> *contextBlocks = nullptr;

  std::optional<llvm::APInt> evaluate(const llvm::Value *value,
                                     std::size_t depth = 0,
                                     bool incomingContext = true,
                                     bool allowContext = true) {
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
    if (instruction && contextBlocks && !contextBlocks->contains(instruction->getParent()))
      allowContext = false;
    // This includes samesign ICMP, nneg ZEXT, disjoint OR and nowrap/exact
    // arithmetic. Modular evaluation of these instructions could hide poison.
    if (!instruction || instruction->hasPoisonGeneratingFlags() ||
        (!incomingContext && instruction->getParent() == &block))
      return std::nullopt;
    if (allowContext && contextConstants)
      if (const auto found = contextConstants->find(value); found != contextConstants->end())
        return found->second;
    if (!active.insert(value).second) return std::nullopt;
    auto result = evaluateInstruction(*instruction, depth,
        incomingContext && instruction->getParent() == &block, allowContext);
    active.erase(value);
    return result;
  }

  std::optional<llvm::APInt> evaluateInstruction(
      const llvm::Instruction &instruction, std::size_t depth,
      bool incomingContext, bool allowContext) {
    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
      if (phi->getParent() != &block) return std::nullopt;
      const auto index = phi->getBasicBlockIndex(&pred);
      if (index < 0) return std::nullopt;
      usesIncomingPHI = true;
      // An incoming expression may refer to an earlier loop execution. Once
      // outside this block, never substitute this invocation's PHI arms.
      return evaluate(phi->getIncomingValue(index), depth + 1, false, allowContext);
    }
    if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction))
      // Only a genuinely proved constant passes through freeze. In particular,
      // two dynamic freezes, or a freeze of poison, are never correlated.
      return evaluate(freeze->getOperand(0), depth + 1, incomingContext, allowContext);
    if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
      const auto condition = evaluate(select->getCondition(), depth + 1, incomingContext, allowContext);
      if (!condition || condition->getBitWidth() != 1) return std::nullopt;
      // LLVM selects only this arm. The other arm may be unknown or poison.
      return evaluate(condition->isZero() ? select->getFalseValue()
                                           : select->getTrueValue(), depth + 1, incomingContext, allowContext);
    }
    if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
      auto operand = evaluate(cast->getOperand(0), depth + 1, incomingContext, allowContext);
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
      const auto left = evaluate(compare->getOperand(0), depth + 1, incomingContext, allowContext);
      const auto right = evaluate(compare->getOperand(1), depth + 1, incomingContext, allowContext);
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
    const auto left = evaluate(binary->getOperand(0), depth + 1, incomingContext, allowContext);
    const auto right = evaluate(binary->getOperand(1), depth + 1, incomingContext, allowContext);
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

struct ContextPath {
  std::vector<llvm::BasicBlock *> blocks;
  // For every block except the last, this is the sole edge routed to the
  // following copy. All other edges, including reentries, keep original CFG.
  std::vector<unsigned> forwardEdges;
  std::size_t instructions = 0;
};

bool contains(const ContextPath &path, const llvm::BasicBlock *block) {
  return std::find(path.blocks.begin(), path.blocks.end(), block) != path.blocks.end();
}

// An earlier instruction may observe a previous execution of a block that the
// forward context could otherwise include. Such an old value must not be
// remapped to the new copy. Only selected PHI arms execute on this path.
bool observesEarlierExecution(const ContextPath &path,
                               const llvm::BasicBlock &next,
                               const llvm::BasicBlock &pred) {
  for (std::size_t index = 0; index < path.blocks.size(); ++index) {
    auto *selectedPred = index ? path.blocks[index - 1] : &pred;
    for (const auto &instruction : *path.blocks[index]) {
      if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
        const auto *incoming = llvm::dyn_cast<llvm::Instruction>(
            phi->getIncomingValueForBlock(selectedPred));
        if (incoming && incoming->getParent() == &next) return true;
        continue;
      }
      for (const auto &operand : instruction.operands())
        if (const auto *incoming = llvm::dyn_cast<llvm::Instruction>(operand))
          if (incoming->getParent() == &next) return true;
    }
  }
  return false;
}

bool canExtend(const ContextPath &path, llvm::BasicBlock &next,
                llvm::BasicBlock &pred, llvm::DominatorTree &dom,
                bool &exhausted) {
  if (contains(path, &next) || &next == &pred || !dom.isReachableFromEntry(&next) ||
      observesEarlierExecution(path, next, pred))
    return false;
  auto *previous = path.blocks.back();
  const auto *terminator = next.getTerminator();
  if (!llvm::isa<llvm::BranchInst, llvm::SwitchInst>(terminator)) return false;
  if (next.size() > MaxBlockInstructions ||
      next.size() > MaxContextInstructions - path.instructions ||
      llvm::pred_size(&next) > MaxIncomingEdges ||
      terminator->getNumSuccessors() > MaxIncomingEdges) {
    exhausted = true;
    return false;
  }
  if (!safeBlock(next)) return false;
  for (const auto &phi : next.phis()) {
    const auto incomingIndex = phi.getBasicBlockIndex(previous);
    if (incomingIndex < 0) return false;
    auto *incoming = phi.getIncomingValue(incomingIndex);
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(incoming)) return false;
    if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(incoming))
      if (instruction->getParent() == &next ||
          !dom.dominates(instruction, previous->getTerminator()))
        return false;
  }
  return true;
}

ContextPath contextPath(llvm::BranchInst &branch, llvm::BasicBlock &pred,
                         unsigned selected, llvm::DominatorTree &dom,
                         bool &exhausted) {
  ContextPath result{{branch.getParent()}, {}, branch.getParent()->size()};
  bool pathBudget = false;
  while (true) {
    auto *tail = result.blocks.back();
    auto *terminator = llvm::dyn_cast<llvm::BranchInst>(tail->getTerminator());
    if (!terminator) break; // A real runtime-target switch remains terminal.
    llvm::BasicBlock *next = nullptr;
    unsigned edge = 0;
    if (result.blocks.size() == 1) {
      edge = selected;
      auto *candidate = branch.getSuccessor(selected);
      if (canExtend(result, *candidate, pred, dom, pathBudget)) next = candidate;
    } else {
      // Keep an unknown conditional branch intact. Structural preference for
      // a terminal dispatch or another check is only a copying heuristic;
      // every other outcome still executes the original successor.
      unsigned bestRank = 0;
      for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index) {
        auto *candidate = terminator->getSuccessor(index);
        if (!canExtend(result, *candidate, pred, dom, pathBudget)) continue;
        const auto *candidateTerm = candidate->getTerminator();
        const auto *candidateBranch = llvm::dyn_cast<llvm::BranchInst>(candidateTerm);
        const unsigned rank = llvm::isa<llvm::SwitchInst>(candidateTerm) ? 3
                            : candidateBranch->isConditional() ? 2 : 1;
        if (rank > bestRank) {
          next = candidate;
          edge = index;
          bestRank = rank;
        }
      }
    }
    if (!next) break;
    if (result.blocks.size() == MaxContextBlocks) {
      pathBudget = true;
      break;
    }
    result.forwardEdges.push_back(edge);
    result.blocks.push_back(next);
    result.instructions += next->size();
  }
  if (pathBudget) {
    exhausted = true;
    // An overlong bridge prefix may not reach any useful join at all. Keep
    // the original one-block precision rather than adding budgeted copies.
    return {{branch.getParent()}, {}, branch.getParent()->size()};
  }
  return result;
}

bool usefulContext(const ContextPath &path, llvm::BasicBlock &pred,
                    const DispatcherSpecializationOptions &options,
                    StateGuardEdgeBudget &budget, std::size_t &edgeNodes,
                    bool &exhausted) {
  std::unordered_map<const llvm::Value *, llvm::APInt> constants;
  std::unordered_set<const llvm::BasicBlock *> prefix;
  for (std::size_t index = 0; index < path.blocks.size(); ++index) {
    auto *block = path.blocks[index];
    auto *selectedPred = index ? path.blocks[index - 1] : &pred;
    prefix.insert(block);
    for (const auto &phi : block->phis()) {
      ConditionProof proof{*block, *selectedPred, options.maxExpressionNodes - edgeNodes,
                           budget, exhausted};
      proof.contextConstants = &constants;
      proof.contextBlocks = &prefix;
      const auto value = proof.evaluate(&phi);
      edgeNodes += proof.nodes;
      if (budget.stopped) return false;
      if (!value) continue;
      if (index != 0) return true;
      constants.emplace(&phi, *value);
    }
  }
  // Copying an unrelated successor check does not advance a proved state
  // relation. Keep the old single-block shape unless a downstream join gains
  // an actual constant PHI context, with poison and old-execution rules intact.
  return false;
}

std::optional<std::size_t> preflightRepair(
    const ContextPath &path, const llvm::Function &function,
    bool &exhausted) {
  std::size_t liveOuts = 0;
  std::size_t externalUses = 0;
  for (auto *block : path.blocks)
    for (const auto &instruction : *block) {
      bool live = false;
      for (const auto &use : instruction.uses()) {
        // A copied switch can reenter an original path block. Original uses
        // in other region blocks must therefore be repaired too.
        if (!needsRepair(use, *block)) continue;
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
  if (liveOuts && function.size() + path.blocks.size() > MaxRepairCells / liveOuts) {
    exhausted = true;
    return std::nullopt;
  }
  return liveOuts * (function.size() + path.blocks.size());
}

void cloneEdge(llvm::BranchInst &branch, llvm::BasicBlock &pred,
                unsigned successorIndex, unsigned selected,
                const ContextPath &path) {
  auto &block = *branch.getParent();
  auto &function = *block.getParent();
  llvm::ValueToValueMapTy map;
  std::vector<llvm::BasicBlock *> clones;
  std::vector<llvm::Instruction *> definitions;
  auto mapped = [&](llvm::Value *value) -> llvm::Value * {
    if (llvm::Value *copy = map.lookup(value)) return copy;
    return value; // Preserve external SSA values, including unknown freezes.
  };
  for (std::size_t index = 0; index < path.blocks.size(); ++index) {
    for (auto &instruction : *path.blocks[index])
      if (!instruction.getType()->isVoidTy()) definitions.push_back(&instruction);
    clones.push_back(llvm::CloneBasicBlock(path.blocks[index], map,
        index ? ".state.context" : ".state.edge", &function));
  }
  for (std::size_t index = 0; index < path.blocks.size(); ++index) {
    auto *selectedPred = index ? path.blocks[index - 1] : &pred;
    for (auto &phi : path.blocks[index]->phis())
      map[&phi] = mapped(phi.getIncomingValueForBlock(selectedPred));
    for (auto iterator = clones[index]->begin();
         iterator != clones[index]->end() && llvm::isa<llvm::PHINode>(*iterator);)
      (iterator++)->eraseFromParent();
  }
  for (auto *clone : clones)
    for (auto &instruction : *clone)
      if (!instruction.isTerminator())
        llvm::RemapInstruction(&instruction, map,
            llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges);

  auto *clonedBranch = clones.front()->getTerminator();
  llvm::BranchInst::Create(path.blocks.size() > 1 ? clones[1]
                                                : branch.getSuccessor(selected),
                          clonedBranch->getIterator());
  clonedBranch->eraseFromParent();
  for (std::size_t index = 1; index < path.blocks.size(); ++index) {
    auto *terminator = clones[index]->getTerminator();
    if (auto *copy = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
      if (copy->isConditional()) copy->setCondition(mapped(copy->getCondition()));
    } else if (auto *copy = llvm::dyn_cast<llvm::SwitchInst>(terminator)) {
      copy->setCondition(mapped(copy->getCondition()));
    }
    if (index + 1 < path.blocks.size())
      terminator->setSuccessor(path.forwardEdges[index], clones[index + 1]);
  }
  for (std::size_t index = 0; index < path.blocks.size(); ++index) {
    auto *original = path.blocks[index];
    auto *clone = clones[index];
    const auto *terminator = original->getTerminator();
    for (unsigned edge = 0; edge < terminator->getNumSuccessors(); ++edge) {
      if (index == 0 && edge != selected) continue;
      if (index + 1 < path.blocks.size() && edge == path.forwardEdges[index]) continue;
      auto *target = terminator->getSuccessor(edge);
      // Every non-forward edge, even one to an earlier original path member,
      // starts a fresh original execution. Never make a loop among copies.
      for (auto &phi : target->phis())
        phi.addIncoming(mapped(phi.getIncomingValueForBlock(original)), clone);
    }
  }
  pred.getTerminator()->setSuccessor(successorIndex, clones.front());
  // Retain the original PHI objects until every outside/loop use is repaired.
  for (auto &phi : block.phis()) phi.removeIncomingValue(&pred, false);

  for (auto *definition : definitions) {
    llvm::SmallVector<llvm::Use *, 32> uses;
    for (auto &use : definition->uses())
      if (needsRepair(use, *definition->getParent())) uses.push_back(&use);
    if (uses.empty()) continue;
    auto *origin = definition->getParent();
    const auto position = std::find(path.blocks.begin(), path.blocks.end(), origin)
                        - path.blocks.begin();
    llvm::SSAUpdater updater;
    updater.Initialize(definition->getType(), "state.edge.live");
    updater.AddAvailableValue(origin, definition);
    updater.AddAvailableValue(clones[position], map[definition]);
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
    const auto selected = condition->isZero() ? 1u : 0u;
    auto path = contextPath(branch, *pred, selected, dom, exhausted);
    if (path.blocks.size() > 1 &&
        !usefulContext(path, *pred, options, budget, proof.nodes, exhausted))
      path = {{&block}, {}, block.size()};
    std::optional<std::size_t> repair;
    for (;;) {
      repair = preflightRepair(path, function, exhausted);
      if (repair && path.blocks.size() <= MaxClones - budget.clones &&
          path.instructions <= MaxClonedInstructions - budget.clonedInstructions &&
          *repair <= MaxRepairCells - budget.repairCells)
        break;
      // An extension must not hide the old one-block proved rewrite. Retry
      // progressively shorter prefixes before declaring this seed budgeted.
      if (path.blocks.size() == 1) break;
      path.instructions -= path.blocks.back()->size();
      path.blocks.pop_back();
      path.forwardEdges.pop_back();
      if (repair) exhausted = true;
    }
    if (!repair || path.blocks.size() > MaxClones - budget.clones ||
        path.instructions > MaxClonedInstructions - budget.clonedInstructions ||
        *repair > MaxRepairCells - budget.repairCells) {
      if (repair) {
        exhausted = true;
        budget.stopped = true;
      }
      continue;
    }
    cloneEdge(branch, *pred, successorIndex, selected, path);
    budget.clones += path.blocks.size();
    budget.clonedInstructions += path.instructions;
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
