#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <dragon/lifters/llvm/Execution.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
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

    auto memoryBlock = [](const llvm::BasicBlock *block) {
      return block->getName().starts_with("memory.");
    };
    auto carriesFacts = [&](const llvm::BasicBlock *from,
                            const llvm::BasicBlock *to) {
      if (to->getName().starts_with("bb_") ||
          to->getName().starts_with("call."))
        return false;
      return memoryBlock(to) || memoryBlock(from);
    };
    auto sameFacts = [](const std::vector<Fact> &left,
                        const std::vector<Fact> &right) {
      if (left.size() != right.size()) return false;
      for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].access.bytes != right[i].access.bytes ||
            !sameAddress(left[i].access.address, right[i].access.address) ||
            left[i].value != right[i].value)
          return false;
      }
      return true;
    };
    std::map<llvm::BasicBlock *, std::vector<Fact>> incoming;
    std::deque<llvm::BasicBlock *> work;
    incoming[&function.getEntryBlock()] = {};
    work.push_back(&function.getEntryBlock());
    std::unordered_set<llvm::BasicBlock *> visited;
    while (!work.empty()) {
      auto *block = work.front();
      work.pop_front();
      if (!visited.insert(block).second ||
          (block != &function.getEntryBlock() && llvm::pred_size(block) > 1))
        continue; // joins/loops are S2's next PHI stage
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
          if (fact != facts.rend() && stats.loadsForwarded < options.maxRewrites) {
            load->replaceAllUsesWith(fact->value);
            load->eraseFromParent();
            ++stats.loadsForwarded;
            if (!fact->access.store)
              ++stats.loadsReused;
            stats.changed = true;
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
          if (fact == facts.rend())
            facts.push_back(Fact{*access, load});
          else
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
        auto nextFacts = carriesFacts(block, successor)
            ? facts : std::vector<Fact>{};
        auto [it, inserted] = incoming.emplace(successor, nextFacts);
        if (inserted)
          work.push_back(successor);
        else if (sameFacts(it->second, nextFacts))
          work.push_back(successor);
        else
          it->second.clear(); // conflicting paths are not forwarded
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
