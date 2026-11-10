#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <dragon/lifters/llvm/Execution.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/ValueTracking.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <deque>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {

struct AddressKey {
  llvm::Value *base = nullptr;
  llvm::Value *dynamic = nullptr;
  std::uint64_t offset = 0;
  bool constant = false;
};

struct Access {
  llvm::Instruction *instruction = nullptr;
  AddressKey address;
  unsigned bytes = 0;
  bool store = false;
  llvm::Value *value = nullptr;
};

struct Fact {
  Access access;
  llvm::Value *value = nullptr;
};

llvm::Value *stripPointerCasts(llvm::Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

std::optional<AddressKey> addressKey(const llvm::Value *pointer,
                                     const std::unordered_set<llvm::Value *> &memoryBases) {
  auto *stripped = stripPointerCasts(const_cast<llvm::Value *>(pointer));
  auto *gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(stripped);
  if (!gep || gep->getNumIndices() != 1)
    return std::nullopt;
  auto *base = stripPointerCasts(gep->getPointerOperand());
  if (!memoryBases.contains(base))
    return std::nullopt;
  auto *index = gep->getOperand(gep->getNumOperands() - 1);
  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(index))
    return AddressKey{base, nullptr, constant->getZExtValue(), true};
  return AddressKey{base, index, 0, false};
}

unsigned accessBytes(const llvm::Type *type) {
  if (!type || !type->isIntegerTy())
    return 0;
  const unsigned bits = type->getIntegerBitWidth();
  if (!bits || bits > 128 || bits % 8)
    return 0;
  return bits / 8;
}

bool sameAddress(const AddressKey &left, const AddressKey &right) {
  if (left.base != right.base || left.constant != right.constant)
    return false;
  return left.constant ? left.offset == right.offset : left.dynamic == right.dynamic;
}

// Return true only when two concrete byte ranges are definitely disjoint.
// Unknown/dynamic addresses are never declared disjoint.
bool provablyDisjoint(const AddressKey &left, unsigned leftBytes,
                      const AddressKey &right, unsigned rightBytes) {
  if (left.base != right.base || !left.constant || !right.constant)
    return false;
  const auto delta = right.offset - left.offset;
  const auto signedDelta = delta <= std::numeric_limits<std::int64_t>::max()
      ? static_cast<std::int64_t>(delta)
      : -static_cast<std::int64_t>(~delta + 1);
  return signedDelta >= static_cast<std::int64_t>(leftBytes) ||
         signedDelta <= -static_cast<std::int64_t>(rightBytes);
}

bool mayOverlap(const AddressKey &left, unsigned leftBytes,
                const AddressKey &right, unsigned rightBytes) {
  return !provablyDisjoint(left, leftBytes, right, rightBytes);
}

bool equivalentValue(const llvm::Value *left, const llvm::Value *right) {
  if (left == right) return true;
  const auto *a = llvm::dyn_cast<llvm::ConstantInt>(left);
  const auto *b = llvm::dyn_cast<llvm::ConstantInt>(right);
  return a && b && a->getType() == b->getType() &&
         a->getValue() == b->getValue();
}

bool sameFact(const Fact &left, const Fact &right) {
  return left.access.bytes == right.access.bytes &&
         sameAddress(left.access.address, right.access.address) &&
         equivalentValue(left.value, right.value);
}

bool sameFacts(const std::vector<Fact> &left, const std::vector<Fact> &right) {
  if (left.size() != right.size()) return false;
  std::vector<bool> matched(right.size(), false);
  for (const auto &fact : left) {
    std::size_t match = right.size();
    for (std::size_t index = 0; index < right.size(); ++index)
      if (!matched[index] && sameFact(fact, right[index])) {
        match = index;
        break;
      }
    if (match == right.size()) return false;
    matched[match] = true;
  }
  return true;
}

bool isMemoryBaseLoad(const llvm::LoadInst &load, const llvm::Function &function) {
  if (!load.getType()->isPointerTy() || load.isVolatile() || load.isAtomic() ||
      function.arg_size() < 2)
    return false;
  std::int64_t offset = 0;
  const auto *base = llvm::GetPointerBaseWithConstantOffset(
      load.getPointerOperand(), offset, function.getParent()->getDataLayout());
  return base == function.getArg(1) && offset == 0;
}

