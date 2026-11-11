#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/CallEffectSummary.hpp>

#include "GuestMemoryEdges.hpp"

#include "analysis/LeafCallEffects.hpp"
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/llvm/Execution.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CaptureTracking.h>
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

bool sameAddress(const AddressKey &left, const AddressKey &right);

llvm::Value *stripPointerCasts(llvm::Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

struct ConstantIndexBudget {
  static constexpr unsigned MaxWidth = 128;
  static constexpr unsigned MaxDepth = 64;
  static constexpr std::size_t MaxNodes = 256;
  std::size_t nodes = 0;
};

std::optional<llvm::APInt> constantInteger(
    const llvm::Value *value, ConstantIndexBudget &budget, unsigned depth = 0) {
  if (!value || depth > ConstantIndexBudget::MaxDepth ||
      ++budget.nodes > ConstantIndexBudget::MaxNodes)
    return std::nullopt;
  const auto *type = llvm::dyn_cast<llvm::IntegerType>(value->getType());
  if (!type || type->getBitWidth() > ConstantIndexBudget::MaxWidth)
    return std::nullopt;
  const unsigned width = type->getBitWidth();
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
    return constant->getValue();
  const auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  // A flagged operation may be poison. In particular freeze(poison) is a
  // stable arbitrary value, not the unflagged operation's modular result.
  if (!instruction || instruction->hasPoisonGeneratingFlags())
    return std::nullopt;
  if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(instruction))
    return constantInteger(freeze->getOperand(0), budget, depth + 1);
  if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(instruction)) {
    auto operand = constantInteger(cast->getOperand(0), budget, depth + 1);
    if (!operand) return std::nullopt;
    switch (cast->getOpcode()) {
      case llvm::Instruction::ZExt:
      case llvm::Instruction::Trunc: return operand->zextOrTrunc(width);
      case llvm::Instruction::SExt: return operand->sextOrTrunc(width);
      default: return std::nullopt;
    }
  }
  const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction);
  if (!binary) return std::nullopt;
  const auto left = constantInteger(binary->getOperand(0), budget, depth + 1);
  const auto right = constantInteger(binary->getOperand(1), budget, depth + 1);
  if (!left || !right || left->getBitWidth() != width ||
      right->getBitWidth() != width)
    return std::nullopt;
  switch (binary->getOpcode()) {
    case llvm::Instruction::Add: return *left + *right;
    case llvm::Instruction::Sub: return *left - *right;
    case llvm::Instruction::Mul: return *left * *right;
    case llvm::Instruction::And: return *left & *right;
    case llvm::Instruction::Or: return *left | *right;
    case llvm::Instruction::Xor: return *left ^ *right;
    case llvm::Instruction::Shl:
      if (!right->uge(width)) return left->shl(right->getZExtValue());
      return std::nullopt;
    case llvm::Instruction::LShr:
      if (!right->uge(width)) return left->lshr(right->getZExtValue());
      return std::nullopt;
    case llvm::Instruction::AShr:
      if (!right->uge(width)) return left->ashr(right->getZExtValue());
      return std::nullopt;
    default: return std::nullopt;
  }
}

