#include "GuestMemoryEdges.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/PHITransAddr.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/SSAUpdater.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {

// These caps bound code growth and SSA repair, independently of maxRewrites.
constexpr std::size_t MaxCorridorBlocks = 6;
constexpr std::size_t MaxCorridorInstructions = 256;
constexpr std::size_t MaxCandidates = 512;
constexpr std::size_t MaxIncomingEdges = 512;
constexpr std::size_t MaxEdgeProofs = 2048;
constexpr std::size_t MaxIndexNodes = 256;
constexpr std::size_t MaxIndexDepth = 64;
constexpr std::size_t MaxModuleIndexNodes = 65536;
constexpr std::size_t MaxClones = 64;
constexpr std::size_t MaxClonedInstructions = 16384;
constexpr std::size_t MaxLiveOuts = 128;
constexpr std::size_t MaxExternalUses = 8192;
constexpr std::size_t MaxRepairCells = 262144;

struct ByteAddress {
  llvm::Value *base = nullptr;
  llvm::Value *index = nullptr;
};

struct Corridor {
  // In execution order, starting with the multiply-entered block.
  std::vector<llvm::BasicBlock *> blocks;
  std::unordered_set<llvm::BasicBlock *> members;
  // New hub paths keep exceptional live-out repair outside their supported
  // domain; the historical direct-edge path retains its existing contract.
  bool expanded = false;
};

bool supportedSSAType(llvm::Type *type) {
  return type->isIntegerTy() || type->isPointerTy();
}

std::optional<ByteAddress> byteAddress(
    llvm::Value *pointer,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout) {
  // Do not fold a host integer expression or infer disjointness. The only
  // address equivalence used below is an existing SSA value after PHI
  // translation. In particular, separately loaded Guest bases are not equal.
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
  if (!gep || !gep->getSourceElementType()->isIntegerTy(8) ||
      gep->getNumIndices() != 1 || gep->getPointerAddressSpace() != 0 ||
      layout.getIndexSizeInBits(0) != 64 ||
      !gep->getOperand(1)->getType()->isIntegerTy(64) ||
      gep->isInBounds() || gep->hasNoUnsignedSignedWrap() ||
      gep->hasNoUnsignedWrap() ||
      !memoryBases.contains(gep->getPointerOperand()))
    return std::nullopt;
  if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(gep->getOperand(1)))
    return std::nullopt;
  return ByteAddress{gep->getPointerOperand(), gep->getOperand(1)};
}

bool plainLoad(const llvm::LoadInst &load) {
  const auto *type = llvm::dyn_cast<llvm::IntegerType>(load.getType());
  return load.isSimple() && type && type->getBitWidth() != 0 &&
         type->getBitWidth() <= 128 && type->getBitWidth() % 8 == 0;
}

bool safeCorridorBlock(const llvm::BasicBlock &block) {
  if (block.isEHPad() || block.hasAddressTaken()) return false;
  const auto *branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
  if (!branch || (branch->isConditional() &&
                  branch->getSuccessor(0) == branch->getSuccessor(1)))
    return false;
  for (const auto &instruction : block) {
    if (instruction.isUsedByMetadata() ||
        llvm::isa<llvm::CallBase, llvm::AllocaInst, llvm::FenceInst,
                  llvm::AtomicRMWInst, llvm::AtomicCmpXchgInst>(instruction))
      return false;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (!load->isSimple()) return false;
      continue;
    }
    if (instruction.isTerminator()) continue;
    if (instruction.mayHaveSideEffects() || instruction.mayReadOrWriteMemory())
      return false;
    if (!instruction.getType()->isVoidTy() &&
        !supportedSSAType(instruction.getType()))
      return false;
  }
  return true;
}

