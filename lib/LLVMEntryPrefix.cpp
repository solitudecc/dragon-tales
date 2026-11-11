#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
namespace {
struct Edge {
  BasicBlock *from = nullptr;
  unsigned successor = 0;
  BasicBlock *target = nullptr;
  bool internal = false;
  bool backedge = false;
  bool terminalLeaf = false;
};
struct DirectExitRepair {
  Instruction *definition = nullptr;
  BasicBlock *exit = nullptr;
  std::vector<Use *> uses;
  std::vector<unsigned> successorSlots;
};
constexpr std::size_t MaxExitRepairValues = 64;
constexpr std::size_t MaxExitRepairUses = 512;
constexpr std::size_t MaxExitRepairInputs = 512;
struct Plan {
  Function *function = nullptr;
  BasicBlock *seed = nullptr, *root = nullptr;
  std::vector<BasicBlock *> blocks;
  std::set<BasicBlock *> members;
  std::map<PHINode *, Value *> rootInputs;
  std::vector<DirectExitRepair> exitRepairs;
  std::map<std::pair<BasicBlock *, unsigned>, Edge> edges;
  bool internalIncoming(BasicBlock *from, BasicBlock *target) const {
    for (unsigned i = 0; i < from->getTerminator()->getNumSuccessors(); ++i) {
      const auto found = edges.find({from, i});
      if (found != edges.end() && found->second.target == target)
        return found->second.internal;
    }
    return false;
  }
  size_t instructions = 0;
  bool truncated = false;
};
bool fail(std::string &error, const Twine &message) {
  error = message.str();
  return false;
}
bool boundary(BasicBlock &block) {
  for (auto &instruction : block)
    if (isa<CallBase>(instruction)) return true;
  return false;
}
// A terminal leaf has no continuation, and every original incoming edge is
// from this same switch block. Thus its fresh effects can be cloned with the
// selector without general live-out repair. Names play no role in admission.
bool terminalExitLeaf(BasicBlock &source, BasicBlock &target) {
  if (!isa<SwitchInst>(source.getTerminator()) ||
      !isa<ReturnInst>(target.getTerminator()) || target.hasAddressTaken() ||
      target.isEHPad()) return false;
  bool hasIncoming = false;
  for (auto *pred : predecessors(&target)) {
    if (pred != &source) return false;
    hasIncoming = true;
  }
  if (!hasIncoming) return false;
  for (auto &instruction : target)
    if (isa<CallBase, AllocaInst>(instruction) || instruction.getType()->isTokenTy())
      return false;
  return true;
}
bool prepare(Function &function, size_t maxBlocks, size_t maxInstructions,
             Plan &plan, std::string &error) {
  plan.function = &function;
  if (function.isDeclaration() || function.hasPersonalityFn())
    return fail(error, "declaration or EH function unsupported");
  for (auto &block : function)
    if (block.getName() == "guarded.accept") plan.seed = &block;
  if (!plan.seed) return fail(error, "missing guarded.accept edge");
  auto *seedBranch = dyn_cast<BranchInst>(plan.seed->getTerminator());
  if (!seedBranch || !seedBranch->isUnconditional())
    return fail(error, "guarded.accept must have one unconditional successor");
  plan.root = seedBranch->getSuccessor(0);
  if (plan.root == plan.seed) return fail(error, "self seed");
  DominatorTree dom(function);
  std::vector<BasicBlock *> pending{plan.root};
  std::set<BasicBlock *> seen;
  for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
    auto *block = pending[cursor];
    if (!seen.insert(block).second) continue;
    if (block == plan.seed || block == &function.getEntryBlock())
      return fail(error, "prefix returns to guard or external function entry");
    // This first prototype does not repair old-execution non-PHI values from
    // a region that already ran before the seed. Otherwise a terminal reentry
    // could legally read stale original SSA and still pass the verifier.
    if (dom.dominates(block, plan.seed))
      return fail(error, "historical region dominates seed; unsupported live-out repair");
    if (boundary(*block)) continue; // Execute calls in their original block.
    if (block->hasAddressTaken() || block->isEHPad())
      return fail(error, "address-taken or EH prefix block");
    auto *term = block->getTerminator();
    if (!isa<BranchInst, SwitchInst, ReturnInst>(term))
      return fail(error, "unsupported prefix terminator");
    for (auto &instruction : *block)
      if (isa<AllocaInst>(instruction) || instruction.getType()->isTokenTy())
        return fail(error, "prefix alloca or token unsupported");
    if (plan.blocks.size() == maxBlocks ||
        block->size() > maxInstructions - plan.instructions) {
      plan.truncated = true;
      continue; // Original boundary must pass the live-out check below.
    }
    plan.blocks.push_back(block);
    plan.members.insert(block);
    plan.instructions += block->size();
    if (auto *branch = dyn_cast<BranchInst>(term)) {
      for (unsigned i = 0; i < branch->getNumSuccessors(); ++i)
        pending.push_back(branch->getSuccessor(i));
    } else if (auto *dispatch = dyn_cast<SwitchInst>(term)) {
      for (unsigned i = 0; i < dispatch->getNumSuccessors(); ++i)
        if (terminalExitLeaf(*block, *dispatch->getSuccessor(i)))
          pending.push_back(dispatch->getSuccessor(i));
    }
    // Other switch targets and all call blocks remain original boundaries.
  }
  if (!plan.members.contains(plan.root))
    return fail(error, "root is a call boundary or exceeds budget");
  // Freeze identity before any mutation. Both branch and switch edges are
  // represented, including repeated successor slots. Branch and admitted
  // terminal-leaf switch edges may be internal; DFS-gray targets stay original.
  for (auto *block : plan.blocks) {
    auto *term = block->getTerminator();
    for (unsigned i = 0; i < term->getNumSuccessors(); ++i)
      plan.edges.emplace(std::make_pair(block, i),
                         Edge{block, i, term->getSuccessor(i), false, false, false});
  }
  std::map<BasicBlock *, unsigned> colors;
  std::function<void(BasicBlock *)> visit = [&](BasicBlock *block) {
    colors[block] = 1;
    auto *term = block->getTerminator();
    if (isa<BranchInst>(term)) {
      for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
        auto &edge = plan.edges.at({block, i});
        if (!plan.members.contains(edge.target)) continue;
        if (colors[edge.target] == 1) {
          edge.backedge = true;
          continue;
        }
        edge.internal = true;
        if (colors[edge.target] == 0) visit(edge.target);
      }
    } else if (isa<SwitchInst>(term)) {
      for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
        auto &edge = plan.edges.at({block, i});
        if (!plan.members.contains(edge.target) ||
            !terminalExitLeaf(*block, *edge.target)) continue;
        edge.internal = true;
        edge.terminalLeaf = true;
        if (colors[edge.target] == 0) visit(edge.target);
      }
    }
    colors[block] = 2;
  };
  visit(plan.root);
  for (auto *block : plan.blocks) {
    if (colors[block] != 2)
      return fail(error, "selected block unreachable after bounded DFS");
    // LLVM PHIs identify predecessors, not successor indices. Duplicate edges
    // must share policy so their verified equal incoming values remain paired.
    std::map<BasicBlock *, bool> policy;
    for (unsigned i = 0; i < block->getTerminator()->getNumSuccessors(); ++i) {
      const auto &edge = plan.edges.at({block, i});
      const auto [it, inserted] = policy.emplace(edge.target, edge.internal);
      if (!inserted && it->second != edge.internal)
        return fail(error, "duplicate successor edges have differing policies");
    }
  }
  for (auto &phi : plan.root->phis()) {
    int index = phi.getBasicBlockIndex(plan.seed);
    if (index < 0 || phi.getNumIncomingValues() < 2)
      return fail(error, "root PHI lacks seed or would become empty");
    auto *input = phi.getIncomingValue(index);
    if (auto *instruction = dyn_cast<Instruction>(input))
      if (plan.members.contains(instruction->getParent()) ||
          !dom.dominates(instruction, plan.seed->getTerminator()))
        return fail(error, "root historical PHI input not external dominating SSA");
    plan.rootInputs[&phi] = input;
  }
  std::size_t repairUses = 0, repairInputs = 0;
  for (auto *block : plan.blocks) {
    for (auto &instruction : *block) {
      auto *phi = dyn_cast<PHINode>(&instruction);
      if (phi && block != plan.root) {
        unsigned retained = 0;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
          if (plan.members.contains(phi->getIncomingBlock(i)) &&
              plan.internalIncoming(phi->getIncomingBlock(i), block)) ++retained;
        if (!retained) return fail(error, "internal PHI has no cloned incoming");
      }
      // Only operands executed in the clone need an external dominance proof.
      for (unsigned i = 0; i < instruction.getNumOperands(); ++i) {
        if (phi) {
          if (block == plan.root) continue;
          auto *from = phi->getIncomingBlock(i);
          if (!plan.members.contains(from) || !plan.internalIncoming(from, block)) continue;
        }
        auto *operand = dyn_cast<Instruction>(instruction.getOperand(i));
        if (operand && !plan.members.contains(operand->getParent()) &&
            !dom.dominates(operand, plan.seed->getTerminator()))
          return fail(error, "external SSA operand does not dominate seed");
      }
      DirectExitRepair repair{&instruction, nullptr, {}, {}};
      for (auto &use : instruction.uses()) {
        auto *user = dyn_cast<Instruction>(use.getUser());
        if (!user) return fail(error, "non-instruction SSA user");
        if (plan.members.contains(user->getParent())) continue;
        if (auto *outsidePhi = dyn_cast<PHINode>(user)) {
          if (!plan.members.contains(outsidePhi->getIncomingBlock(use.getOperandNo())))
            return fail(error, "unsupported external PHI live-out");
          continue; // Existing original-edge PHI repair remains unchanged.
        }
        auto *exit = user->getParent();
        // Unique predecessor BLOCK, not edge: repeated switch/branch successor
        // slots are handled individually below. No nonlocal dominance guess.
        if (exit->getUniquePredecessor() != block ||
            (repair.exit && repair.exit != exit))
          return fail(error, "non-PHI live-out is not confined to one direct unique-predecessor exit");
        auto *type = instruction.getType();
        if (!type->isIntegerTy() && !type->isFloatingPointTy() &&
            !type->isPointerTy() && !type->isVectorTy())
          return fail(error, "unsupported direct-exit SSA type");
        repair.exit = exit;
        if (++repairUses > MaxExitRepairUses) {
          plan.truncated = true;
          return fail(error, "direct-exit ordinary-use budget exhausted");
        }
        repair.uses.push_back(&use);
      }
      if (!repair.uses.empty()) {
        if (plan.exitRepairs.size() == MaxExitRepairValues) {
          plan.truncated = true;
          return fail(error, "direct-exit value budget exhausted");
        }
        auto *term = block->getTerminator();
        for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
          const auto &edge = plan.edges.at({block, i});
          if (edge.target != repair.exit) continue;
          if (edge.internal) return fail(error, "direct-exit repair crosses an internal edge");
          repair.successorSlots.push_back(i);
        }
        if (repair.successorSlots.empty()) return fail(error, "missing direct-exit edge");
        if (repair.successorSlots.size() > (MaxExitRepairInputs - repairInputs) / 2) {
          plan.truncated = true;
          return fail(error, "direct-exit PHI incoming budget exhausted");
        }
        repairInputs += repair.successorSlots.size() * 2;
        plan.exitRepairs.push_back(std::move(repair));
      }
    }
  }
  // New bridges consume the same instruction budget as copied instructions.
  // Refuse before mutation instead of silently allocating extra repair space.
  if (plan.exitRepairs.size() > maxInstructions - plan.instructions) {
    plan.truncated = true;
    return fail(error, "direct-exit PHI instruction budget exhausted");
  }
  plan.instructions += plan.exitRepairs.size();
  return true;
}
void apply(Plan &plan) {
  ValueToValueMapTy values;
  std::map<BasicBlock *, BasicBlock *> copies;
  for (auto *block : plan.blocks) {
    auto *copy = CloneBasicBlock(block, values, ".entry.prefix", plan.function);
    values[block] = copy;
    copies[block] = copy;
  }
  // Identity-map all external locals explicitly. Missing MapValue entries must
  // never manufacture null operands or successor PHI values.
  for (auto &argument : plan.function->args()) values[&argument] = &argument;
  for (auto &block : *plan.function) {
    if (plan.members.contains(&block)) continue;
    // Newly cloned blocks already contain mapped instructions; do not overwrite.
    bool isCopy = false;
    for (auto &[original, copy] : copies) if (copy == &block) isCopy = true;
    if (isCopy) continue;
    values[&block] = &block;
    for (auto &instruction : block) values[&instruction] = &instruction;
  }
  // Selected root values belong to execution before the seed, never fresh copies.
  for (auto &[phi, input] : plan.rootInputs) values[phi] = input;
  for (auto *block : plan.blocks) {
    auto *copy = copies.at(block);
    auto originalPhi = block->phis().begin();
    for (auto iterator = copy->begin(); iterator != copy->end() &&
         isa<PHINode>(&*iterator);) {
      auto *phi = cast<PHINode>(&*iterator++);
      auto *source = &*originalPhi++;
      if (block == plan.root) {
        phi->replaceAllUsesWith(plan.rootInputs.at(source));
        phi->eraseFromParent();
        continue;
      }
      for (unsigned i = phi->getNumIncomingValues(); i-- > 0;) {
        auto *from = phi->getIncomingBlock(i);
        if (!plan.internalIncoming(from, block)) phi->removeIncomingValue(i, false);
      }
    }
    for (auto &instruction : *copy)
      RemapInstruction(&instruction, values, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
    // Do not infer successor policy from block membership: the same original
    // block can have a fresh copy and remain the target of a reentry edge.
    auto *term = block->getTerminator();
    for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
      const auto &edge = plan.edges.at({block, i});
      copy->getTerminator()->setSuccessor(
          i, edge.internal ? copies.at(edge.target) : edge.target);
    }
  }
  // Add one PHI entry per actual original exit edge, including duplicate slots.
  // The source value is mapped from this execution; a missing map means an
  // external original SSA value, never null. Original PHIs are otherwise kept.
  for (const auto &[identity, edge] : plan.edges) {
    if (edge.internal) continue;
    auto *copy = copies.at(edge.from);
    for (auto &phi : edge.target->phis()) {
      auto *input = phi.getIncomingValueForBlock(edge.from);
      auto found = values.find(input);
      auto *mapped = found == values.end() ? input : static_cast<Value *>(found->second);
      phi.addIncoming(mapped, copy);
    }
  }
  cast<BranchInst>(plan.seed->getTerminator())->setSuccessor(0, copies.at(plan.root));
  // Keep original blocks and all non-seed execution paths intact.
  for (auto &phi : plan.root->phis()) phi.removeIncomingValue(plan.seed, false);
  // Only direct original exits are repaired. The old edge carries the original
  // execution's definition; each cloned edge carries its fresh mapped value.
  // Calls and volatile effects stay in the original exit after these PHIs.
  for (const auto &repair : plan.exitRepairs) {
    auto *origin = repair.definition->getParent();
    auto found = values.find(repair.definition);
    auto *fresh = found == values.end() ? repair.definition : static_cast<Value *>(found->second);
    auto *bridge = PHINode::Create(repair.definition->getType(),
        repair.successorSlots.size() * 2, "entry.prefix.live", &repair.exit->front());
    for (const auto slot : repair.successorSlots) {
      (void)slot; // Equal values for every original/cloned successor slot.
      bridge->addIncoming(repair.definition, origin);
      bridge->addIncoming(fresh, copies.at(origin));
    }
    for (auto *use : repair.uses) use->set(bridge);
  }
}
}

