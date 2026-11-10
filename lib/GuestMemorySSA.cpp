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

std::optional<std::uint64_t> constantInteger(const llvm::Value *value) {
  const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant)
    return constant->getBitWidth() <= 64
        ? std::optional<std::uint64_t>(constant->getZExtValue()) : std::nullopt;
  const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
  if (!binary) return std::nullopt;
  const auto left = constantInteger(binary->getOperand(0));
  const auto right = constantInteger(binary->getOperand(1));
  if (!left || !right) return std::nullopt;
  switch (binary->getOpcode()) {
    case llvm::Instruction::Add: return *left + *right;
    case llvm::Instruction::Sub: return *left - *right;
    case llvm::Instruction::Mul: return *left * *right;
    case llvm::Instruction::And: return *left & *right;
    case llvm::Instruction::Or: return *left | *right;
    case llvm::Instruction::Xor: return *left ^ *right;
    case llvm::Instruction::Shl:
      return *right < 64 ? *left << *right : 0;
    case llvm::Instruction::LShr:
      return *right < 64 ? *left >> *right : 0;
    default: return std::nullopt;
  }
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
  if (const auto constant = constantInteger(index))
    return AddressKey{base, nullptr, *constant, true};
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

bool isDefinedValue(const llvm::Value *value) {
  return value && !llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value);
}