std::optional<Corridor> findCorridor(llvm::LoadInst &load,
                                    llvm::DominatorTree &dom,
                                    bool &exhausted) {
  Corridor result;
  auto *block = load.getParent();
  std::size_t instructions = 0;
  while (true) {
    if (result.blocks.size() == MaxCorridorBlocks ||
        block->size() > MaxCorridorInstructions - instructions) {
      exhausted = true;
      return std::nullopt;
    }
    if (!dom.isReachableFromEntry(block) ||
        !result.members.insert(block).second || !safeCorridorBlock(*block))
      return std::nullopt;
    instructions += block->size();
    result.blocks.push_back(block);
    auto *predecessor = block->getSinglePredecessor();
    if (!predecessor) {
      if (llvm::pred_size(block) < 2) return std::nullopt;
      break;
    }
    block = predecessor;
  }
  std::reverse(result.blocks.begin(), result.blocks.end());
  // The chain is acyclic; a branch back into any earlier corridor block must
  // never be mistaken for a side exit that can reuse the cloned invocation.
  for (std::size_t index = 0; index < result.blocks.size(); ++index)
    for (auto *successor : llvm::successors(result.blocks[index])) {
      if (!result.members.contains(successor)) continue;
      if (index + 1 == result.blocks.size() ||
          successor != result.blocks[index + 1])
        return std::nullopt;
    }
  return result;
}

llvm::StoreInst *nearestStore(llvm::BasicBlock &pred, std::size_t &instructions,
                              bool &exhausted) {
  auto iterator = pred.getTerminator()->getIterator();
  while (iterator != pred.begin()) {
    if (++instructions > MaxCorridorInstructions) {
      exhausted = true;
      return nullptr;
    }
    auto &instruction = *--iterator;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      return store->isSimple() ? store : nullptr;
    if (llvm::isa<llvm::CallBase, llvm::FenceInst, llvm::AtomicRMWInst,
                  llvm::AtomicCmpXchgInst>(&instruction))
      return nullptr;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (!load->isSimple()) return nullptr;
      continue;
    }
    if (instruction.mayHaveSideEffects()) return nullptr;
  }
  return nullptr;
}

bool selectedEdge(llvm::BasicBlock &pred, llvm::BasicBlock &join,
                   unsigned &successorIndex) {
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(pred.getTerminator());
  if (!branch || pred.isEHPad() || pred.hasAddressTaken()) return false;
  unsigned count = 0;
  for (unsigned index = 0; index < branch->getNumSuccessors(); ++index)
    if (branch->getSuccessor(index) == &join) {
      ++count;
      successorIndex = index;
    }
  return count == 1;
}

using EntryInputs = std::vector<llvm::Value *>;

bool selectedPHIsAvailable(const Corridor &corridor, llvm::BasicBlock &pred,
                            llvm::DominatorTree &dom, EntryInputs &inputs) {
  for (std::size_t position = 0; position < corridor.blocks.size(); ++position) {
    auto *selectedPred = position ? corridor.blocks[position - 1] : &pred;
    for (const auto &instruction : *corridor.blocks[position]) {
      if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
        const auto index = phi->getBasicBlockIndex(selectedPred);
        if (index < 0 || !supportedSSAType(phi->getType())) return false;
        auto *incoming = phi->getIncomingValue(index);
        if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(incoming)) return false;
        if (const auto *value = llvm::dyn_cast<llvm::Instruction>(incoming)) {
          const auto origin = std::find(corridor.blocks.begin(),
              corridor.blocks.end(), value->getParent());
          // Only the first selected PHI can import an earlier execution.
          // Its definition must already exist on the original incoming edge;
          // it is snapshotted separately from all freshly cloned definitions.
          if (origin != corridor.blocks.end()) {
            if (position == 0) {
              if (!dom.dominates(value, pred.getTerminator())) return false;
              if (std::find(inputs.begin(), inputs.end(), incoming) == inputs.end())
                inputs.push_back(incoming);
            } else if (static_cast<std::size_t>(origin - corridor.blocks.begin()) >= position) {
              return false;
            }
          } else if (!dom.dominates(value, selectedPred->getTerminator())) {
            return false;
          }
        }
        continue;
      }
      for (const auto &operand : instruction.operands()) {
        const auto *value = llvm::dyn_cast<llvm::Instruction>(operand);
        if (!value) continue;
        const auto origin = std::find(corridor.blocks.begin(),
            corridor.blocks.end(), value->getParent());
        if (origin != corridor.blocks.end() &&
            static_cast<std::size_t>(origin - corridor.blocks.begin()) > position)
          return false;
      }
    }
  }
  return true;
}

