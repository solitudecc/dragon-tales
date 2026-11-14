#include "LLVMPredicateFolding.hpp"

#ifdef DRAGON_WITH_Z3
#include <z3++.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {
struct OperandRange {
  llvm::APInt unsignedMin, unsignedMax, signedMin, signedMax;
  explicit OperandRange(unsigned width)
      : unsignedMin(width, 0), unsignedMax(llvm::APInt::getMaxValue(width)),
        signedMin(llvm::APInt::getSignedMinValue(width)),
        signedMax(llvm::APInt::getSignedMaxValue(width)) {}
};

// Operand-only, context-free bounds. Do not consult the operation's result,
// its flags, PHI backedges, path conditions or assumed memory contents.
OperandRange operandRange(llvm::Value *value) {
  const unsigned width = value->getType()->getIntegerBitWidth();
  OperandRange range(width);
  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    range.unsignedMin = range.unsignedMax = range.signedMin = range.signedMax =
        constant->getValue();
  } else if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
             binary && binary->getOpcode() == llvm::Instruction::And) {
    auto *mask = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(0));
    if (!mask) mask = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
    if (mask) {
      range.unsignedMax = mask->getValue();
      if (!mask->getValue().isNegative()) range.signedMin = llvm::APInt(width, 0);
      range.signedMax = mask->getValue() & llvm::APInt::getSignedMaxValue(width);
    }
  } else if (auto *extend = llvm::dyn_cast<llvm::ZExtInst>(value)) {
    range.unsignedMax = llvm::APInt::getLowBitsSet(
        width, extend->getSrcTy()->getIntegerBitWidth());
    range.signedMin = llvm::APInt(width, 0);
    range.signedMax = range.unsignedMax;
  }
  return range;
}