bool isKnownLocalStore(const llvm::StoreInst &store,
                       const std::unordered_set<llvm::Value *> &memoryBases,
                       const llvm::DataLayout &layout) {
  if (addressKey(store.getPointerOperand(), memoryBases))
    return false;
  const auto *underlying = llvm::getUnderlyingObject(store.getPointerOperand(), 32);
  if (llvm::isa<llvm::AllocaInst>(underlying))
    return true;
  if (const auto *argument = llvm::dyn_cast<llvm::Argument>(underlying))
    return argument->getType()->isPointerTy();
  (void)layout;
  return false;
}

void invalidate(std::vector<Fact> &facts, const Access &store,
                MemoryForwardStats &stats) {
  facts.erase(std::remove_if(facts.begin(), facts.end(), [&](const Fact &fact) {
    if (provablyDisjoint(fact.access.address, fact.access.bytes,
                         store.address, store.bytes))
      return false;
    if (!sameAddress(fact.access.address, store.address))
      ++stats.partialOverlaps;
    return true;
  }), facts.end());
}

std::optional<Access> classifyLoad(llvm::LoadInst &load,
                                   const std::unordered_set<llvm::Value *> &memoryBases) {
  if (load.isVolatile() || load.isAtomic())
    return std::nullopt;
  auto address = addressKey(load.getPointerOperand(), memoryBases);
  const auto bytes = accessBytes(load.getType());
  if (!address || !bytes)
    return std::nullopt;
  return Access{&load, *address, bytes, false, &load};
}

std::optional<Access> classifyStore(llvm::StoreInst &store,
                                    const std::unordered_set<llvm::Value *> &memoryBases) {
  if (store.isVolatile() || store.isAtomic())
    return std::nullopt;
  auto address = addressKey(store.getPointerOperand(), memoryBases);
  const auto bytes = accessBytes(store.getValueOperand()->getType());
  if (!address || !bytes)
    return std::nullopt;
  return Access{&store, *address, bytes, true, store.getValueOperand()};
}

} // namespace