std::optional<AddressKey> addressKey(
    const llvm::Value *pointer,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout,
    std::unordered_set<const llvm::Value *> &active) {
  if (!pointer || !active.insert(pointer).second)
    return std::nullopt;

  auto *stripped = stripPointerCasts(const_cast<llvm::Value *>(pointer));
  if (stripped != pointer) {
    auto result = addressKey(stripped, memoryBases, layout, active);
    active.erase(pointer);
    return result;
  }

  if (memoryBases.contains(stripped)) {
    active.erase(pointer);
    return AddressKey{stripped, nullptr, 0, true};
  }

  std::optional<AddressKey> result;
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stripped)) {
    auto base = addressKey(gep->getPointerOperand(), memoryBases, layout, active);
    if (base) {
      // AddressKey/ByteFacts describe full 64-bit byte displacements. A narrower
      // GEP index wraps only the low pointer bits, so linear byte reconstruction
      // is not justified without a separate pointer-range proof. Keep the exact
      // GEP identity rather than inventing a full-width constant displacement.
      if (layout.getPointerSizeInBits(gep->getPointerAddressSpace()) != 64 ||
          layout.getIndexSizeInBits(gep->getPointerAddressSpace()) != 64) {
        active.erase(pointer);
        return AddressKey{base->base, stripped, 0, false};
      }
      llvm::APInt offset(64, 0);
      bool constantOffset = gep->accumulateConstantOffset(layout, offset);
      // LLVM's opaque-pointer GEPs normally provide the complete DataLayout
      // information above. Keep a bounded one-index fallback for lifted
      // Guest byte GEPs whose source type is already explicit; this preserves
      // the old x86 lifter form without treating an unknown index as a byte
      // offset.
      if (!constantOffset && gep->getNumIndices() == 1) {
        const auto *sourceType = gep->getSourceElementType();
        const auto *index = gep->getOperand(gep->getNumOperands() - 1);
        ConstantIndexBudget budget;
        if (sourceType->isSized()) {
          const auto size = layout.getTypeAllocSize(
              const_cast<llvm::Type *>(sourceType));
          const auto value = constantInteger(index, budget);
          if (value && !size.isScalable()) {
            // GEP sign extends or truncates the final integer index to the
            // pointer index width before multiplying by the fixed type size.
            offset = value->sextOrTrunc(64) * llvm::APInt(64, size.getFixedValue());
            constantOffset = true;
          }
        }
      }
      if (constantOffset) {
        const auto delta = offset.zextOrTrunc(64).getZExtValue();
        if (base->constant)
          result = AddressKey{base->base, nullptr, base->offset + delta, true};
        else
          result = AddressKey{base->base, base->dynamic,
                              base->offset + delta, false};
      } else {
        // Preserve the exact dynamic GEP identity.  This is intentionally
        // less aggressive than constructing an algebraic pointer expression:
        // separately computed but merely similar dynamic addresses remain
        // potentially aliased and therefore conservative.
        result = AddressKey{base->base, stripped, 0, false};
      }
    }
  } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(stripped)) {
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      auto incoming = addressKey(phi->getIncomingValue(index), memoryBases,
                                 layout, active);
      if (!incoming || (result && !sameAddress(*result, *incoming))) {
        result.reset();
        break;
      }
      result = incoming;
    }
  } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(stripped)) {
    auto left = addressKey(select->getTrueValue(), memoryBases, layout, active);
    auto right = addressKey(select->getFalseValue(), memoryBases, layout, active);
    if (left && right && sameAddress(*left, *right))
      result = left;
  }

  active.erase(pointer);
  return result;
}

std::optional<AddressKey> addressKey(
    const llvm::Value *pointer,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout) {
  std::unordered_set<const llvm::Value *> active;
  return addressKey(pointer, memoryBases, layout, active);
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
  return left.dynamic == right.dynamic && left.offset == right.offset;
}

