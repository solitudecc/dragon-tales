#include "LLVMPathValueRanges.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <map>
#include <set>

using namespace llvm;
namespace dragon::llvmlifter::detail {

struct PathValueRanges::Impl {
  struct Fact {
    Value *value;
    BasicBlock *source, *target;
    PathValueRange range;
  };
  Function &function;
  DominatorTree dominance;
  std::set<Value *> stable;
  std::vector<Fact> facts;
  std::size_t limit, spent = 0;
  bool exhausted = false, valid = true;
  std::map<std::pair<Value *, BasicBlock *>, std::optional<PathValueRange>> cache;

  bool spend(std::size_t n = 1) {
    if (n > limit - spent) { exhausted = true; valid = false; return false; }
    spent += n; return true;
  }
  static std::uint64_t mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (std::uint64_t(1) << bits) - 1;
  }

  // A one-edge history bridge names the same observation only when that
  // observation already dominates the bridge. A loop-carried input must not
  // be read as the current iteration's value.
  Value *observation(Value *value) {
    std::set<Value *> seen;
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (!spend() || !seen.insert(value).second) return nullptr;
      auto *phi = dyn_cast<PHINode>(value);
      if (!phi) return value;
      if (phi->getNumIncomingValues() != 1 || pred_size(phi->getParent()) != 1)
        return value;
      auto *input = phi->getIncomingValue(0);
      if (auto *instruction = dyn_cast<Instruction>(input)) {
        if (instruction->getFunction() != &function ||
            !dominance.dominates(instruction, phi)) return value;
      }
      value = input;
    }
    exhausted = true; valid = false; return nullptr;
  }

  std::optional<PathValueRange> comparison(ICmpInst::Predicate predicate,
                                          ConstantInt *constant, bool truth) {
    const auto bits = constant->getBitWidth();
    const auto c = constant->getZExtValue(), top = mask(bits);
    if (!truth) predicate = ICmpInst::getInversePredicate(predicate);
    switch (predicate) {
    case ICmpInst::ICMP_EQ: return PathValueRange{c, c, bits};
    case ICmpInst::ICMP_NE:
      if (c == 0 && top) return PathValueRange{1, top, bits};
      if (c == top && top) return PathValueRange{0, top-1, bits};
      return {};
    case ICmpInst::ICMP_ULT:
      if (c) return PathValueRange{0, c-1, bits};
      return {};
    case ICmpInst::ICMP_ULE: return PathValueRange{0, c, bits};
    case ICmpInst::ICMP_UGT:
      if (c != top) return PathValueRange{c+1, top, bits};
      return {};
    case ICmpInst::ICMP_UGE: return PathValueRange{c, top, bits};
    default: return {};
    }
  }

  void decode(Value *raw, bool truth, BranchInst *branch, BasicBlock *target,
              unsigned depth, unsigned &nodes) {
    if (!valid || !spend()) return;
    if (depth > 32 || ++nodes > 256) {
      exhausted = true; valid = false; return;
    }
    auto *value = observation(raw);
    if (!value) return;
    if (auto *binary = dyn_cast<BinaryOperator>(value)) {
      if (binary->hasPoisonGeneratingAnnotations() || !binary->getType()->isIntegerTy(1)) return;
      if ((binary->getOpcode() == Instruction::And && truth) ||
          (binary->getOpcode() == Instruction::Or && !truth)) {
        decode(binary->getOperand(0), truth, branch, target, depth+1, nodes);
        decode(binary->getOperand(1), truth, branch, target, depth+1, nodes);
      } else if (binary->getOpcode() == Instruction::Xor) {
        auto *c = dyn_cast<ConstantInt>(binary->getOperand(1));
        if (!c) { c = dyn_cast<ConstantInt>(binary->getOperand(0));
          if (c) decode(binary->getOperand(1), truth != c->isOne(), branch, target, depth+1, nodes);
        } else decode(binary->getOperand(0), truth != c->isOne(), branch, target, depth+1, nodes);
      }
      return;
    }
    auto *compare = dyn_cast<ICmpInst>(value);
    if (!compare || compare->hasPoisonGeneratingAnnotations()) return;
    auto predicate = compare->getPredicate();
    auto *left = observation(compare->getOperand(0));
    auto *right = observation(compare->getOperand(1));
    if (!left || !right) return;
    auto *constant = dyn_cast<ConstantInt>(right);
    if (!constant) {
      constant = dyn_cast<ConstantInt>(left);
      if (!constant) return;
      left = right; predicate = ICmpInst::getSwappedPredicate(predicate);
    }
    if (constant->getBitWidth() > 64) return;
    if (stable.contains(left)) {
      auto *definition = dyn_cast<Instruction>(left);
      if (!definition || !dominance.dominates(definition, branch)) return;
      if (auto range = comparison(predicate, constant, truth)) {
        if (facts.size() == 8192) { exhausted = true; valid = false; return; }
        facts.push_back({left, branch->getParent(), target, *range});
      }
      return;
    }
    if (predicate != ICmpInst::ICMP_EQ && predicate != ICmpInst::ICMP_NE) return;
    auto outcome = [&](ConstantInt *candidate) {
      bool equal = candidate->getValue() == constant->getValue();
      return predicate == ICmpInst::ICMP_EQ ? equal : !equal;
    };
    if (auto *select = dyn_cast<SelectInst>(left)) {
      if (select->hasPoisonGeneratingAnnotations()) return;
      auto *a = dyn_cast<ConstantInt>(select->getTrueValue());
      auto *b = dyn_cast<ConstantInt>(select->getFalseValue());
      if (a && b && outcome(a) != outcome(b))
        decode(select->getCondition(), outcome(a) == truth, branch, target, depth+1, nodes);
    } else if (auto *cast = dyn_cast<CastInst>(left)) {
      if (!cast->hasPoisonGeneratingAnnotations() &&
          (cast->getOpcode() == Instruction::ZExt || cast->getOpcode() == Instruction::SExt) &&
          cast->getOperand(0)->getType()->isIntegerTy(1)) {
        auto width = constant->getBitWidth();
        auto *zero = ConstantInt::get(cast->getContext(), APInt(width, 0));
        auto *one = ConstantInt::get(cast->getContext(), cast->getOpcode() == Instruction::SExt
              ? APInt::getAllOnes(width) : APInt(width, 1));
        if (outcome(zero) != outcome(one))
          decode(cast->getOperand(0), outcome(one) == truth, branch, target, depth+1, nodes);
      }
    }
  }

  Impl(Function &f, const std::vector<Value *> &witnesses, std::size_t maxWork)
      : function(f), dominance(f), limit(std::min<std::size_t>(maxWork, 65536)) {
    if (witnesses.size() > 256 || f.size() > 4096) {
      exhausted = true; valid = false; return;
    }
    for (auto *value : witnesses) {
      if (!spend()) return;
      auto *instruction = dyn_cast_or_null<Instruction>(value);
      if (!instruction || instruction->getFunction() != &f ||
          !value->getType()->isIntegerTy() || value->getType()->getIntegerBitWidth() > 64 ||
          !dominance.isReachableFromEntry(instruction->getParent())) { valid = false; return; }
      stable.insert(value);
    }
    if (stable.empty()) return;
    for (auto &block : f) {
      if (!spend(block.size()+1)) return;
      auto *branch = dyn_cast<BranchInst>(block.getTerminator());
      if (!branch || !branch->isConditional() || branch->getSuccessor(0) == branch->getSuccessor(1) ||
          !dominance.isReachableFromEntry(&block)) continue;
      for (unsigned slot = 0; slot < 2; ++slot) {
        unsigned nodes = 0;
        decode(branch->getCondition(), slot == 0, branch, branch->getSuccessor(slot), 0, nodes);
        if (!valid) return;
      }
    }
  }

  std::optional<PathValueRange> get(Value *value, Instruction *at) {
    if (!valid || !at || at->getFunction() != &function || !spend() ||
        !dominance.isReachableFromEntry(at->getParent())) return {};
    auto *observed = observation(value);
    if (!observed || !stable.contains(observed)) return {};
    auto *definition = cast<Instruction>(observed);
    if (!dominance.dominates(definition, at)) return {};
    auto key = std::pair(observed, at->getParent());
    if (auto found = cache.find(key); found != cache.end()) return found->second;
    std::optional<PathValueRange> result;
    for (const auto &fact : facts) {
      if (!spend()) return {};
      if (fact.value != observed) continue;
      BasicBlockEdge edge(fact.source, fact.target);
      if (!edge.isSingleEdge() || !dominance.dominates(edge, at->getParent())) continue;
      if (!result) result = fact.range;
      else {
        result->low = std::max(result->low, fact.range.low);
        result->high = std::min(result->high, fact.range.high);
        if (result->low > result->high) { result.reset(); break; }
      }
    }
    cache.emplace(key, result); return result;
  }
};

PathValueRanges::PathValueRanges(Function &f, const std::vector<Value *> &witnesses,
                                 std::size_t maxWork)
    : impl(std::make_unique<Impl>(f, witnesses, maxWork)) {}
PathValueRanges::~PathValueRanges() = default;
std::optional<PathValueRange> PathValueRanges::get(Value *v, Instruction *at,
                                                std::size_t availableWork) {
  // The enclosing proof may have spent work since construction or the last
  // query. Refuse before traversal crosses its CURRENT remaining allowance.
  const auto original=impl->limit;
  impl->limit=impl->spent+std::min(availableWork,original-impl->spent);
  auto result=impl->get(v,at);
  impl->limit=original;
  return result;
}
std::size_t PathValueRanges::work() const { return impl->spent; }
bool PathValueRanges::budgetExhausted() const { return impl->exhausted; }

} // namespace dragon::llvmlifter::detail