struct IndexProof {
  CheckedMemoryEdgeBudget &budget;
  bool &exhausted;
  std::size_t nodes = 0;

  bool visit(std::size_t depth) {
    if (nodes == MaxIndexNodes || depth > MaxIndexDepth) {
      exhausted = true;
      return false;
    }
    if (budget.indexNodes == MaxModuleIndexNodes) {
      exhausted = true;
      budget.stopped = true;
      return false;
    }
    ++nodes;
    ++budget.indexNodes;
    return true;
  }
};

llvm::Value *incomingForEdge(llvm::PHINode &phi, const Corridor &corridor,
                             llvm::BasicBlock &pred) {
  auto position = std::find(corridor.blocks.begin(), corridor.blocks.end(),
                            phi.getParent());
  if (position == corridor.blocks.end()) return nullptr;
  auto *incomingBlock = position == corridor.blocks.begin()
      ? &pred : *(position - 1);
  const auto index = phi.getBasicBlockIndex(incomingBlock);
  return index < 0 ? nullptr : phi.getIncomingValue(index);
}

struct IndexDependencies {
  std::unordered_set<llvm::Value *> fresh;
  std::unordered_set<llvm::Value *> historical;
};

bool checkedIndexDependencies(
    llvm::Value *value, const Corridor &corridor, llvm::BasicBlock &pred,
    bool translate, IndexProof &proof,
    IndexDependencies &visited, std::size_t depth = 0) {
  if (!value || llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value))
    return false;
  auto &seen = translate ? visited.fresh : visited.historical;
  if (seen.contains(value)) return true;
  if (!proof.visit(depth)) return false;
  seen.insert(value);
  if (llvm::isa<llvm::ConstantInt>(value)) return true;
  if (const auto *argument = llvm::dyn_cast<llvm::Argument>(value))
    return argument->getType()->isIntegerTy();
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (!instruction || !instruction->getType()->isIntegerTy()) return false;
  if (llvm::isa<llvm::FreezeInst>(instruction)) {
    // A shared, already-executed freeze is a stable SSA value. A new freeze
    // in the clone is a different execution and may choose different bits.
    return !translate || !corridor.members.contains(instruction->getParent());
  }
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(instruction))
    return load->isSimple() &&
           (!translate || !corridor.members.contains(instruction->getParent()));
  if (instruction->hasPoisonGeneratingFlags() ||
      instruction->mayHaveSideEffects() || instruction->mayReadOrWriteMemory())
    return false;
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction)) {
    if (translate && corridor.members.contains(phi->getParent()))
      return checkedIndexDependencies(incomingForEdge(*phi, corridor, pred),
          corridor, pred, phi->getParent() != corridor.blocks.front(),
          proof, visited, depth + 1);
    // An outside loop PHI is an existing SSA boundary. Inspect its finite
    // dependency graph for visible undef/poison, with visited-set convergence;
    // do not try to prove a new frame or loop invariant.
  }
  for (auto &operand : instruction->operands())
    if (!checkedIndexDependencies(operand.get(), corridor, pred, translate,
                                   proof, visited, depth + 1))
      return false;
  return true;
}