// Return true only when two concrete byte ranges are definitely disjoint.
// Unknown/dynamic addresses are never declared disjoint.
bool provablyDisjoint(const AddressKey &left, unsigned leftBytes,
                      const AddressKey &right, unsigned rightBytes) {
  if (left.base != right.base || !left.constant || !right.constant)
    return false;
  const auto *base = llvm::dyn_cast<llvm::Instruction>(left.base);
  if (!base || !base->getType()->isPointerTy()) return false;
  const auto width = base->getModule()->getDataLayout().getIndexSizeInBits(
      base->getType()->getPointerAddressSpace());
  if (!width || width > 64) return false;
  const auto mask = width == 64 ? std::numeric_limits<std::uint64_t>::max()
                               : (std::uint64_t{1} << width) - 1;
  // Circular half-open ranges are disjoint only if neither start lies in the
  // other range. Both distances are unsigned and use the actual index width.
  const auto forward = (right.offset - left.offset) & mask;
  const auto backward = (left.offset - right.offset) & mask;
  return forward >= leftBytes && backward >= rightBytes;
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

// A proof is keyed by the explicit target metadata emitted for a static
// internal call.  It is deliberately narrower than the full LeafCallProof:
// Guest-memory SSA can consume only a proof that the callee writes no memory
// at all.  Stack writes and constants are retained as barriers because this
// pass has no callee-entry-SP/global-address relation to the Guest base.
using LeafCallCache = std::map<std::uint64_t, bool>;

bool provenLeafNoMemoryWrite(const llvm::CallBase &call, const Graph *graph,
                             const MemoryForwardOptions &options,
                             LeafCallCache &cache) {
  if (!options.proveLeafCalls || !graph)
    return false;
  const auto *metadata = call.getMetadata("dragon.guest.call.target");
  if (!metadata || metadata->getNumOperands() != 1)
    return false;
  const auto *constant = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
      metadata->getOperand(0));
  if (!constant || constant->getBitWidth() > 64)
    return false;
  const auto target = constant->getZExtValue();
  if (const auto it = cache.find(target); it != cache.end())
    return it->second;

  const auto proof = analysis::detail::proveLeafCallee(
      *graph, target, options.maxCallBlocks, options.maxCallOperations,
      options.maxCallFacts, {});
  const bool safe = proof.proven && proof.stackWriteBytes.empty() &&
                    proof.memoryConstants.empty();
  cache.emplace(target, safe);
  return safe;
}

void invalidate(std::vector<Fact> &, const Access &, MemoryForwardStats &);

enum class CallMemoryEffect {
  Barrier,
  ReadOnly,
  ProvenLeafNoWrite,
  ProvenSummaryNoWrite,
  ProvenSummaryWrites,
  SummaryRejected,
};

bool hasCallEffectSummary(const llvm::CallBase &call) {
  return call.getMetadata("dragon.call.effect") != nullptr;
}

CallMemoryEffect callMemoryEffect(const llvm::CallBase &call,
                                  const Graph *graph,
                                  const MemoryForwardOptions &options,
                                  LeafCallCache &cache) {
  if (callPreservesMemory(call))
    return CallMemoryEffect::ReadOnly;
  if (options.consumeCallEffectSummaries) {
    const auto summary = getCallEffectSummary(call);
    if (summary && (summary->memory == CallMemoryEffectKind::NoMemory ||
                    summary->memory == CallMemoryEffectKind::ReadOnly))
      return CallMemoryEffect::ProvenSummaryNoWrite;
    if (summary && summary->memory == CallMemoryEffectKind::KnownRanges) {
      const bool guestRanges = std::all_of(
          summary->writes.begin(), summary->writes.end(), [](const auto &range) {
            return range.base == CallMemoryRange::Base::GuestMemory;
          });
      return guestRanges ? CallMemoryEffect::ProvenSummaryWrites
                         : CallMemoryEffect::SummaryRejected;
    }
    if (hasCallEffectSummary(call))
      return CallMemoryEffect::SummaryRejected;
  }
  if (provenLeafNoMemoryWrite(call, graph, options, cache))
    return CallMemoryEffect::ProvenLeafNoWrite;
  return CallMemoryEffect::Barrier;
}

std::vector<llvm::Value *> summaryBases(
    const std::vector<Fact> &facts,
    const std::unordered_set<llvm::Value *> &memoryBases) {
  std::vector<llvm::Value *> bases;
  for (const auto &fact : facts)
    if (fact.access.address.base &&
        std::find(bases.begin(), bases.end(), fact.access.address.base) == bases.end())
      bases.push_back(fact.access.address.base);
  if (bases.empty())
    for (auto *base : memoryBases)
      bases.push_back(base);
  return bases;
}