namespace dragon::llvmlifter {
namespace {
constexpr std::size_t MaxTransactionInstructions = 1000000;
constexpr std::size_t MaxTransactionBlocks = 65536;
bool supportedIdentity(llvm::Function &function, bool &budget, std::string &error) {
  if (function.hasPersonalityFn() || function.getSubprogram())
    return fail(error, "EH or debug function identity unsupported");
  std::size_t instructions = 0;
  if (function.size() > MaxTransactionBlocks) {
    budget = true;
    return fail(error, "transaction block budget exhausted");
  }
  for (auto &block : function) {
    if (block.hasAddressTaken() || block.isEHPad())
      return fail(error, "address-taken or EH function block unsupported");
    if (block.size() > MaxTransactionInstructions - instructions) {
      budget = true;
      return fail(error, "transaction instruction budget exhausted");
    }
    instructions += block.size();
    for (auto &instruction : block)
      if (instruction.getDebugLoc() || instruction.hasDbgRecords())
        return fail(error, "debug instruction identity unsupported");
  }
  return true;
}
llvm::Function *makeTrial(llvm::Function &source) {
  auto *trial = llvm::Function::Create(source.getFunctionType(),
      llvm::GlobalValue::InternalLinkage, source.getAddressSpace(),
      source.getName() + ".entry.prefix.trial", source.getParent());
  trial->copyAttributesFrom(&source);
  llvm::ValueToValueMapTy map;
  auto argument = trial->arg_begin();
  for (auto &old : source.args()) {
    argument->setName(old.getName());
    map[&old] = &*argument++;
  }
  // Do not map source -> trial: recursive/global references remain source.
  llvm::SmallVector<llvm::ReturnInst *, 8> returns;
  llvm::CloneFunctionInto(trial, &source, map,
      llvm::CloneFunctionChangeType::LocalChangesOnly, returns);
  // CloneFunctionInto may copy attributes too. Normalize only the temporary
  // global identity after cloning; the original function is never modified.
  trial->setLinkage(llvm::GlobalValue::InternalLinkage);
  trial->setVisibility(llvm::GlobalValue::DefaultVisibility);
  trial->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  trial->setComdat(nullptr);
  return trial;
}
bool verify(llvm::Function &function, std::string &error) {
  llvm::raw_string_ostream output(error);
  return !llvm::verifyFunction(function, &output);
}
bool commit(llvm::Function &source, llvm::Function &trial, std::string &error) {
  // Move, never delete, the old body until the committed candidate verifies.
  // No source attributes, argument objects, Function or Module are replaced.
  auto *backup = llvm::Function::Create(source.getFunctionType(),
      llvm::GlobalValue::InternalLinkage, source.getAddressSpace(),
      source.getName() + ".entry.prefix.rollback", source.getParent());
  backup->splice(backup->end(), &source);
  auto oldArgument = source.arg_begin();
  for (auto &argument : trial.args()) argument.replaceAllUsesWith(&*oldArgument++);
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
}
EntryPrefixStats LLVMLifter::runEntryPrefix(const EntryPrefixOptions &options) {
  EntryPrefixStats stats;
  if (mode_ != ExecutionMode::StaticAnalysis || module().empty()) return stats;
  stats.available = true;
  if (options.maxBlocks > 4096 || options.maxInstructions > 65536) {
    stats.complete = false;
    stats.budgetExhausted = 1;
    stats.message = "entry-prefix options exceed hard limits 4096/65536";
    return stats;
  }
  std::vector<llvm::Function *> candidates;
  for (auto &function : module())
    for (auto &block : function)
      if (block.getName() == "guarded.accept") {
        candidates.push_back(&function);
        break;
      }
  stats.candidateEdges = candidates.size();
  if (candidates.empty()) return stats;
  std::string inputError;
  llvm::raw_string_ostream inputOutput(inputError);
  if (llvm::verifyModule(module(), &inputOutput)) {
    stats.complete = false;
    stats.rejected = candidates.size();
    stats.message = "invalid entry-prefix input module: " + inputError;
    return stats;
  }
  for (auto *function : candidates) {
    llvm::BasicBlock *seed = nullptr;
    for (auto &block : *function)
      if (block.getName() == "guarded.accept") seed = &block;
    auto *branch = llvm::dyn_cast_or_null<llvm::BranchInst>(seed->getTerminator());
    if (branch && branch->isUnconditional() &&
        branch->getSuccessor(0)->getSinglePredecessor() == seed) {
      ++stats.skippedSingleEntry;
      continue;
    }
    bool budget = false;
    std::string error;
    if (!supportedIdentity(*function, budget, error) || !verify(*function, error)) {
      ++stats.rejected;
      stats.complete = false;
      stats.message = error;
      if (budget) { ++stats.budgetExhausted; stats.complete = false; }
      continue;
    }
    if (stats.clonedBlocks >= options.maxBlocks ||
        stats.clonedInstructions >= options.maxInstructions) {
      ++stats.rejected;
      ++stats.budgetExhausted;
      stats.complete = false;
      stats.message = "entry-prefix copy budget exhausted";
      continue;
    }
    auto *trial = makeTrial(*function);
    Plan plan;
    const bool admitted = prepare(*trial, options.maxBlocks - stats.clonedBlocks,
        options.maxInstructions - stats.clonedInstructions, plan, error);
    if (plan.truncated) { ++stats.budgetExhausted; stats.complete = false; }
    if (!admitted) {
      ++stats.rejected;
      stats.complete = false;
      stats.message = error;
      trial->eraseFromParent();
      continue;
    }
    apply(plan);
    if (!verify(*trial, error) || !commit(*function, *trial, error)) {
      ++stats.rejected;
      stats.complete = false;
      stats.message = "entry-prefix transaction verification failed: " + error;
      trial->eraseFromParent();
      continue;
    }
    ++stats.peeledEdges;
    stats.changed = true;
    stats.clonedBlocks += plan.blocks.size();
    stats.clonedInstructions += plan.instructions;
    for (const auto &[identity, edge] : plan.edges) {
      stats.originalExits += !edge.internal;
      stats.internalEdges += edge.internal;
      stats.backedges += edge.backedge;
      stats.terminalEdges += edge.terminalLeaf;
    }
    trial->eraseFromParent();
  }
  return stats;
}
}
