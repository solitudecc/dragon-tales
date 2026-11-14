#include "LLVMGuestCellSSA.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {

struct Node;
struct Edge {
  llvm::BasicBlock *predecessor;
  unsigned successorSlot;
  Node *definition = nullptr;
};
struct Node {
  llvm::BasicBlock *block;
  llvm::Value *value = nullptr;
  std::vector<Edge> incoming;
  bool active = true;
};

class Planner {
  llvm::Function &function;
  llvm::Value &base;
  llvm::LoadInst &load;
  const llvm::DataLayout &layout;
  GuestCellSSAOptions limits;
  GuestCellSSAStats stats;
  std::uint64_t cellOffset = 0;
  unsigned cellBytes = 0;
  std::unordered_map<llvm::BasicBlock *, std::vector<Edge>> predecessors;
  std::unordered_map<llvm::BasicBlock *, Node *> ends;
  std::unordered_set<llvm::BasicBlock *> visitedBlocks;
  std::vector<std::unique_ptr<Node>> nodes;

  bool refuse(const char *message, bool exhausted = false) {
    if (!stats.refused) stats.message = message;
    stats.refused = true;
    stats.budgetExhausted |= exhausted;
    return false;
  }
  bool charge(std::size_t amount = 1) {
    if (stats.refused) return false;
    if (amount > limits.maxWork - stats.work)
      return refuse("work limit", true);
    stats.work += amount;
    return true;
  }
  static unsigned integerBytes(llvm::Type *type) {
    if (!type->isIntegerTy()) return 0;
    const unsigned width = type->getIntegerBitWidth();
    return width == 8 || width == 16 || width == 32 || width == 64 ||
                   width == 128 ? width / 8 : 0;
  }

  std::optional<std::uint64_t> offset(llvm::Value *pointer) {
    std::uint64_t result = 0;
    std::unordered_set<llvm::Value *> seen;
    while (pointer != &base) {
      if (!charge() || !seen.insert(pointer).second) return std::nullopt;
      auto *gep = llvm::dyn_cast<llvm::GEPOperator>(pointer);
      if (!gep || gep->getPointerAddressSpace() !=
                      base.getType()->getPointerAddressSpace())
        return std::nullopt;
      // Charge each explicit index before LLVM traverses the GEP indices.
      if (!charge(gep->getNumIndices())) return std::nullopt;
      llvm::APInt delta(64, 0);
      if (!gep->accumulateConstantOffset(layout, delta)) return std::nullopt;
      result += delta.getZExtValue(); // Unsigned addition is modular i64.
      pointer = gep->getPointerOperand();
    }
    return result;
  }
  bool overlap(std::uint64_t other, unsigned bytes) {
    // Cells are at most 16 bytes. Explicit modular bytes avoid overflow-prone
    // interval endpoints, including a cell crossing offset UINT64_MAX.
    for (unsigned a = 0; a < cellBytes; ++a)
      for (unsigned b = 0; b < bytes; ++b) {
        if (!charge()) return true;
        if (cellOffset + a == other + b) return true;
      }
    return false;
  }

  Node *scan(llvm::BasicBlock &block, llvm::Instruction *before = nullptr) {
    // LOAD-prefix and this same block's end are deliberately separate nodes.
    if (!before) {
      if (auto found = ends.find(&block); found != ends.end()) {
        if (found->second->active) refuse("store-free definition cycle");
        return stats.refused ? nullptr : found->second;
      }
    }
    if (!charge()) return nullptr;
    if (nodes.size() == limits.maxNodes) {
      refuse("node limit", true); return nullptr;
    }
    if (!visitedBlocks.contains(&block)) {
      if (visitedBlocks.size() == limits.maxBlocks) {
        refuse("block limit", true); return nullptr;
      }
      visitedBlocks.insert(&block);
    }
    auto owned = std::make_unique<Node>();
    owned->block = &block;
    Node *node = owned.get();
    nodes.push_back(std::move(owned));
    stats.nodes = nodes.size();
    stats.blocks = visitedBlocks.size();
    if (!before) ends.emplace(&block, node);
    auto cursor = before ? before->getIterator() : block.end();
    while (cursor != block.begin()) {
      llvm::Instruction &instruction = *--cursor;
      if (!charge()) return nullptr;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!store->isSimple()) { refuse("non-simple STORE"); return nullptr; }
        const unsigned bytes = integerBytes(store->getValueOperand()->getType());
        auto where = offset(store->getPointerOperand());
        if (!bytes || !where) { refuse("unknown STORE cell"); return nullptr; }
        if (*where == cellOffset && bytes == cellBytes) {
          llvm::Value *value = store->getValueOperand();
          if (llvm::isa<llvm::UndefValue>(value) ||
              llvm::isa<llvm::PoisonValue>(value)) {
            refuse("explicit undef/poison STORE value"); return nullptr;
          }
          node->value = value;
          node->active = false;
          return node; // Earlier effects cannot change this last definition.
        }
        if (overlap(*where, bytes)) {
          refuse("overlapping STORE"); return nullptr;
        }
        continue;
      }
      if (llvm::isa<llvm::CallBase>(instruction) ||
          llvm::isa<llvm::FenceInst>(instruction) || instruction.isEHPad() ||
          instruction.mayWriteToMemory()) {
        refuse("call/write/fence barrier"); return nullptr;
      }
      if (auto *read = llvm::dyn_cast<llvm::LoadInst>(&instruction);
          read && !read->isSimple()) {
        refuse("non-simple LOAD barrier"); return nullptr;
      }
    }
    auto found = predecessors.find(&block);
    if (found == predecessors.end() || found->second.empty()) {
      refuse("entry without a STORE definition"); return nullptr;
    }
    node->incoming = found->second;
    for (auto &edge : node->incoming) {
      if (!charge()) return nullptr;
      edge.definition = scan(*edge.predecessor);
      if (!edge.definition) return nullptr;
    }
    node->active = false;
    return node;
  }