bool invalidateSummaryWrites(
    std::vector<Fact> &facts, const CallEffectSummary &summary,
    const std::unordered_set<llvm::Value *> &memoryBases,
    llvm::LLVMContext &context, MemoryForwardStats &stats) {
  if (summary.memory != CallMemoryEffectKind::KnownRanges ||
      !std::all_of(summary.writes.begin(), summary.writes.end(),
                   [](const auto &range) {
                     return range.base == CallMemoryRange::Base::GuestMemory;
                   }))
    return false;
  const auto bases = summaryBases(facts, memoryBases);
  for (const auto &range : summary.writes)
    for (auto *base : bases)
      invalidate(facts,
                 Access{nullptr, AddressKey{base, nullptr, range.offset, true},
                        range.bytes, true, nullptr},
                 stats);
  for (const auto &constant : summary.memoryConstants) {
    if (!constant.complete || constant.range.base != CallMemoryRange::Base::GuestMemory ||
        constant.range.bytes == 0 || constant.range.bytes > sizeof(std::uint64_t))
      continue;
    auto *type = llvm::IntegerType::get(context, constant.range.bytes * 8);
    auto *value = llvm::ConstantInt::get(type, constant.value);
    for (auto *base : bases) {
      Access access{nullptr,
                    AddressKey{base, nullptr, constant.range.offset, true},
                    constant.range.bytes, true, value};
      facts.push_back(Fact{access, value});
    }
  }
  return true;
}

bool isKnownLocalStore(const llvm::StoreInst &store,
                       const std::unordered_set<llvm::Value *> &memoryBases,
                       const llvm::DataLayout &layout) {
  if (!store.isSimple()) return false;
  if (addressKey(store.getPointerOperand(), memoryBases, layout))
    return false;
  const auto *underlying = llvm::getUnderlyingObject(store.getPointerOperand(), 32);
  if (llvm::isa<llvm::AllocaInst>(underlying))
    // A captured allocation may itself have been installed as Guest data.
    return !llvm::PointerMayBeCaptured(underlying, true, 128);
  if (const auto *argument = llvm::dyn_cast<llvm::Argument>(underlying))
    return argument->getType()->isPointerTy() && argument->hasNoAliasAttr();
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
                                   const std::unordered_set<llvm::Value *> &memoryBases,
                                   const llvm::DataLayout &layout) {
  if (load.isVolatile() || load.isAtomic())
    return std::nullopt;
  auto address = addressKey(load.getPointerOperand(), memoryBases, layout);
  const auto bytes = accessBytes(load.getType());
  if (!address || !bytes)
    return std::nullopt;
  return Access{&load, *address, bytes, false, &load};
}