bool sameFact(const Fact &left, const Fact &right) {
  return isDefinedValue(left.value) && isDefinedValue(right.value) &&
         left.access.bytes == right.access.bytes &&
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

bool sameFactKeys(const std::vector<Fact> &left, const std::vector<Fact> &right) {
  if (left.size() != right.size()) return false;
  std::vector<bool> matched(right.size(), false);
  for (const auto &fact : left) {
    std::size_t match = right.size();
    for (std::size_t index = 0; index < right.size(); ++index) {
      const auto &candidate = right[index];
      if (!matched[index] && candidate.access.bytes == fact.access.bytes &&
          sameAddress(candidate.access.address, fact.access.address)) {
        match = index;
        break;
      }
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

bool callPreservesMemory(const llvm::CallBase &call) {
  return call.doesNotAccessMemory() || call.onlyReadsMemory();
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

struct BytePart {
  llvm::Value *value = nullptr;
  unsigned byteIndex = 0;
  unsigned valueBytes = 0;
  bool fromLoad = false;
};

using ByteFacts = std::map<llvm::Value *, std::map<std::uint64_t, BytePart>>;

std::optional<std::uint64_t> byteOffset(const Access &access, unsigned index) {
  if (!access.address.constant ||
      access.address.offset > std::numeric_limits<std::uint64_t>::max() - index)
    return std::nullopt;
  return access.address.offset + index;
}

llvm::Value *extractByte(llvm::IRBuilder<> &builder, const BytePart &part) {
  if (!isDefinedValue(part.value) || !part.value->getType()->isIntegerTy())
    return nullptr;
  const unsigned sourceBytes = accessBytes(part.value->getType());
  if (!sourceBytes || part.valueBytes != sourceBytes || part.byteIndex >= sourceBytes)
    return nullptr;
  llvm::Value *shifted = part.value;
  if (part.byteIndex) {
    auto *type = part.value->getType();
    shifted = builder.CreateLShr(
        shifted, llvm::ConstantInt::get(type, part.byteIndex * 8), "guest.mem.byte");
  }
  auto *byteType = builder.getInt8Ty();
  return sourceBytes == 1 ? shifted : builder.CreateTrunc(shifted, byteType,
                                                           "guest.mem.byte");
}

llvm::Value *rebuildBytes(llvm::LoadInst &load, const std::vector<BytePart> &parts,
                          const llvm::DataLayout &layout) {
  const unsigned bytes = accessBytes(load.getType());
  if (!bytes || parts.size() != bytes || !layout.isLittleEndian())
    return nullptr;
  llvm::IRBuilder<> builder(&load);
  auto *type = load.getType();
  llvm::Value *result = nullptr;
  for (unsigned index = 0; index < bytes; ++index) {
    auto *byte = extractByte(builder, parts[index]);
    if (!byte) return nullptr;
    llvm::Value *part = byte;
    if (bytes > 1) {
      part = builder.CreateZExt(byte, type, "guest.mem.zext");
      if (index)
        part = builder.CreateShl(
            part, llvm::ConstantInt::get(type, index * 8), "guest.mem.shl");
    }
    result = result ? builder.CreateOr(result, part, "guest.mem.or") : part;
  }
  return result;
}

bool sameBytePart(const BytePart &left, const BytePart &right) {
  return left.byteIndex == right.byteIndex &&
         left.valueBytes == right.valueBytes &&
         equivalentValue(left.value, right.value);
}

void runByteForwarding(llvm::Function &function,
                       const std::unordered_set<llvm::Value *> &memoryBases,
                       const llvm::DataLayout &layout,
                       const MemoryForwardOptions &options,
                       MemoryForwardStats &stats) {
  if (!layout.isLittleEndian()) return;
  llvm::DominatorTree dom(function);
  std::unordered_set<llvm::BasicBlock *> reachable;
  std::deque<llvm::BasicBlock *> reachQueue{&function.getEntryBlock()};
  while (!reachQueue.empty()) {
    auto *block = reachQueue.front(); reachQueue.pop_front();
    if (!reachable.insert(block).second) continue;
    for (auto *successor : llvm::successors(block)) reachQueue.push_back(successor);
  }
  std::unordered_set<llvm::BasicBlock *> loopBlocks;
  for (auto *header : reachable) {
    for (auto *pred : llvm::predecessors(header)) {
      if (!reachable.contains(pred) || !dom.dominates(header, pred)) continue;
      std::deque<llvm::BasicBlock *> loopQueue{pred};
      loopBlocks.insert(header);
      while (!loopQueue.empty()) {
        auto *member = loopQueue.front(); loopQueue.pop_front();
        if (!loopBlocks.insert(member).second) continue;
        for (auto *memberPred : llvm::predecessors(member))
          if (reachable.contains(memberPred) && memberPred != header)
            loopQueue.push_back(memberPred);
      }
    }
  }

  std::map<llvm::BasicBlock *, ByteFacts> incoming;
  std::map<llvm::BasicBlock *, std::map<llvm::BasicBlock *, ByteFacts>> edgeFacts;
  std::map<llvm::BasicBlock *, std::size_t> pending;
  std::deque<llvm::BasicBlock *> work;
  std::unordered_set<llvm::BasicBlock *> visited;
  incoming[&function.getEntryBlock()] = {};
  work.push_back(&function.getEntryBlock());
  for (auto *block : reachable) {
    if (block == &function.getEntryBlock()) continue;
    pending[block] = 0;
    for (auto *pred : llvm::predecessors(block))
      if (reachable.contains(pred) && !dom.dominates(block, pred)) ++pending[block];
  }
  auto mergeIncoming = [&](llvm::BasicBlock *successor) {
    ByteFacts merged;
    const auto &edges = edgeFacts[successor];
    if (!edges.empty()) {
      const auto &first = edges.begin()->second;
      for (const auto &[base, baseFacts] : first) {
        for (const auto &[offset, part] : baseFacts) {
          bool present = true;
          for (const auto &[pred, predFacts] : edges) {
            (void)pred;
            auto baseIt = predFacts.find(base);
            auto partIt = baseIt == predFacts.end()
                ? std::map<std::uint64_t, BytePart>::const_iterator{}
                : baseIt->second.find(offset);
            if (baseIt == predFacts.end() || partIt == baseIt->second.end() ||
                !sameBytePart(part, partIt->second)) {
              present = false;
              break;
            }
          }
          if (present) merged[base][offset] = part;
        }
      }
    }
    incoming[successor] = std::move(merged);
    work.push_back(successor);
  };
  auto acceptEdge = [&](llvm::BasicBlock *pred, llvm::BasicBlock *successor,
                        ByteFacts facts) {
    edgeFacts[successor][pred] = std::move(facts);
    if (pending[successor] > 0) --pending[successor];
    if (pending[successor] == 0) mergeIncoming(successor);
  };
  while (!work.empty()) {
    auto *block = work.front(); work.pop_front();
    if (!visited.insert(block).second) continue;
    ByteFacts facts = incoming[block];
    for (auto it = block->begin(); it != block->end();) {
      auto &instruction = *it++;
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const bool lifetimeStart = call->getCalledFunction() &&
            call->getCalledFunction()->isIntrinsic() &&
            call->getCalledFunction()->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
        if (!lifetimeStart && !callPreservesMemory(*call))
          facts.clear();
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        auto access = classifyStore(*store, memoryBases);
        if (!access) {
          if (!isKnownLocalStore(*store, memoryBases, layout)) facts.clear();
          continue;
        }
        if (!access->address.constant || !isDefinedValue(access->value)) {
          facts.clear();
          continue;
        }
        auto &baseFacts = facts[access->address.base];
        for (unsigned index = 0; index < access->bytes; ++index) {
          const auto offset = byteOffset(*access, index);
          if (!offset) { facts.clear(); break; }
          baseFacts[*offset] = BytePart{access->value, index, access->bytes, false};
        }
        continue;
      }

      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (!load) continue;
      auto access = classifyLoad(*load, memoryBases);
      if (!access || !access->address.constant) continue;
      auto base = facts.find(access->address.base);
      if (base != facts.end()) {
        std::vector<BytePart> parts;
        parts.reserve(access->bytes);
        bool complete = true;
        for (unsigned index = 0; index < access->bytes; ++index) {
          const auto offset = byteOffset(*access, index);
          if (!offset) { complete = false; break; }
          auto part = base->second.find(*offset);
          if (part == base->second.end()) { complete = false; break; }
          parts.push_back(part->second);
        }
        if (complete && stats.loadsForwarded < options.maxRewrites) {
          if (auto *value = rebuildBytes(*load, parts, layout)) {
            load->replaceAllUsesWith(value);
            load->eraseFromParent();
            ++stats.loadsForwarded;
            ++stats.byteRebuilds;
            if (std::all_of(parts.begin(), parts.end(),
                            [](const BytePart &part) { return part.fromLoad; }))
              ++stats.loadsReused;
            stats.changed = true;
            continue;
          }
        }
      }
      if (!isDefinedValue(load)) continue;
      auto &baseFacts = facts[access->address.base];
      for (unsigned index = 0; index < access->bytes; ++index) {
        const auto offset = byteOffset(*access, index);
        if (!offset) { facts.clear(); break; }
        if (!baseFacts.contains(*offset))
          baseFacts[*offset] = BytePart{load, index, access->bytes, true};
      }
    }
    for (auto *successor : llvm::successors(block)) {
      if (!reachable.contains(successor) || dom.dominates(successor, block)) continue;
      auto nextFacts = (loopBlocks.contains(block) || loopBlocks.contains(successor))
          ? ByteFacts{} : facts;
      acceptEdge(block, successor, std::move(nextFacts));
    }
    if (stats.loadsForwarded >= options.maxRewrites) return;
  }
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
      AddressKey address;
      unsigned bytes = 0;
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
            if (!presentEverywhere ||
                std::any_of(values.begin(), values.end(),
                            [](llvm::Value *value) { return !isDefinedValue(value); })) {
              ++stats.joinBarriers;
              continue;
            }
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
            const bool lifetimeStart = call->getCalledFunction() &&
                call->getCalledFunction()->isIntrinsic() &&
                call->getCalledFunction()->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
            if (!lifetimeStart && !callPreservesMemory(*call)) {
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
            if (fact != facts.rend() && isDefinedValue(fact->value) &&
                plannedRewrites < options.maxRewrites) {
              ++plannedRewrites;
              if (loopBlocks.contains(block)) {
                pendingRewrites.push_back({load, fact->value, access->address,
                                           access->bytes, !fact->access.store});
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
    struct LoopPhi {
      AddressKey address;
      unsigned bytes = 0;
      llvm::PHINode *phi = nullptr;
      llvm::Value *entryValue = nullptr;
    };
    std::vector<LoopPhi> loopPhis;
    llvm::BasicBlock *admittedLoopHeader = nullptr;
    llvm::BasicBlock *admittedLoopBody = nullptr;
    bool loopPhiAdmitted = false;
    if (!loopsStable && options.enablePhi && backedgePreds.size() == 1 &&
        loopBlocks.size() == 2) {
      const auto &loop = *backedgePreds.begin();
      auto *header = loop.first;
      auto *body = *loop.second.begin();
      std::vector<llvm::BasicBlock *> outsidePreds;
      for (auto *pred : llvm::predecessors(header))
        if (pred != body) outsidePreds.push_back(pred);
      std::vector<llvm::BasicBlock *> headerExits;
      for (auto *successor : llvm::successors(header))
        if (successor != body) headerExits.push_back(successor);
      std::vector<llvm::BasicBlock *> bodyExits;
      for (auto *successor : llvm::successors(body))
        if (successor != header) bodyExits.push_back(successor);
      const auto input = incoming.find(header);
      const auto edges = edgeFacts.find(header);
      const std::vector<Fact> *backFacts = nullptr;
      if (edges != edgeFacts.end()) {
        const auto backValue = edges->second.find(body);
        if (backValue != edges->second.end())
          backFacts = &backValue->second;
      }
      const bool headerHasGuestStore = std::any_of(
          header->begin(), header->end(), [&](const llvm::Instruction &instruction) {
            const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            return store && classifyStore(
                *const_cast<llvm::StoreInst *>(store), memoryBases).has_value();
          });
      bool valid = outsidePreds.size() == 1 && headerExits.size() == 1 &&
                   bodyExits.size() <= 1 && !loopBlocks.contains(outsidePreds.front()) &&
                   !headerHasGuestStore && input != incoming.end() &&
                   backFacts && sameFactKeys(input->second, *backFacts);
      if (valid) {
        for (const auto &candidate : input->second) {
          auto it = std::find_if(backFacts->begin(), backFacts->end(),
              [&](const Fact &fact) {
                return fact.access.bytes == candidate.access.bytes &&
                       sameAddress(fact.access.address, candidate.access.address);
              });
          if (it == backFacts->end() ||
              !isDefinedValue(candidate.value) || !isDefinedValue(it->value) ||
              it->value->getType() != candidate.value->getType()) {
            valid = false;
            break;
          }
          if (equivalentValue(candidate.value, it->value))
            continue;
          if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(it->value);
              instruction && loopBlocks.contains(
                  const_cast<llvm::BasicBlock *>(instruction->getParent()))) {
            valid = false;
            break;
          }
          llvm::IRBuilder<> builder(header, header->getFirstInsertionPt());
          auto *phi = builder.CreatePHI(candidate.value->getType(), 2,
                                        "guest.mem.loop.phi");
          phi->addIncoming(candidate.value, outsidePreds.front());
          phi->addIncoming(it->value, body);
          loopPhis.push_back({candidate.access.address, candidate.access.bytes,
                              phi, candidate.value});
          ++stats.phisInserted;
        }
        loopPhiAdmitted = valid && !loopPhis.empty();
        if (!loopPhiAdmitted) {
          for (const auto &loopPhi : loopPhis)
            loopPhi.phi->eraseFromParent();
          if (stats.phisInserted >= loopPhis.size())
            stats.phisInserted -= loopPhis.size();
          loopPhis.clear();
        } else {
          admittedLoopHeader = header;
          admittedLoopBody = body;
        }
      }
    }
    const bool loopAdmitted = loopsStable || loopPhiAdmitted;
    if (!loopAdmitted) {
      pendingRewrites.clear();
      plannedRewrites = stats.loadsForwarded;
    } else if (loopPhiAdmitted) {
      for (auto &rewrite : pendingRewrites) {
        if (!rewrite.load) continue;
        auto *parent = rewrite.load->getParent();
        for (const auto &loopPhi : loopPhis) {
          if (rewrite.bytes != loopPhi.bytes ||
              !sameAddress(rewrite.address, loopPhi.address) ||
              !loopBlocks.contains(parent))
            continue;
          if (parent == admittedLoopHeader ||
              (parent == admittedLoopBody &&
               equivalentValue(rewrite.value, loopPhi.entryValue)))
            rewrite.value = loopPhi.phi;
        }
      }
    }
    for (auto &edge : deferredEdges) {
      auto facts = loopAdmitted && options.enableCrossBlock
          ? std::move(edge.facts) : std::vector<Fact>{};
      if (loopPhiAdmitted && edge.pred == admittedLoopHeader) {
        for (auto &fact : facts)
          for (const auto &loopPhi : loopPhis)
            if (fact.access.bytes == loopPhi.bytes &&
                sameAddress(fact.access.address, loopPhi.address))
              fact.value = loopPhi.phi;
      }
      acceptEdge(edge.pred, edge.successor, std::move(facts));
    }
    processWork();
    if (loopAdmitted) {
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
    if (stats.loadsForwarded < options.maxRewrites)
      runByteForwarding(function, memoryBases, module.getDataLayout(), options, stats);
    if (stats.loadsForwarded >= options.maxRewrites) {
      stats.complete = false;
      ++stats.budgetExhausted;
      break;
    }
  }
  return stats;
}

} // namespace dragon::llvmlifter::detail