bool structurallyTranslatedIndex(
    llvm::Value *loaded, llvm::Value *stored, const Corridor &corridor,
    llvm::BasicBlock &pred, IndexProof &proof, std::size_t depth = 0,
    bool historical = false) {
  if (!loaded || !stored || !proof.visit(depth) ||
      llvm::isa<llvm::UndefValue, llvm::PoisonValue>(loaded) ||
      llvm::isa<llvm::UndefValue, llvm::PoisonValue>(stored))
    return false;
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(loaded))
    if (!historical && corridor.members.contains(phi->getParent()))
      return structurallyTranslatedIndex(
          incomingForEdge(*phi, corridor, pred), stored, corridor, pred,
          proof, depth + 1, phi->getParent() == corridor.blocks.front());
  if (loaded == stored) {
    const auto *instruction = llvm::dyn_cast<llvm::Instruction>(loaded);
    if (historical || !instruction || !corridor.members.contains(
            const_cast<llvm::BasicBlock *>(instruction->getParent())))
      return true;
  }
  const auto *left = llvm::dyn_cast<llvm::BinaryOperator>(loaded);
  const auto *right = llvm::dyn_cast<llvm::BinaryOperator>(stored);
  if (!left || !right || left->getOpcode() != right->getOpcode() ||
      (left->getOpcode() != llvm::Instruction::Add &&
       left->getOpcode() != llvm::Instruction::Sub) ||
      !left->getType()->isIntegerTy(64) ||
      left->getType() != right->getType() ||
      left->hasPoisonGeneratingFlags() || right->hasPoisonGeneratingFlags())
    return false;
  return structurallyTranslatedIndex(left->getOperand(0), right->getOperand(0),
             corridor, pred, proof, depth + 1, historical) &&
         structurallyTranslatedIndex(left->getOperand(1), right->getOperand(1),
             corridor, pred, proof, depth + 1, historical);
}

bool needsRepair(const llvm::Use &use, const Corridor &corridor,
                   const llvm::BasicBlock &origin) {
  const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
  if (!corridor.expanded) {
    // findCorridor admits only a single-predecessor forward chain after its
    // original head. No outside edge can bypass an original definition and
    // enter another member of that chain. Keep the historical repair scope
    // (and its actual work charge) for those direct paths.
    if (!user || !corridor.members.contains(
                     const_cast<llvm::BasicBlock *>(user->getParent())))
      return true;
    const auto *phi = llvm::dyn_cast<llvm::PHINode>(user);
    return phi && !corridor.members.contains(phi->getIncomingBlock(use.getOperandNo()));
  }
  if (!user || user->getParent() != &origin) return true;
  // An exit can reenter another original corridor block. Repair those uses
  // too, while keeping ordinary same-block uses on their current definition.
  const auto *phi = llvm::dyn_cast<llvm::PHINode>(user);
  return phi && phi->getIncomingBlock(use.getOperandNo()) != &origin;
}