std::optional<Access> classifyStore(llvm::StoreInst &store,
                                    const std::unordered_set<llvm::Value *> &memoryBases,
                                    const llvm::DataLayout &layout) {
  if (store.isVolatile() || store.isAtomic())
    return std::nullopt;
  auto address = addressKey(store.getPointerOperand(), memoryBases, layout);
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

bool invalidateByteSummaryWrites(
    ByteFacts &facts, const CallEffectSummary &summary,
    const std::unordered_set<llvm::Value *> &memoryBases,
    llvm::LLVMContext &context) {
  if (summary.memory != CallMemoryEffectKind::KnownRanges ||
      !std::all_of(summary.writes.begin(), summary.writes.end(),
                   [](const auto &range) {
                     return range.base == CallMemoryRange::Base::GuestMemory;
                   }))
    return false;
  for (const auto &range : summary.writes) {
    if (range.offset > std::numeric_limits<std::uint64_t>::max() - range.bytes) {
      facts.clear();
      continue;
    }
    const auto end = range.offset + range.bytes;
    for (auto base = facts.begin(); base != facts.end();) {
      auto &bytes = base->second;
      for (auto it = bytes.begin(); it != bytes.end();) {
        if (it->first >= range.offset && it->first < end)
          it = bytes.erase(it);
        else
          ++it;
      }
      if (bytes.empty()) base = facts.erase(base);
      else ++base;
    }
  }
  std::vector<llvm::Value *> bases;
  for (const auto &[base, bytes] : facts)
    if (std::find(bases.begin(), bases.end(), base) == bases.end())
      bases.push_back(base);
  if (bases.empty())
    for (auto *base : memoryBases) bases.push_back(base);
  for (const auto &constant : summary.memoryConstants) {
    if (!constant.complete || constant.range.base != CallMemoryRange::Base::GuestMemory ||
        constant.range.bytes == 0 || constant.range.bytes > sizeof(std::uint64_t))
      continue;
    auto *type = llvm::IntegerType::get(context, constant.range.bytes * 8);
    auto *value = llvm::ConstantInt::get(type, constant.value);
    for (auto *base : bases) {
      auto &parts = facts[base];
      for (unsigned index = 0; index < constant.range.bytes; ++index)
        parts[constant.range.offset + index] =
            BytePart{value, index, constant.range.bytes, false};
    }
  }
  return true;
}

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

// A non-natural cycle can leave the predecessor-count scheduler waiting on
// itself.  Once the ordinary worklist and its loop proofs have finished, the
// remaining reachable blocks may still use facts established within that very
// block.  Never import a predecessor fact, export a fact, or create a PHI here.
void forwardConservativeBlocks(
    llvm::Function &function,
    const std::unordered_set<llvm::BasicBlock *> &reachable,
    const std::unordered_set<llvm::BasicBlock *> &visited,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats) {
  for (auto &block : function) {
    if (!reachable.contains(&block) || visited.contains(&block)) continue;
    if (stats.loadsForwarded >= options.maxRewrites) return;
    ++stats.blocksVisited;
    std::vector<Fact> facts;
    for (auto it = block.begin(); it != block.end();) {
      auto &instruction = *it++;
      if (llvm::isa<llvm::CallBase, llvm::FenceInst>(instruction)) {
        facts.clear();
        if (llvm::isa<llvm::CallBase>(instruction)) ++stats.callBarriers;
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        auto access = classifyStore(*store, memoryBases, layout);
        if (!access) {
          // Even a store through another pointer argument may alias Guest
          // memory.  The fallback has no external/local address assumptions.
          facts.clear();
          ++stats.aliasBarriers;
          continue;
        }
        ++stats.guestStores;
        invalidate(facts, *access, stats);
        if (isDefinedValue(access->value))
          facts.push_back(Fact{*access, access->value});
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (!load->isSimple()) {
          facts.clear();
          continue;
        }
        auto access = classifyLoad(*load, memoryBases, layout);
        if (!access) continue;
        ++stats.guestLoads;
        const auto fact = std::find_if(facts.rbegin(), facts.rend(),
            [&](const Fact &candidate) {
              return candidate.access.bytes == access->bytes &&
                     sameAddress(candidate.access.address, access->address);
            });
        if (fact != facts.rend()) {
          load->replaceAllUsesWith(fact->value);
          load->eraseFromParent();
          ++stats.loadsForwarded;
          stats.changed = true;
          if (stats.loadsForwarded >= options.maxRewrites) return;
        }
        continue;
      }
      if (instruction.mayWriteToMemory()) facts.clear();
    }
  }
}

void forwardConservativeBytes(
    llvm::Function &function,
    const std::unordered_set<llvm::BasicBlock *> &reachable,
    const std::unordered_set<llvm::BasicBlock *> &visited,
    const std::unordered_set<llvm::Value *> &memoryBases,
    const llvm::DataLayout &layout, const MemoryForwardOptions &options,
    MemoryForwardStats &stats) {
  for (auto &block : function) {
    if (!reachable.contains(&block) || visited.contains(&block)) continue;
    ByteFacts facts;
    for (auto it = block.begin(); it != block.end();) {
      if (stats.loadsForwarded >= options.maxRewrites) return;
      auto &instruction = *it++;
      if (llvm::isa<llvm::CallBase, llvm::FenceInst>(instruction)) {
        facts.clear();
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        auto access = classifyStore(*store, memoryBases, layout);
        if (!access || !access->address.constant ||
            !isDefinedValue(access->value)) {
          facts.clear();
          continue;
        }
        // Distinct loaded Guest bases are not a no-alias proof.
        for (auto base = facts.begin(); base != facts.end();) {
          if (base->first != access->address.base) base = facts.erase(base);
          else ++base;
        }
        auto &baseFacts = facts[access->address.base];
        for (unsigned index = 0; index < access->bytes; ++index) {
          const auto offset = byteOffset(*access, index);
          if (!offset) { facts.clear(); break; }
          baseFacts[*offset] = BytePart{access->value, index, access->bytes, false};
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (!load->isSimple()) {
          facts.clear();
          continue;
        }
        auto access = classifyLoad(*load, memoryBases, layout);
        if (!access || !access->address.constant) continue;
        auto base = facts.find(access->address.base);
        if (base == facts.end()) continue;
        std::vector<BytePart> parts;
        for (unsigned index = 0; index < access->bytes; ++index) {
          const auto offset = byteOffset(*access, index);
          if (!offset) break;
          const auto part = base->second.find(*offset);
          if (part == base->second.end()) break;
          parts.push_back(part->second);
        }
        if (parts.size() != access->bytes) continue;
        if (auto *value = rebuildBytes(*load, parts, layout)) {
          load->replaceAllUsesWith(value);
          load->eraseFromParent();
          ++stats.loadsForwarded;
          ++stats.byteRebuilds;
          stats.changed = true;
        }
        continue;
      }
      if (instruction.mayWriteToMemory()) facts.clear();
    }
  }
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
                       MemoryForwardStats &stats, const Graph *graph,
                       LeafCallCache &leafCallCache) {
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
        if (!lifetimeStart) {
          const auto effect = callMemoryEffect(*call, graph, options, leafCallCache);
          if (effect == CallMemoryEffect::Barrier ||
              effect == CallMemoryEffect::SummaryRejected) {
            facts.clear();
            if (options.consumeCallEffectSummaries && hasCallEffectSummary(*call))
              ++stats.summaryCallsRejected;
          } else if (effect == CallMemoryEffect::ProvenSummaryNoWrite) {
            ++stats.summaryCallsPreserved;
          } else if (effect == CallMemoryEffect::ProvenSummaryWrites) {
            const auto summary = getCallEffectSummary(*call);
            if (summary && invalidateByteSummaryWrites(
                                facts, *summary, memoryBases, function.getContext()))
              ++stats.summaryCallsPreserved;
            else {
              facts.clear();
              ++stats.summaryCallsRejected;
            }
          }
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        auto access = classifyStore(*store, memoryBases, layout);
        if (!access) {
          if (!isKnownLocalStore(*store, memoryBases, layout)) facts.clear();
          continue;
        }
        if (!access->address.constant || !isDefinedValue(access->value)) {
          facts.clear();
          continue;
        }
        // Different loads of descriptor.data may refer to the same bytes.
        for (auto base = facts.begin(); base != facts.end();) {
          if (base->first != access->address.base) base = facts.erase(base);
          else ++base;
        }
        auto &baseFacts = facts[access->address.base];
        for (unsigned index = 0; index < access->bytes; ++index) {
          const auto offset = byteOffset(*access, index);
          if (!offset) { facts.clear(); break; }
          baseFacts[*offset] = BytePart{access->value, index, access->bytes, false};
        }
        continue;
      }

      if (llvm::isa<llvm::FenceInst>(instruction) || instruction.mayWriteToMemory()) {
        facts.clear();
        continue;
      }
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (!load) continue;
      if (!load->isSimple()) {
        facts.clear();
        continue;
      }
      auto access = classifyLoad(*load, memoryBases, layout);
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
  forwardConservativeBytes(function, reachable, visited, memoryBases, layout,
                           options, stats);
}

} // namespace

MemoryForwardStats runGuestMemorySSA(llvm::Module &module,
                                     const MemoryForwardOptions &options) {
  return runGuestMemorySSA(module, options, nullptr);
}

MemoryForwardStats runGuestMemorySSA(llvm::Module &module,
                                     const MemoryForwardOptions &options,
                                     const Graph *graph) {
  MemoryForwardStats stats;
  stats.available = true;
  if (!options.maxRewrites)
    return stats;

  LeafCallCache leafCallCache;
  CheckedMemoryEdgeBudget checkedEdgeBudget;
  struct PendingCheckedHubs {
    llvm::Function *function;
    std::unordered_set<llvm::Value *> memoryBases;
    std::vector<llvm::LoadInst *> loads;
  };
  std::vector<PendingCheckedHubs> pendingCheckedHubs;
  bool rewriteBudgetReported = false;

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
            const auto effect = lifetimeStart
                ? CallMemoryEffect::ReadOnly
                : callMemoryEffect(*call, graph, options, leafCallCache);
            if (!lifetimeStart &&
                (effect == CallMemoryEffect::Barrier ||
                 effect == CallMemoryEffect::SummaryRejected)) {
              facts.clear();
              ++stats.callBarriers;
              if (options.consumeCallEffectSummaries && hasCallEffectSummary(*call))
                ++stats.summaryCallsRejected;
            } else if (!lifetimeStart &&
                       effect == CallMemoryEffect::ProvenLeafNoWrite) {
              ++stats.leafCallsPreserved;
            } else if (!lifetimeStart &&
                       effect == CallMemoryEffect::ProvenSummaryNoWrite) {
              ++stats.summaryCallsPreserved;
            } else if (!lifetimeStart &&
                       effect == CallMemoryEffect::ProvenSummaryWrites) {
              const auto summary = getCallEffectSummary(*call);
              if (summary && invalidateSummaryWrites(
                                  facts, *summary, memoryBases,
                                  module.getContext(), stats))
                ++stats.summaryCallsPreserved;
              else {
                facts.clear();
                ++stats.summaryCallsRejected;
                ++stats.callBarriers;
              }
            }
            continue;
          }

          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            if (!load->isSimple()) {
              facts.clear();
              ++stats.aliasBarriers;
              continue;
            }
            auto access = classifyLoad(*load, memoryBases, module.getDataLayout());
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
            auto access = classifyStore(*store, memoryBases, module.getDataLayout());
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
            continue;
          }
          if (llvm::isa<llvm::FenceInst>(instruction) || instruction.mayWriteToMemory()) {
            facts.clear();
            ++stats.aliasBarriers;
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
                *const_cast<llvm::StoreInst *>(store), memoryBases,
                module.getDataLayout()).has_value();
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
    // Run only after all original loop/incoming-fact decisions and pending
    // rewrites.  These local facts must never become a new loop invariant or
    // an incoming fact for a block already processed by the ordinary worklist.
    if (stats.loadsForwarded < options.maxRewrites)
      forwardConservativeBlocks(function, reachable, visited, memoryBases,
                                module.getDataLayout(), options, stats);
    if (stats.loadsForwarded < options.maxRewrites)
      runByteForwarding(function, memoryBases, module.getDataLayout(), options,
                        stats, graph, leafCallCache);
    if (stats.loadsForwarded < options.maxRewrites) {
      std::vector<llvm::LoadInst *> pendingHubs;
      forwardCheckedMemoryEdges(function, memoryBases, module.getDataLayout(),
                                options, stats, checkedEdgeBudget, pendingHubs);
      if (!pendingHubs.empty())
        pendingCheckedHubs.push_back({&function, memoryBases, std::move(pendingHubs)});
    }
    if (stats.loadsForwarded >= options.maxRewrites) {
      stats.complete = false;
      ++stats.budgetExhausted;
      rewriteBudgetReported = true;
      break;
    }
  }
  // New multi-join searches share the original module budgets, but cannot
  // spend them before a later function's historical direct-store paths.
  // Every queued LOAD was captured once after that function's ordinary and
  // byte rewrites; only cloned LOADs can be removed by the CFG edge phases.
  for (const auto &pending : pendingCheckedHubs) {
    if (checkedEdgeBudget.stopped || stats.loadsForwarded >= options.maxRewrites)
      break;
    forwardCheckedMemoryHubs(*pending.function, pending.memoryBases,
                            module.getDataLayout(), options, stats,
                            checkedEdgeBudget, pending.loads);
  }
  if (!rewriteBudgetReported && stats.loadsForwarded >= options.maxRewrites) {
    stats.complete = false;
    ++stats.budgetExhausted;
  }
  return stats;
}

} // namespace dragon::llvmlifter::detail