// Called only after both operands have been completely lowered. These bounds
// show that the flags add no overflow poison for defined operands; they do not
// assert that the operands themselves are defined or publish any LLVM fact.
bool safeAddSubFlags(llvm::BinaryOperator &operation) {
  const auto left = operandRange(operation.getOperand(0));
  const auto right = operandRange(operation.getOperand(1));
  const bool add = operation.getOpcode() == llvm::Instruction::Add;
  const unsigned width = operation.getType()->getIntegerBitWidth();
  if (operation.hasNoUnsignedWrap()) {
    if (add) {
      if ((left.unsignedMax.zext(width + 1) + right.unsignedMax.zext(width + 1))
              .ugt(llvm::APInt::getMaxValue(width).zext(width + 1))) return false;
    } else if (left.unsignedMin.ult(right.unsignedMax)) return false;
  }
  if (operation.hasNoSignedWrap()) {
    const auto minimum = add
        ? left.signedMin.sext(width + 1) + right.signedMin.sext(width + 1)
        : left.signedMin.sext(width + 1) - right.signedMax.sext(width + 1);
    const auto maximum = add
        ? left.signedMax.sext(width + 1) + right.signedMax.sext(width + 1)
        : left.signedMax.sext(width + 1) - right.signedMin.sext(width + 1);
    if (minimum.slt(llvm::APInt::getSignedMinValue(width).sext(width + 1)) ||
        maximum.sgt(llvm::APInt::getSignedMaxValue(width).sext(width + 1))) return false;
  }
  return true;
}

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
    if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(instruction)) {
      const auto id = intrinsic->getIntrinsicID();
      auto *callee = intrinsic->getCalledFunction();
      if ((id != llvm::Intrinsic::fshl && id != llvm::Intrinsic::fshr) ||
          intrinsic->arg_size() != 3 || intrinsic->getNumOperandBundles() ||
          intrinsic->isConvergent() || intrinsic->mayReadOrWriteMemory() ||
          intrinsic->mayHaveSideEffects() || !callee || !callee->isDeclaration() ||
          callee->isConvergent() || callee->isVarArg() ||
          callee->getFunctionType() != intrinsic->getFunctionType()) return std::nullopt;
      // Validate every argument before making an observation boundary. This
      // preserves nested poison/unsupported/flag and existing budget refusals.
      for (auto &argument : intrinsic->args())
        if (argument->getType() != type || !lower(argument.get(), depth + 1))
          return std::nullopt;
      // Every defined result is contained in this unconstrained SSA input.
      // Distinct calls stay distinct; do not infer a value, count or noundef.
      return input(value, type->getBitWidth());
    }
    // Division, FP, ordinary calls, flagged multiplication/left shift and
    // variable/out-of-range shifts remain unsupported.
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction)) {
      switch (binary->getOpcode()) {
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
    if (auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(instruction);
        operation && (opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub) &&
        (operation->hasNoSignedWrap() || operation->hasNoUnsignedWrap()) &&
        !safeAddSubFlags(*operation)) return std::nullopt;
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
  std::size_t nodes() const { return nodes_; }
  const std::vector<z3::expr> &inputs() const { return inputs_; }
private:
  z3::expr remember(llvm::Value *value, z3::expr expression) {
    cache_.emplace(value, expression); return expression;
  }
  z3::expr input(llvm::Value *value, unsigned width) {
    const auto name = "input_" + std::to_string(cache_.size());
    auto expression = context_.bv_const(name.c_str(), width);
    inputs_.push_back(expression);
    return remember(value, expression);
  }
  z3::context &context_;
  std::size_t limit_;
  std::size_t nodes_ = 0;
  std::uint64_t &remaining_;
  std::unordered_map<llvm::Value *, z3::expr> cache_;
  std::vector<z3::expr> inputs_;
};

struct SchedulingWork {
  const std::uint64_t cap;
  std::uint64_t used = 0, evaluations = 0, exhausted = 0, exceptions = 0;
  std::size_t observedDeferred = 0, proofConflicts = 0;
  explicit SchedulingWork(std::uint64_t originalWork)
      : cap(std::min<std::uint64_t>(originalWork, 1048576)) {}
  bool charge(std::uint64_t cost) {
    if (cost > cap - used) { ++exhausted; return false; }
    used += cost; return true;
  }
};

// This can establish only nonconstancy of the same overapproximating formula
// sent to SMT. All its SSA inputs receive complete, width-correct numeric
// assignments. Failing to find both bits proves nothing and keeps legacy SMT.
bool twoNumericWitnesses(z3::context &context, const z3::expr &expression,
                         const IntegerExpression &lowerer, SchedulingWork &work) {
  if (lowerer.inputs().empty()) return false;
  const std::uint64_t cost = 1 + std::uint64_t{lowerer.nodes()} + lowerer.inputs().size();
  try {
    bool observed[2] = {false, false};
    for (unsigned pattern = 0; pattern < 8; ++pattern) {
      if (!work.charge(cost)) return false;
      z3::expr_vector inputs(context), assignment(context);
      for (unsigned i = 0; i < lowerer.inputs().size(); ++i) {
        const auto &input = lowerer.inputs()[i];
        inputs.push_back(input);
        const unsigned width = input.get_sort().bv_size();
        llvm::APInt value(width, 0);
        switch (pattern) {
          case 1: value = llvm::APInt::getAllOnes(width); break;
          case 2: value = llvm::APInt(width, 1); break;
          case 3: value = llvm::APInt::getSignedMinValue(width); break;
          case 4: value = llvm::APInt::getSignedMaxValue(width); break;
          case 5: if (i & 1) value = llvm::APInt::getAllOnes(width); break;
          case 6: if (!(i & 1)) value = llvm::APInt::getAllOnes(width); break;
          case 7: value = llvm::APInt(64, std::uint64_t{i} + 2).zextOrTrunc(width); break;
          default: break;
        }
        llvm::SmallString<48> digits; value.toStringUnsigned(digits);
        assignment.push_back(context.bv_val(digits.c_str(), width));
      }
      auto grounded = expression;
      grounded = grounded.substitute(inputs, assignment).simplify();
      if (!grounded.is_numeral()) return false;
      const auto bit = grounded.get_numeral_uint64();
      if (bit > 1) return false;
      ++work.evaluations; observed[bit] = true;
      if (observed[0] && observed[1]) return true;
    }
  } catch (const z3::exception &) {
    ++work.exceptions; // Scheduling failure is not a solver proof/unknown.
  }
  return false;
}
} // namespace