std::optional<std::size_t> preflightRepair(
    const Corridor &corridor, const EntryInputs &inputs,
    const llvm::Function &function, bool &exhausted) {
  std::size_t liveOuts = 0;
  // Snapshots add original-definition uses and one block to SSA repair.
  std::size_t externalUses = inputs.size();
  for (auto *block : corridor.blocks)
    for (const auto &instruction : *block) {
      bool live = false;
      for (const auto &use : instruction.uses()) {
        if (!needsRepair(use, corridor, *block)) continue;
        const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
        if (!user || user->getFunction() != &function ||
            (corridor.expanded && user->getParent()->isEHPad()) ||
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
  const auto copiedBlocks = corridor.blocks.size() + !inputs.empty();
  if (liveOuts && function.size() + copiedBlocks >
                         MaxRepairCells / liveOuts) {
    exhausted = true;
    return std::nullopt;
  }
  return liveOuts * (function.size() + copiedBlocks);
}

llvm::StoreInst *proveStore(
    llvm::LoadInst &load, const ByteAddress &loadedAddress,
    const Corridor &corridor, llvm::BasicBlock &pred,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, llvm::DominatorTree &dom,
    bool &exhausted, CheckedMemoryEdgeBudget &budget, EntryInputs &inputs) {
  if (corridor.members.contains(&pred) ||
      !selectedPHIsAvailable(corridor, pred, dom, inputs))
    return nullptr;
  auto *base = llvm::dyn_cast<llvm::Instruction>(loadedAddress.base);
  if (!base || corridor.members.contains(base->getParent()) ||
      !dom.dominates(base, pred.getTerminator()))
    return nullptr;
  std::size_t instructions = 0;
  auto *store = nearestStore(pred, instructions, exhausted);
  if (!store || store->getValueOperand()->getType() != load.getType() ||
      !llvm::isGuaranteedNotToBeUndefOrPoison(store->getValueOperand()))
    return nullptr;
  if (auto *value = llvm::dyn_cast<llvm::Instruction>(store->getValueOperand()))
    if (corridor.members.contains(value->getParent())) return nullptr;
  const auto storedAddress = byteAddress(store->getPointerOperand(), memoryBases,
                                        layout);
  if (!storedAddress || loadedAddress.base != storedAddress->base)
    return nullptr;
  if (const auto *index = llvm::dyn_cast<llvm::Instruction>(storedAddress->index))
    if (corridor.members.contains(
            const_cast<llvm::BasicBlock *>(index->getParent())) &&
        (inputs.empty() || !dom.dominates(index, pred.getTerminator())))
      return nullptr;
  IndexProof proof{budget, exhausted};
  IndexDependencies loadedDependencies;
  IndexDependencies storedDependencies;
  if (!checkedIndexDependencies(loadedAddress.index, corridor, pred, true,
                                proof, loadedDependencies) ||
      !checkedIndexDependencies(storedAddress->index, corridor, pred, false,
                                proof, storedDependencies))
    return nullptr;

  // PHITransAddr has no old/fresh execution distinction. Keep its historical
  // non-snapshot path only; snapshot paths use the explicit view below.
  if (inputs.empty()) {
    llvm::PHITransAddr address(loadedAddress.index, layout, nullptr);
    bool translated = true;
    for (std::size_t index = corridor.blocks.size(); index > 1; --index)
      if (!address.translateValue(corridor.blocks[index - 1],
                                  corridor.blocks[index - 2], &dom, true)) {
        translated = false;
        break;
      }
    if (translated && address.translateValue(corridor.blocks.front(), &pred,
                                             &dom, true) == storedAddress->index)
      return store;
  }
  // LLVM's PHITransAddr does not translate Sub expressions. Match only an
  // existing, identical Add/Sub tree after per-edge PHI substitution. This
  // creates no instructions and neither folds flags nor strips freeze.
  return structurallyTranslatedIndex(loadedAddress.index, storedAddress->index,
                                      corridor, pred, proof) ? store : nullptr;
}

void cloneEdge(llvm::LoadInst &load, llvm::StoreInst &store,
                const Corridor &corridor, llvm::BasicBlock &pred,
                unsigned successorIndex, const EntryInputs &inputs,
                MemoryForwardStats &stats) {
  auto &function = *load.getFunction();
  llvm::ValueToValueMapTy map;
  llvm::DenseMap<llvm::Value *, llvm::Value *> oldInputs;
  llvm::BasicBlock *entry = nullptr;
  if (!inputs.empty()) {
    entry = llvm::BasicBlock::Create(function.getContext(),
        corridor.blocks.front()->getName() + ".guest.inputs", &function);
    for (auto *value : inputs) {
      auto *snapshot = llvm::PHINode::Create(value->getType(), 1,
          "guest.edge.old", entry);
      snapshot->addIncoming(value, &pred);
      oldInputs[value] = snapshot;
    }
  }
  std::vector<llvm::BasicBlock *> clones;
  std::vector<llvm::Instruction *> definitions;
  for (auto *block : corridor.blocks) {
    for (auto &instruction : *block)
      if (!instruction.getType()->isVoidTy())
        definitions.push_back(&instruction);
    auto *clone = llvm::CloneBasicBlock(block, map, ".guest.edge", &function);
    map[block] = clone;
    clones.push_back(clone);
  }
  // Every copy has precisely its selected path predecessor. In particular,
  // a non-front join must not retain arms belonging to original unknown paths.
  auto *join = corridor.blocks.front();
  auto *clonedJoin = clones.front();
  for (std::size_t index = 0; index < corridor.blocks.size(); ++index) {
    auto *selectedPred = index ? corridor.blocks[index - 1] : &pred;
    for (auto &phi : corridor.blocks[index]->phis()) {
      auto *incoming = phi.getIncomingValueForBlock(selectedPred);
      llvm::Value *selected = incoming;
      if (index == 0) {
        if (auto *snapshot = oldInputs.lookup(incoming)) selected = snapshot;
      } else if (const auto found = map.find(incoming); found != map.end()) {
        selected = found->second;
      }
      map[&phi] = selected;
    }
    for (auto iterator = clones[index]->begin();
         iterator != clones[index]->end() && llvm::isa<llvm::PHINode>(*iterator);)
      (iterator++)->eraseFromParent();
  }
  for (auto *clone : clones)
    for (auto &instruction : *clone)
      llvm::RemapInstruction(&instruction, map,
          llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges);

  auto *clonedLoad = llvm::cast<llvm::LoadInst>(map[&load]);
  clonedLoad->replaceAllUsesWith(store.getValueOperand());
  map[&load] = store.getValueOperand();
  clonedLoad->eraseFromParent();

  for (std::size_t index = 0; index < corridor.blocks.size(); ++index) {
    auto *original = corridor.blocks[index];
    auto *clone = clones[index];
    for (auto *successor : llvm::successors(original)) {
      if (corridor.members.contains(successor)) continue;
      for (auto &phi : successor->phis()) {
        auto *incoming = phi.getIncomingValueForBlock(original);
        // Values defined outside the corridor remain the same SSA values.
        // MapValue can return null for an unmapped local, even with
        // RF_IgnoreMissingLocals; that flag is handled by RemapInstruction.
        auto *mapped = incoming;
        if (const auto found = map.find(incoming); found != map.end())
          mapped = found->second;
        phi.addIncoming(mapped, clone);
      }
    }
  }
  if (entry) llvm::BranchInst::Create(clonedJoin, entry);
  pred.getTerminator()->setSuccessor(successorIndex, entry ? entry : clonedJoin);
  // Keep original PHI objects alive for the live-out repair. Automatic PHI
  // simplification here could delete a definition still held in the plan.
  for (auto &phi : join->phis()) phi.removeIncomingValue(&pred, false);

  for (auto *definition : definitions) {
    llvm::SmallVector<llvm::Use *, 32> uses;
    for (auto &use : definition->uses())
      if (needsRepair(use, corridor, *definition->getParent())) uses.push_back(&use);
    if (uses.empty()) continue;
    llvm::SmallVector<llvm::PHINode *, 8> phis;
    llvm::SSAUpdater updater(&phis);
    updater.Initialize(definition->getType(), "guest.edge.live");
    updater.AddAvailableValue(definition->getParent(), definition);
    updater.AddAvailableValue(
        llvm::cast<llvm::BasicBlock>(map[definition->getParent()]),
        map[definition]);
    for (auto *use : uses) updater.RewriteUse(*use);
    stats.phisInserted += phis.size();
  }
  stats.blocksVisited += clones.size() + (entry != nullptr);
  stats.phisInserted += inputs.size();
  ++stats.loadsForwarded;
  stats.changed = true;
}

bool prependHub(Corridor &corridor, llvm::BasicBlock &hub,
                  llvm::DominatorTree &dom, bool &exhausted) {
  // Keep the original head multiply-entered after redirecting one edge. This
  // also prevents leaving a zero-incoming original PHI behind.
  if (llvm::pred_size(&hub) < 2 || corridor.members.contains(&hub) ||
      !dom.isReachableFromEntry(&hub) || !safeCorridorBlock(hub))
    return false;
  if (llvm::pred_size(&hub) > MaxIncomingEdges) {
    exhausted = true;
    return false;
  }
  std::size_t instructions = hub.size();
  for (auto *block : corridor.blocks) instructions += block->size();
  if (corridor.blocks.size() == MaxCorridorBlocks ||
      instructions > MaxCorridorInstructions) {
    exhausted = true;
    return false;
  }
  // Only the selected forward edge enters the next copy. A branch to an
  // earlier member would require a fresh original invocation, not a copy loop.
  for (auto *successor : llvm::successors(&hub))
    if (successor == &hub ||
        (corridor.members.contains(successor) && successor != corridor.blocks.front()))
      return false;
  for (auto *block : corridor.blocks)
    for (auto *successor : llvm::successors(block))
      if (successor == &hub) return false;
  corridor.blocks.insert(corridor.blocks.begin(), &hub);
  corridor.members.insert(&hub);
  corridor.expanded = true;
  return true;
}

bool forwardCorridor(
    llvm::LoadInst &load, const ByteAddress &loadedAddress,
    const Corridor &corridor,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats, CheckedMemoryEdgeBudget &budget,
    llvm::DominatorTree &dom, bool &exhausted, bool allowHubs) {
  auto *join = corridor.blocks.front();
  if (llvm::pred_size(join) > MaxIncomingEdges) {
    exhausted = true;
    return false;
  }
  std::vector<llvm::BasicBlock *> predecessors(llvm::pred_begin(join),
                                              llvm::pred_end(join));
  std::unordered_set<llvm::BasicBlock *> seen;
  std::vector<llvm::BasicBlock *> hubs;
  for (auto *pred : predecessors) {
    if (!seen.insert(pred).second) continue;
    unsigned successorIndex = 0;
    if (!selectedEdge(*pred, *join, successorIndex)) continue;
    if (++budget.edgeProofs > MaxEdgeProofs) {
      exhausted = true;
      budget.stopped = true;
      return false;
    }
    EntryInputs inputs;
    auto *store = proveStore(load, loadedAddress, corridor, *pred,
                             memoryBases, layout, dom, exhausted, budget, inputs);
    if (budget.stopped) return false;
    if (!store) {
      // Try all historical direct-store candidates before widening the
      // selected path. The hub itself must remain a read-only safe block.
      if (allowHubs && !corridor.members.contains(pred)) hubs.push_back(pred);
      continue;
    }
    const auto repair = preflightRepair(corridor, inputs, *load.getFunction(), exhausted);
    if (!repair) continue;
    std::size_t clonedInstructions = inputs.empty() ? 0 : inputs.size() + 1;
    for (auto *block : corridor.blocks) clonedInstructions += block->size();
    const auto snapshotBlocks = static_cast<std::size_t>(!inputs.empty());
    if (corridor.blocks.size() + snapshotBlocks > MaxCorridorBlocks ||
        clonedInstructions > MaxCorridorInstructions) {
      exhausted = true;
      continue;
    }
    if (1 + snapshotBlocks > MaxClones - budget.clones ||
        *repair > MaxRepairCells - budget.repairCells ||
        clonedInstructions > MaxClonedInstructions - budget.clonedInstructions ||
        stats.loadsForwarded >= options.maxRewrites) {
      exhausted = true;
      budget.stopped = true;
      return false;
    }
    cloneEdge(load, *store, corridor, *pred, successorIndex, inputs, stats);
    budget.clones += 1 + snapshotBlocks;
    budget.repairCells += *repair;
    budget.clonedInstructions += clonedInstructions;
    dom.recalculate(*load.getFunction());
    return true;
  }
  // Each recursive extension adds a distinct block and every selected edge
  // spends the same module proof budget as the existing direct-edge search.
  for (auto *hub : hubs) {
    if (budget.stopped) break;
    auto extended = corridor;
    if (prependHub(extended, *hub, dom, exhausted) &&
        forwardCorridor(load, loadedAddress, extended, memoryBases, layout,
                        options, stats, budget, dom, exhausted, true))
      return true;
  }
  return false;
}

} // namespace

void forwardCheckedMemoryEdges(
    llvm::Function &function,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats, CheckedMemoryEdgeBudget &budget,
    std::vector<llvm::LoadInst *> &pendingHubs) {
  if (!options.enableCrossBlock || !options.enablePhi ||
      stats.loadsForwarded >= options.maxRewrites || budget.stopped)
    return;
  std::vector<llvm::LoadInst *> candidates;
  bool full = false;
  for (auto &block : function) {
    for (auto &instruction : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
        if (plainLoad(*load) && byteAddress(load->getPointerOperand(),
                                          memoryBases, layout)) {
          candidates.push_back(load);
          if (candidates.size() > MaxCandidates - budget.candidates) {
            full = true;
            break;
          }
        }
    }
    if (full) break;
  }

  llvm::DominatorTree dom(function);
  bool exhausted = false;
  // Snapshot only original LOADs. New clones cannot recursively expand this
  // run, and at most one incoming edge is specialized for each original LOAD.
  for (auto *load : candidates) {
    if (++budget.candidates > MaxCandidates) {
      exhausted = true;
      budget.stopped = true;
      break;
    }
    const auto loadedAddress = byteAddress(load->getPointerOperand(),
                                           memoryBases, layout);
    if (!loadedAddress) continue;
    const auto corridor = findCorridor(*load, dom, exhausted);
    if (!corridor) continue;
    const bool rewritten = forwardCorridor(
        *load, *loadedAddress, *corridor, memoryBases, layout,
        options, stats, budget, dom, exhausted, false);
    // Keep only unrevised original LOADs. Later functions cannot erase them,
    // and an edge copy only removes its cloned LOAD, never this original.
    if (!rewritten && !budget.stopped) pendingHubs.push_back(load);
    if (budget.stopped || stats.loadsForwarded >= options.maxRewrites)
      break;
  }
  if (exhausted && !budget.reported) {
    stats.complete = false;
    ++stats.budgetExhausted;
    budget.reported = true;
  }
}

void forwardCheckedMemoryHubs(
    llvm::Function &function,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats, CheckedMemoryEdgeBudget &budget,
    const std::vector<llvm::LoadInst *> &pendingHubs) {
  if (!options.enableCrossBlock || !options.enablePhi ||
      stats.loadsForwarded >= options.maxRewrites || budget.stopped)
    return;
  llvm::DominatorTree dom(function);
  bool exhausted = false;
  for (auto *load : pendingHubs) {
    if (budget.stopped || stats.loadsForwarded >= options.maxRewrites) break;
    // Candidates were snapshotted and counted once by the direct phase.
    // Rebuild current proofs, but never rescan or count the newly copied IR.
    const auto loadedAddress = byteAddress(load->getPointerOperand(),
                                           memoryBases, layout);
    if (!loadedAddress) continue;
    const auto corridor = findCorridor(*load, dom, exhausted);
    if (!corridor) continue;
    forwardCorridor(*load, *loadedAddress, *corridor, memoryBases, layout,
                    options, stats, budget, dom, exhausted, true);
  }
  if (exhausted && !budget.reported) {
    stats.complete = false;
    ++stats.budgetExhausted;
    budget.reported = true;
  }
}

void forwardCheckedMemoryEdges(
    llvm::Function &function,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats, CheckedMemoryEdgeBudget &budget) {
  // Preserve the existing private per-function entry point. The module pass
  // uses the queued overload so every legacy direct path gets priority first.
  std::vector<llvm::LoadInst *> pendingHubs;
  forwardCheckedMemoryEdges(function, memoryBases, layout, options, stats,
                             budget, pendingHubs);
  forwardCheckedMemoryHubs(function, memoryBases, layout, options, stats,
                           budget, pendingHubs);
}

} // namespace dragon::llvmlifter::detail
