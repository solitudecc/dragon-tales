#include "LLVMPredicateFolding.hpp"

#ifdef DRAGON_WITH_Z3
#include <z3++.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>

namespace dragon::llvmlifter::detail {
namespace {
// Each candidate is proved without path assumptions. Opaque SSA inputs are
// distinct by Value identity, never by register name, address or load pointer.
// This is an overapproximation: in particular two memory reads may differ.
class IntegerExpression {
public:
  IntegerExpression(z3::context &context, std::size_t limit,
                    std::uint64_t &remaining)
      : context_(context), limit_(limit), remaining_(remaining) {}

  std::optional<z3::expr> lower(llvm::Value *value, unsigned depth = 0) {
    if (auto it = cache_.find(value); it != cache_.end()) return it->second;
    if (depth >= 64 || nodes_ >= limit_ || remaining_ == 0) {
      limited = true; return std::nullopt;
    }
    --remaining_; ++nodes_;
    auto *type = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (!type || type->getBitWidth() > 128) return std::nullopt;
    // Do not manufacture identities from explicit missing/poisoned semantics.
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value)) return std::nullopt;
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      llvm::SmallString<48> digits;
      constant->getValue().toStringUnsigned(digits);
      return remember(value, context_.bv_val(digits.c_str(), type->getBitWidth()));
    }
    if (llvm::isa<llvm::Argument, llvm::PHINode, llvm::FreezeInst>(value))
      return input(value, type->getBitWidth());
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
      if (load->isVolatile() || load->isAtomic()) return std::nullopt;
      return input(value, type->getBitWidth());
    }
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction) return std::nullopt;
    // The first version deliberately excludes division, FP, calls, overflow
    // flags and variable/out-of-range shifts instead of guessing their domain.
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction)) {
      switch (binary->getOpcode()) {
        case llvm::Instruction::Add: case llvm::Instruction::Sub:
        case llvm::Instruction::Mul: case llvm::Instruction::Shl:
          if (binary->hasNoSignedWrap() || binary->hasNoUnsignedWrap()) return std::nullopt;
          break;
        default: break;
      }
    }
    const auto opcode = instruction->getOpcode();
    const bool binary = opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
        opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::And ||
        opcode == llvm::Instruction::Or || opcode == llvm::Instruction::Xor ||
        opcode == llvm::Instruction::Shl || opcode == llvm::Instruction::LShr ||
        opcode == llvm::Instruction::AShr || opcode == llvm::Instruction::ICmp;
    const bool cast = opcode == llvm::Instruction::ZExt || opcode == llvm::Instruction::SExt ||
        opcode == llvm::Instruction::Trunc;
    if (!binary && !cast && opcode != llvm::Instruction::Select) return std::nullopt;
    if (opcode == llvm::Instruction::Shl || opcode == llvm::Instruction::LShr ||
        opcode == llvm::Instruction::AShr) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(instruction->getOperand(1));
      if (!amount || amount->getValue().uge(type->getBitWidth())) return std::nullopt;
      if (auto *operation = llvm::dyn_cast<llvm::PossiblyExactOperator>(instruction);
          operation && operation->isExact()) return std::nullopt;
    }
    auto left = lower(instruction->getOperand(0), depth + 1);
    if (!left) return std::nullopt;
    const auto width = type->getBitWidth();
    if (cast) {
      const auto oldWidth = left->get_sort().bv_size();
      if (opcode == llvm::Instruction::Trunc)
        return remember(value, left->extract(width - 1, 0));
      return remember(value, opcode == llvm::Instruction::SExt
          ? z3::sext(*left, width - oldWidth) : z3::zext(*left, width - oldWidth));
    }
    auto right = lower(instruction->getOperand(1), depth + 1);
    if (!right) return std::nullopt;
    switch (opcode) {
      case llvm::Instruction::Add: return remember(value, *left + *right);
      case llvm::Instruction::Sub: return remember(value, *left - *right);
      case llvm::Instruction::Mul: return remember(value, *left * *right);
      case llvm::Instruction::And: return remember(value, *left & *right);
      case llvm::Instruction::Or: return remember(value, *left | *right);
      case llvm::Instruction::Xor: return remember(value, *left ^ *right);
      case llvm::Instruction::Shl: return remember(value, z3::shl(*left, *right));
      case llvm::Instruction::LShr: return remember(value, z3::lshr(*left, *right));
      case llvm::Instruction::AShr: return remember(value, z3::ashr(*left, *right));
      case llvm::Instruction::Select: {
        auto alternative = lower(instruction->getOperand(2), depth + 1);
        if (!alternative) return std::nullopt;
        return remember(value, z3::ite(*left != context_.bv_val(0, 1), *right, *alternative));
      }
      default: break;
    }
    auto *compare = llvm::cast<llvm::ICmpInst>(instruction);
    auto condition = [&]() -> z3::expr {
      switch (compare->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ: return *left == *right;
        case llvm::CmpInst::ICMP_NE: return *left != *right;
        case llvm::CmpInst::ICMP_ULT: return z3::ult(*left, *right);
        case llvm::CmpInst::ICMP_ULE: return z3::ule(*left, *right);
        case llvm::CmpInst::ICMP_UGT: return z3::ugt(*left, *right);
        case llvm::CmpInst::ICMP_UGE: return z3::uge(*left, *right);
        case llvm::CmpInst::ICMP_SLT: return *left < *right;
        case llvm::CmpInst::ICMP_SLE: return *left <= *right;
        case llvm::CmpInst::ICMP_SGT: return *left > *right;
        default: return *left >= *right;
      }
    }();
    return remember(value, z3::ite(condition, context_.bv_val(1, 1), context_.bv_val(0, 1)));
  }

  bool limited = false;