public:
  Planner(llvm::Function &function, llvm::Value &base, llvm::LoadInst &load,
          const GuestCellSSAOptions &options)
      : function(function), base(base), load(load),
        layout(function.getParent()->getDataLayout()), limits(options) {
    limits.maxBlocks = std::min(limits.maxBlocks, std::size_t{128});
    limits.maxNodes = std::min(limits.maxNodes, std::size_t{256});
    limits.maxPhis = std::min(limits.maxPhis, std::size_t{128});
    limits.maxWork = std::min(limits.maxWork, std::size_t{16384});
  }

  GuestCellSSAStats run() {
    cellBytes = integerBytes(load.getType());
    if (load.getFunction() != &function || !load.isSimple() || !cellBytes ||
        !base.getType()->isPointerTy()) {
      refuse("unsupported LOAD/base"); return stats;
    }
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(&base);
        instruction && instruction->getFunction() != &function) {
      refuse("foreign Guest base"); return stats;
    }
    if (auto *argument = llvm::dyn_cast<llvm::Argument>(&base);
        argument && argument->getParent() != &function) {
      refuse("foreign Guest base"); return stats;
    }
    unsigned addressSpace = base.getType()->getPointerAddressSpace();
    if (layout.getPointerSizeInBits(addressSpace) != 64 ||
        layout.getIndexSizeInBits(addressSpace) != 64 ||
        layout.isNonIntegralPointerType(base.getType())) {
      refuse("unsupported pointer/index layout"); return stats;
    }
    auto where = offset(load.getPointerOperand());
    if (!where) { refuse("nonconstant Guest LOAD cell"); return stats; }
    cellOffset = *where;
    // Inventory every actual successor slot in stable original IR order. This
    // neither deduplicates duplicate edges nor omits unreachable predecessors.
    for (auto &block : function) {
      if (!charge()) return stats;
      llvm::Instruction *terminator = block.getTerminator();
      if (!terminator) { refuse("missing terminator"); return stats; }
      for (unsigned slot = 0; slot < terminator->getNumSuccessors(); ++slot) {
        if (!charge()) return stats;
        predecessors[terminator->getSuccessor(slot)].push_back({&block, slot});
      }
    }
    Node *root = scan(*load.getParent(), &load);
    if (!root) return stats;

    llvm::DominatorTree dominance(function);
    // Validate/reserve the entire plan before creating the first instruction.
    // A planned definition is a block-entry PHI; an original leaf must dominate
    // its actual incoming edge. These tests do not recursively evaluate values.
    auto dominates = [&](Node *definition, llvm::Instruction *use) {
      if (definition->value) return dominance.dominates(definition->value, use);
      return dominance.dominates(definition->block, use->getParent());
    };
    if (root->value && !dominates(root, &load)) {
      refuse("root definition does not dominate LOAD"); return stats;
    }
    for (auto &node : nodes) {
      if (!charge()) return stats;
      if (node->value) continue;
      if (stats.phis == limits.maxPhis) {
        refuse("PHI limit", true); return stats;
      }
      ++stats.phis; // Prepaid; no IR exists yet, including on late refusal.
      for (auto &edge : node->incoming) {
        if (!charge()) return stats;
        auto *terminator = edge.predecessor->getTerminator();
        if (terminator->getSuccessor(edge.successorSlot) != node->block ||
            !dominates(edge.definition, terminator)) {
          refuse("incoming definition does not dominate edge"); return stats;
        }
      }
    }
    std::unordered_map<llvm::BasicBlock *, std::vector<Node *>> byBlock;
    for (auto &node : nodes) {
      if (!charge()) return stats;
      byBlock[node->block].push_back(node.get());
    }
    std::vector<Node *> ordered;
    for (auto &block : function) {
      if (!charge()) return stats;
      if (auto found = byBlock.find(&block); found != byBlock.end())
        for (Node *node : found->second) {
          // Prepay commit allocation, incoming connections, and value lookup.
          // Nothing below the commit boundary can exhaust a budget.
          if (!charge(2 + node->incoming.size())) return stats;
          ordered.push_back(node);
        }
    }
    if (!charge(2)) return stats; // Replacement and erasure.
    // The plan is closed. All PHIs are allocated in function/block order before
    // any incoming is connected. No post-mutation refusal path is possible.
    std::unordered_map<Node *, llvm::Value *> values;
    for (auto &node : nodes)
      if (node->value) values.emplace(node.get(), node->value);
    for (Node *node : ordered)
      if (!node->value)
        values.emplace(node, llvm::PHINode::Create(
            load.getType(), node->incoming.size(), "guest.cell.ssa",
            &*node->block->begin()));
    for (auto &node : nodes) {
      if (node->value) continue;
      auto *phi = llvm::cast<llvm::PHINode>(values.at(node.get()));
      for (auto &edge : node->incoming)
        phi->addIncoming(values.at(edge.definition), edge.predecessor);
    }
    load.replaceAllUsesWith(values.at(root));
    load.eraseFromParent();
    stats.changed = true;
    stats.message = "nearest STORE definitions cover every predecessor slot";
    return stats;
  }
};

} // namespace

GuestCellSSAStats forwardGuestCellLoad(
    llvm::Function &function, llvm::Value &base, llvm::LoadInst &load,
    const GuestCellSSAOptions &options) {
  if (!function.getParent()) {
    GuestCellSSAStats stats;
    stats.refused = true;
    stats.message = "Function has no Module";
    return stats;
  }
  return Planner(function, base, load, options).run();
}

} // namespace dragon::llvmlifter::detail