MemoryForwardStats runGuestMemorySSA(llvm::Module &module,
                                     const MemoryForwardOptions &options) {
  MemoryForwardStats stats;
  stats.available = true;
  if (!options.maxRewrites)
    return stats;

  for (auto &function : module) {
    if (function.isDeclaration() || function.arg_size() < 2)
      continue;

    std::unordered_set<llvm::Value *> memoryBases;
    for (const auto &block : function)
      for (const auto &instruction : block)
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
          if (isMemoryBaseLoad(*load, function))
            memoryBases.insert(const_cast<llvm::LoadInst *>(load));
    if (memoryBases.empty())
      continue;

    llvm::DominatorTree dom(function);
    std::unordered_set<llvm::BasicBlock *> reachable;
    std::deque<llvm::BasicBlock *> reachQueue{&function.getEntryBlock()};
    while (!reachQueue.empty()) {
      auto *block = reachQueue.front(); reachQueue.pop_front();
      if (!reachable.insert(block).second) continue;
      for (auto *successor : llvm::successors(block)) reachQueue.push_back(successor);
    }
    std::unordered_set<llvm::BasicBlock *> loopBlocks;
    std::map<llvm::BasicBlock *, std::unordered_set<llvm::BasicBlock *>> backedgePreds;
    for (auto *header : reachable) {
      for (auto *pred : llvm::predecessors(header)) {
        if (!reachable.contains(pred) || !dom.dominates(header, pred))
          continue;
        backedgePreds[header].insert(pred);
        std::deque<llvm::BasicBlock *> loopQueue{pred};
        loopBlocks.insert(header);
        while (!loopQueue.empty()) {
          auto *member = loopQueue.front();
          loopQueue.pop_front();
          if (!loopBlocks.insert(member).second)
            continue;
          for (auto *memberPred : llvm::predecessors(member)) {
            if (reachable.contains(memberPred) && memberPred != header)
              loopQueue.push_back(memberPred);
          }
        }
      }
    }
    std::map<llvm::BasicBlock *, std::vector<Fact>> incoming;
    std::map<llvm::BasicBlock *, std::map<llvm::BasicBlock *, std::vector<Fact>>> edgeFacts;
    std::map<llvm::BasicBlock *, std::size_t> pending;
    std::deque<llvm::BasicBlock *> work;
    incoming[&function.getEntryBlock()] = {};
    work.push_back(&function.getEntryBlock());
    std::unordered_set<llvm::BasicBlock *> visited;
    struct DeferredEdge {
      llvm::BasicBlock *pred = nullptr;
      llvm::BasicBlock *successor = nullptr;
      std::vector<Fact> facts;
    };
    std::vector<DeferredEdge> deferredEdges;
    struct PendingRewrite {
      llvm::LoadInst *load = nullptr;
      llvm::Value *value = nullptr;
      bool reused = false;
    };
    std::vector<PendingRewrite> pendingRewrites;
    std::size_t plannedRewrites = stats.loadsForwarded;
    for (auto *block : reachable) {
      if (block == &function.getEntryBlock()) continue;
      pending[block] = 0;
      for (auto *pred : llvm::predecessors(block))
        if (reachable.contains(pred) && !dom.dominates(block, pred))
          ++pending[block];
    }
    auto mergeIncoming = [&](llvm::BasicBlock *successor) {
      std::vector<std::pair<llvm::BasicBlock *, const std::vector<Fact> *>> edges;
      for (const auto &[pred, predFacts] : edgeFacts[successor])
        if (const auto backedges = backedgePreds.find(successor);
            backedges == backedgePreds.end() || !backedges->second.contains(pred))
          edges.emplace_back(pred, &predFacts);

      std::vector<Fact> merged;
      if (!edges.empty()) {
        const auto &first = *edges.front().second;
        for (const auto &candidate : first) {
          bool presentEverywhere = true;
          std::vector<llvm::Value *> values;
          for (const auto &[pred, predFacts] : edges) {
            (void)pred;
            auto it = std::find_if(predFacts->begin(), predFacts->end(),
                [&](const Fact &fact) {
                  return fact.access.bytes == candidate.access.bytes &&
                         sameAddress(fact.access.address, candidate.access.address);
                });
            if (it == predFacts->end()) { presentEverywhere = false; break; }
            values.push_back(it->value);
          }
          if (!presentEverywhere) { ++stats.joinBarriers; continue; }
          const bool equal = std::all_of(values.begin() + 1, values.end(),
              [&](llvm::Value *value) { return equivalentValue(value, values.front()); });
          llvm::Value *value = values.front();
          if (!equal && options.enablePhi) {
            llvm::IRBuilder<> builder(successor, successor->getFirstInsertionPt());
            auto *phi = builder.CreatePHI(value->getType(), values.size(), "guest.mem.phi");
            std::size_t index = 0;
            for (const auto &[pred, predFacts] : edges) {
              (void)predFacts;
              phi->addIncoming(values[index++], pred);
            }
            value = phi;
            ++stats.phisInserted;
          } else if (!equal) {
            ++stats.joinBarriers;
            continue;
          }
          auto fact = candidate;
          fact.value = value;
          merged.push_back(fact);
        }
      }
      incoming[successor] = std::move(merged);
      work.push_back(successor);
    };
    auto acceptEdge = [&](llvm::BasicBlock *pred, llvm::BasicBlock *successor,
                          std::vector<Fact> facts) {
      edgeFacts[successor][pred] = std::move(facts);
      if (pending[successor] > 0) --pending[successor];
      if (pending[successor] == 0)
        mergeIncoming(successor);
    };
    auto processWork = [&]() {
      while (!work.empty()) {
        auto *block = work.front(); work.pop_front();
        if (!visited.insert(block).second) continue;
        ++stats.blocksVisited;
        auto facts = incoming[block];
        for (auto it = block->begin(); it != block->end();) {
          auto &instruction = *it++;
          if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            if (!call->getCalledFunction() ||
                !call->getCalledFunction()->isIntrinsic() ||
                call->getCalledFunction()->getIntrinsicID() != llvm::Intrinsic::lifetime_start) {
              facts.clear();
              ++stats.callBarriers;
            }
            continue;
          }

          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            auto access = classifyLoad(*load, memoryBases);
            if (!access)
              continue;
            ++stats.guestLoads;
            auto fact = std::find_if(facts.rbegin(), facts.rend(), [&](const Fact &candidate) {
              return candidate.access.bytes == access->bytes &&
                     sameAddress(candidate.access.address, access->address);
            });
            if (fact != facts.rend() && plannedRewrites < options.maxRewrites) {
              ++plannedRewrites;
              if (loopBlocks.contains(block)) {
                pendingRewrites.push_back({load, fact->value, !fact->access.store});
              } else {
                load->replaceAllUsesWith(fact->value);
                load->eraseFromParent();
                ++stats.loadsForwarded;
                if (!fact->access.store)
                  ++stats.loadsReused;
                stats.changed = true;
              }
              continue;
            }
            if (fact == facts.rend())
              for (const auto &candidate : facts)
                if (!sameAddress(candidate.access.address, access->address) &&
                    mayOverlap(candidate.access.address, candidate.access.bytes,
                               access->address, access->bytes)) {
                  ++stats.partialOverlaps;
                  break;
                }
            facts.push_back(Fact{*access, load});
            continue;
          }

          if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            auto access = classifyStore(*store, memoryBases);
            if (!access) {
              if (!isKnownLocalStore(*store, memoryBases, module.getDataLayout())) {
                facts.clear();
                ++stats.aliasBarriers;
              }
              continue;
            }
            ++stats.guestStores;
            invalidate(facts, *access, stats);
            facts.push_back(Fact{*access, access->value});
          }
        }
        for (auto *successor : llvm::successors(block)) {
          if (!reachable.contains(successor)) continue;
          if (dom.dominates(successor, block)) {
            ++stats.loopBarriers;
            edgeFacts[successor][block] = facts;
            continue;
          }
          const bool leavesLoop = loopBlocks.contains(block) &&
                                  !loopBlocks.contains(successor);
          if (leavesLoop) {
            ++stats.loopBarriers;
            deferredEdges.push_back({block, successor, facts});
            continue;
          }
          auto nextFacts = options.enableCrossBlock ? facts : std::vector<Fact>{};
          acceptEdge(block, successor, std::move(nextFacts));
        }
      }
    };
    processWork();

    // A loop is admitted only when its memory fact set is an exact invariant:
    // the state entering the header must match every recorded backedge. This
    // is deliberately narrower than a general loop dataflow fixed point.
    bool loopsStable = true;
    for (const auto &[header, backedges] : backedgePreds) {
      const auto input = incoming.find(header);
      if (input == incoming.end() || backedges.empty()) {
        loopsStable = false;
        break;
      }
      for (auto *pred : backedges) {
        const auto edges = edgeFacts.find(header);
        if (edges == edgeFacts.end()) {
          loopsStable = false;
          break;
        }
        const auto value = edges->second.find(pred);
        if (value == edges->second.end() ||
            !sameFacts(input->second, value->second)) {
          loopsStable = false;
          break;
        }
      }
      if (!loopsStable) break;
    }
    if (!loopsStable) {
      pendingRewrites.clear();
      plannedRewrites = stats.loadsForwarded;
    }
    for (auto &edge : deferredEdges) {
      auto facts = loopsStable && options.enableCrossBlock
          ? std::move(edge.facts) : std::vector<Fact>{};
      acceptEdge(edge.pred, edge.successor, std::move(facts));
    }
    processWork();
    if (loopsStable) {
      for (const auto &rewrite : pendingRewrites) {
        if (!rewrite.load || !rewrite.load->getParent() ||
            stats.loadsForwarded >= options.maxRewrites)
          continue;
        rewrite.load->replaceAllUsesWith(rewrite.value);
        rewrite.load->eraseFromParent();
        ++stats.loadsForwarded;
        if (rewrite.reused) ++stats.loadsReused;
        stats.changed = true;
      }
    }
    if (stats.loadsForwarded >= options.maxRewrites) {
      stats.complete = false;
      ++stats.budgetExhausted;
      break;
    }
  }
  return stats;
}

} // namespace dragon::llvmlifter::detail