private:
  z3::expr remember(llvm::Value *value, z3::expr expression) {
    cache_.emplace(value, expression); return expression;
  }
  z3::expr input(llvm::Value *value, unsigned width) {
    const auto name = "input_" + std::to_string(cache_.size());
    return remember(value, context_.bv_const(name.c_str(), width));
  }
  z3::context &context_;
  std::size_t limit_;
  std::size_t nodes_ = 0;
  std::uint64_t &remaining_;
  std::unordered_map<llvm::Value *, z3::expr> cache_;
};
} // namespace

PredicateFoldStats foldPredicates(llvm::Module &module, const PredicateFoldOptions &options) {
  PredicateFoldStats stats; stats.available = true;
  z3::context context;
  std::uint64_t remaining = std::uint64_t{options.maxQueries} * options.maxExpressionNodes;
  std::unordered_map<llvm::Value *, std::optional<bool>> proven;
  for (auto &function : module) for (auto &block : function) for (auto &instruction : block) {
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(&instruction);
    auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction);
    if ((!branch || !branch->isConditional()) && !select) continue;
    auto *condition = branch ? branch->getCondition() : select->getCondition();
    if (!condition->getType()->isIntegerTy(1)) continue;
    ++stats.conditionsVisited;
    std::optional<bool> answer;
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition)) answer = !constant->isZero();
    else if (auto it = proven.find(condition); it != proven.end()) answer = it->second;
    else if (options.timeoutMs == 0 || stats.solverQueries >= options.maxQueries || remaining == 0) {
      ++stats.budgetSkipped; continue;
    } else {
      try {
        IntegerExpression lowerer{context, options.maxExpressionNodes, remaining};
        auto expression = lowerer.lower(condition);
        if (!expression) {
          if (lowerer.limited) ++stats.budgetSkipped;
          else ++stats.unsupportedConditions;
          proven.emplace(condition, std::nullopt); continue;
        }
        auto simplified = expression->simplify();
        if (simplified.is_numeral()) answer = simplified.get_numeral_uint64() != 0;
        else {
          z3::solver solver{context};
          solver.set("timeout", options.timeoutMs);
          solver.set("rlimit", options.resourceLimit);
          for (bool candidate : {false, true}) {
            if (stats.solverQueries >= options.maxQueries) { ++stats.budgetSkipped; break; }
            solver.push();
            solver.add(*expression != context.bv_val(candidate ? 1 : 0, 1));
            ++stats.solverQueries;
            const auto result = solver.check();
            solver.pop();
            if (result == z3::unsat) { answer = candidate; break; }
            if (result == z3::unknown) ++stats.unknownQueries;
          }
        }
        proven.emplace(condition, answer);
      } catch (const z3::exception &) {
        ++stats.unknownQueries; // A failed proof never changes a condition.
      }
    }
    if (!answer) continue;
    auto *constant = llvm::ConstantInt::get(condition->getType(), *answer);
    if (branch) branch->setCondition(constant);
    else select->setCondition(constant);
    ++stats.conditionsFolded;
  }
  return stats;
}
} // namespace dragon::llvmlifter::detail
#else
namespace dragon::llvmlifter::detail {
PredicateFoldStats foldPredicates(llvm::Module &, const PredicateFoldOptions &) { return {}; }
}
#endif