PredicateFoldStats foldPredicates(llvm::Module &module, const PredicateFoldOptions &options) {
  PredicateFoldStats stats; stats.available = true;
  z3::context context;
  std::uint64_t remaining = std::uint64_t{options.maxQueries} * options.maxExpressionNodes;
  // Optional scheduling work is independently bounded by the original work
  // allowance. It never spends/reset lowerer's remaining or adds SMT queries.
  SchedulingWork scheduling(remaining);
  struct Deferred {
    llvm::Value *condition;
    z3::expr expression;
    std::size_t consumers = 1;
  };
  const std::size_t queueLimit = std::min<std::uint32_t>(options.maxQueries, 4096);
  std::vector<Deferred> deferred;
  std::unordered_map<llvm::Value *, std::size_t> waiting;
  std::unordered_map<llvm::Value *, std::optional<bool>> proven;
  auto solve = [&](const z3::expr &expression) -> std::optional<bool> {
    z3::solver solver{context};
    solver.set("timeout", options.timeoutMs);
    solver.set("rlimit", options.resourceLimit);
    for (bool candidate : {false, true}) {
      if (stats.solverQueries >= options.maxQueries) { ++stats.budgetSkipped; break; }
      solver.push();
      solver.add(expression != context.bv_val(candidate ? 1 : 0, 1));
      ++stats.solverQueries;
      const auto result = solver.check();
      solver.pop();
      if (result == z3::unsat) return candidate;
      if (result == z3::unknown) ++stats.unknownQueries;
    }
    return std::nullopt;
  };
  for (auto &function : module) for (auto &block : function) for (auto &instruction : block) {
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(&instruction);
    auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction);
    if ((!branch || !branch->isConditional()) && !select) continue;
    auto *condition = branch ? branch->getCondition() : select->getCondition();
    if (!condition->getType()->isIntegerTy(1)) continue;
    ++stats.conditionsVisited;
    if (auto it = waiting.find(condition); it != waiting.end()) {
      ++deferred[it->second].consumers; continue;
    }
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
        else if (deferred.size() < queueLimit &&
                 twoNumericWitnesses(context, *expression, lowerer, scheduling)) {
          waiting.emplace(condition, deferred.size());
          deferred.push_back({condition, *expression, 1});
          ++scheduling.observedDeferred;
          continue;
        } else answer = solve(*expression);
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
  // Potentially constant conditions have had their normal proof attempts.
  // Deferred formulas were already lowered inside the original node budget;
  // process them in first-consumer order using only the remaining SMT budget.
  for (const auto &item : deferred) {
    if (stats.solverQueries >= options.maxQueries) {
      // Legacy's pre-query gate does not cache a result: each consumer would
      // be skipped. A partial two-candidate attempt below instead counts its
      // one inner budget skip and shares the resulting unresolved cache.
      stats.budgetSkipped += item.consumers;
      continue;
    }
    try {
      auto answer = solve(item.expression);
      // Two concrete opposite results already preclude a constant proof.
      // Never publish a constant from this scheduling pool, even if a solver
      // were to return a result contradicting those numeric witnesses.
      if (answer) ++scheduling.proofConflicts;
      proven.emplace(item.condition, std::nullopt);
    } catch (const z3::exception &) { ++stats.unknownQueries; }
  }
  // Diagnostic copies report scheduling.cap/used/evaluations/observedDeferred/
  // exhausted/exceptions/proofConflicts here; public statistics/ABI stay fixed.
  return stats;
}
} // namespace dragon::llvmlifter::detail
#else
namespace dragon::llvmlifter::detail {
PredicateFoldStats foldPredicates(llvm::Module &, const PredicateFoldOptions &) { return {}; }
}
#endif
