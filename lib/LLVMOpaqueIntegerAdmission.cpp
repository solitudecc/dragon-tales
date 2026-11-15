#include "LLVMOpaqueIntegerAdmission.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <algorithm>
#include <optional>
#include <set>
#include <vector>

namespace dragon::llvmlifter::detail {

OpaqueIntegerAdmissionResult OpaqueIntegerAdmission::get(
    llvm::Value *root, std::size_t maxNodes, std::size_t maxWork) {
  OpaqueIntegerAdmissionResult result;
  const auto nodeLimit = std::min<std::size_t>(maxNodes, 128);
  const auto workLimit = std::min<std::size_t>(maxWork, 1000000);
  auto spend = [&]() {
    if (result.work == workLimit) {
      result.budgetExhausted = true;
      result.message = "opaque integer admission work cap";
      return false;
    }
    ++result.work;
    return true;
  };
  auto refuse = [&](const char *why) { result.message = why; return result; };
  if (!nodeLimit) {
    result.budgetExhausted = true;
    return refuse("opaque integer admission has no node allowance");
  }
  if (!spend()) return result;
  if (auto cached = admittedRoots_.find(root); cached != admittedRoots_.end()) {
    result.uniqueNodes = cached->second;
    if (cached->second > nodeLimit) {
      result.budgetExhausted = true;
      return refuse("cached opaque integer footprint exceeds current node allowance");
    }
    result.admitted = true;
    return refuse("cached complete opaque integer shape; Full only");
  }
  std::set<llvm::Value *> seen;
  std::vector<llvm::Value *> pending;
  auto enqueue = [&](llvm::Value *value) {
    if (!spend()) return false;
    if (seen.contains(value)) return true;
    if (seen.size() == nodeLimit) {
      result.budgetExhausted = true;
      result.message = "opaque integer admission node cap";
      return false;
    }
    seen.insert(value); pending.push_back(value); result.uniqueNodes = seen.size();
    return true;
  };
  if (!enqueue(root)) return result;
  for (std::size_t n = 0; n < pending.size(); ++n) {
    if (!spend()) return result;
    auto *value = pending[n];
    if (!value || !value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() > 64 ||
        llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value))
      return refuse("opaque dependency is not an admitted scalar integer");
    if (auto *argument = llvm::dyn_cast<llvm::Argument>(value)) {
      if (argument->getParent() != &function_) return refuse("foreign integer argument");
      continue;
    }
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction) continue; // ConstantInt and other non-Instruction integers.
    if (!instruction->getParent() || instruction->getFunction() != &function_)
      return refuse("foreign or detached integer instruction");
    if (llvm::isa<llvm::FreezeInst>(instruction)) continue; // New opaque observation.
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
      if (!load->isSimple()) return refuse("observable integer LOAD");
      continue; // No address/memory traversal and no LOAD value/definedness fact.
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction)) {
      std::map<llvm::BasicBlock *, std::size_t> slots;
      for (auto *pred : llvm::predecessors(phi->getParent())) {
        if (!spend()) return result;
        ++slots[pred];
      }
      if (slots.empty()) return refuse("PHI has no actual defining predecessor");
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (!spend()) return result;
        auto *pred = phi->getIncomingBlock(i);
        auto found = slots.find(pred);
        if (!pred || pred->getParent() != &function_ || found == slots.end() ||
            phi->getIncomingValue(i)->getType() != phi->getType())
          return refuse("PHI does not cover actual same-typed predecessor slots");
        if (!--found->second) slots.erase(found);
        if (!enqueue(phi->getIncomingValue(i))) return result;
      }
      if (!slots.empty()) return refuse("PHI omits an actual predecessor slot");
      continue;
    }
    if (auto *cast = llvm::dyn_cast<llvm::CastInst>(instruction)) {
      if (cast->getOpcode() != llvm::Instruction::Trunc &&
          cast->getOpcode() != llvm::Instruction::ZExt &&
          cast->getOpcode() != llvm::Instruction::SExt)
        return refuse("unsupported opaque integer cast");
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(instruction)) {
      auto *callee = call->getCalledFunction();
      if (call->isTerminator() || !callee ||
          (callee->getIntrinsicID() != llvm::Intrinsic::fshl &&
                      callee->getIntrinsicID() != llvm::Intrinsic::fshr) ||
          !call->doesNotAccessMemory() || call->isConvergent() ||
          call->hasFnAttr(llvm::Attribute::NoDuplicate) || call->getNumOperandBundles() ||
          call->arg_size() != 3)
        return refuse("call is not an admitted mathematical funnel intrinsic");
      for (auto &argument : call->args()) {
        if (argument->getType() != call->getType())
          return refuse("noncanonical funnel intrinsic type");
        if (!enqueue(argument.get())) return result;
      }
      continue;
    }
    if (instruction->isTerminator() || instruction->mayReadOrWriteMemory() ||
        instruction->mayHaveSideEffects())
      return refuse("nonpure integer dependency");
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction)) {
      const auto opcode = binary->getOpcode();
      if (opcode == llvm::Instruction::Shl || opcode == llvm::Instruction::LShr ||
          opcode == llvm::Instruction::AShr) {
        auto *countValue = binary->getOperand(1);
        auto *count = llvm::dyn_cast<llvm::ConstantInt>(countValue);
        if (!count) {
          if (!spend()) return result; // Point-independent exact SSA H lookup.
          auto found = hypotheses_.find(countValue);
          if (found != hypotheses_.end()) count = found->second;
        }
        if (!count || count->getType() != countValue->getType() ||
            count->getBitWidth() > 64 ||
            count->getZExtValue() >= binary->getType()->getIntegerBitWidth())
          return refuse("shift count is not a legal literal or exact singleton hypothesis");
      }
    }
    // Select's condition and BOTH arms, all integer operands of binary/cast,
    // and the old pure-instruction default are all traversed. No flag becomes
    // a no-overflow/noundef fact. Explicit bad operands cannot hide behind H.
    for (auto &operand : instruction->operands()) {
      if (!spend()) return result;
      if (operand->getType()->isIntegerTy() && !enqueue(operand.get())) return result;
    }
  }
  if (!spend()) return result; // Entire-root cache publication is prepaid.
  admittedRoots_.emplace(root, result.uniqueNodes);
  result.admitted = true;
  return refuse("complete opaque integer dependency shape; Full only");
}

// Explicit bounded structural compression; the original get() stays unchanged.
OpaqueIntegerAdmissionResult OpaqueIntegerAdmission::getPhiRegions(
    llvm::Value *root, std::size_t maxNodes, std::size_t maxWork) {
  OpaqueIntegerAdmissionResult result;
  const auto nodeLimit = std::min<std::size_t>(maxNodes, 128);
  const auto workLimit = std::min<std::size_t>(maxWork, 1000000);
  auto spend = [&]() {
    if (result.work == workLimit) {
      result.budgetExhausted = true;
      result.message = "opaque integer admission work cap";
      return false;
    }
    ++result.work;
    return true;
  };
  auto refuse = [&](const char *why) { result.message = why; return result; };
  if (!nodeLimit) {
    result.budgetExhausted = true;
    return refuse("opaque integer admission has no node allowance");
  }
  if (!spend()) return result;
  if (auto cached = admittedRegionRoots_.find(root); cached != admittedRegionRoots_.end()) {
    result.uniqueNodes = cached->second;
    if (cached->second > nodeLimit) {
      result.budgetExhausted = true;
      return refuse("cached opaque integer footprint exceeds current node allowance");
    }
    result.admitted = true;
    return refuse("cached complete opaque Phi-region shape; Full only");
  }
  std::map<llvm::Value *, std::size_t> seen;
  std::vector<std::pair<llvm::PHINode *, llvm::PHINode *>> phiEdges;
  std::vector<llvm::Value *> pending;
  auto enqueue = [&](llvm::Value *value) {
    if (!spend()) return false;
    if (seen.contains(value)) return true;
    if (seen.size() == 65536) {
      result.budgetExhausted = true;
      result.message = "opaque Phi-region physical graph cap";
      return false;
    }
    seen.emplace(value, pending.size()); pending.push_back(value);
    return true;
  };
  if (!enqueue(root)) return result;
  for (std::size_t n = 0; n < pending.size(); ++n) {
    if (!spend()) return result;
    auto *value = pending[n];
    if (!value || !value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() > 64 ||
        llvm::isa<llvm::UndefValue, llvm::PoisonValue>(value))
      return refuse("opaque dependency is not an admitted scalar integer");
    if (auto *argument = llvm::dyn_cast<llvm::Argument>(value)) {
      if (argument->getParent() != &function_) return refuse("foreign integer argument");
      continue;
    }
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction) continue; // ConstantInt and other non-Instruction integers.
    if (!instruction->getParent() || instruction->getFunction() != &function_)
      return refuse("foreign or detached integer instruction");
    if (llvm::isa<llvm::FreezeInst>(instruction)) continue; // New opaque observation.
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
      if (!load->isSimple()) return refuse("observable integer LOAD");
      continue; // No address/memory traversal and no LOAD value/definedness fact.
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction)) {
      std::map<llvm::BasicBlock *, std::size_t> slots;
      for (auto *pred : llvm::predecessors(phi->getParent())) {
        if (!spend()) return result;
        ++slots[pred];
      }
      if (slots.empty()) return refuse("PHI has no actual defining predecessor");
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (!spend()) return result;
        auto *pred = phi->getIncomingBlock(i);
        auto found = slots.find(pred);
        if (!pred || pred->getParent() != &function_ || found == slots.end() ||
            phi->getIncomingValue(i)->getType() != phi->getType())
          return refuse("PHI does not cover actual same-typed predecessor slots");
        if (!--found->second) slots.erase(found);
        if (!enqueue(phi->getIncomingValue(i))) return result;
        if (auto *source = llvm::dyn_cast<llvm::PHINode>(phi->getIncomingValue(i)))
          phiEdges.emplace_back(phi, source);
      }
      if (!slots.empty()) return refuse("PHI omits an actual predecessor slot");
      continue;
    }
    if (auto *cast = llvm::dyn_cast<llvm::CastInst>(instruction)) {
      if (cast->getOpcode() != llvm::Instruction::Trunc &&
          cast->getOpcode() != llvm::Instruction::ZExt &&
          cast->getOpcode() != llvm::Instruction::SExt)
        return refuse("unsupported opaque integer cast");
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(instruction)) {
      auto *callee = call->getCalledFunction();
      if (call->isTerminator() || !callee ||
          (callee->getIntrinsicID() != llvm::Intrinsic::fshl &&
                      callee->getIntrinsicID() != llvm::Intrinsic::fshr) ||
          !call->doesNotAccessMemory() || call->isConvergent() ||
          call->hasFnAttr(llvm::Attribute::NoDuplicate) || call->getNumOperandBundles() ||
          call->arg_size() != 3)
        return refuse("call is not an admitted mathematical funnel intrinsic");
      for (auto &argument : call->args()) {
        if (argument->getType() != call->getType())
          return refuse("noncanonical funnel intrinsic type");
        if (!enqueue(argument.get())) return result;
      }
      continue;
    }
    if (instruction->isTerminator() || instruction->mayReadOrWriteMemory() ||
        instruction->mayHaveSideEffects())
      return refuse("nonpure integer dependency");
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(instruction)) {
      const auto opcode = binary->getOpcode();
      if (opcode == llvm::Instruction::Shl || opcode == llvm::Instruction::LShr ||
          opcode == llvm::Instruction::AShr) {
        auto *countValue = binary->getOperand(1);
        auto *count = llvm::dyn_cast<llvm::ConstantInt>(countValue);
        if (!count) {
          if (!spend()) return result; // Point-independent exact SSA H lookup.
          auto found = hypotheses_.find(countValue);
          if (found != hypotheses_.end()) count = found->second;
        }
        if (!count || count->getType() != countValue->getType() ||
            count->getBitWidth() > 64 ||
            count->getZExtValue() >= binary->getType()->getIntegerBitWidth())
          return refuse("shift count is not a legal literal or exact singleton hypothesis");
      }
    }
    // Select's condition and BOTH arms, all integer operands of binary/cast,
    // and the old pure-instruction default are all traversed. No flag becomes
    // a no-overflow/noundef fact. Explicit bad operands cannot hide behind H.
    for (auto &operand : instruction->operands()) {
      if (!spend()) return result;
      if (operand->getType()->isIntegerTy() && !enqueue(operand.get())) return result;
    }
  }
  // Contract only direct PHI-to-PHI forwarding regions, after validating
  // EVERY original node and actual incoming slot above. This groups shape
  // checks, never runtime values: it proves no equality, stability, range or
  // definedness. Non-PHI operations/observations retain separate nodes.
  std::vector<std::size_t> parent(pending.size()), rank(pending.size(), 0);
  for (std::size_t n = 0; n < parent.size(); ++n) {
    if (!spend()) return result;
    parent[n] = n;
  }
  auto representative = [&](std::size_t n) -> std::optional<std::size_t> {
    while (parent[n] != n) {
      if (!spend()) return {};
      n = parent[n];
    }
    return n;
  };
  for (auto [left, right] : phiEdges) {
    if (!spend()) return result;
    auto l = representative(seen.at(left)), r = representative(seen.at(right));
    if (!l || !r) return result;
    if (*l == *r) continue;
    if (!spend()) return result;
    if (rank[*l] < rank[*r]) parent[*l] = *r;
    else { parent[*r] = *l; if (rank[*l] == rank[*r]) ++rank[*l]; }
  }
  std::set<std::size_t> regions;
  std::size_t scalarNodes = 0;
  for (std::size_t n = 0; n < pending.size(); ++n) {
    if (!spend()) return result;
    if (llvm::isa<llvm::PHINode>(pending[n])) {
      auto region = representative(n);
      if (!region) return result;
      regions.insert(*region);
    } else ++scalarNodes;
  }
  result.uniqueNodes = scalarNodes + regions.size();
  if (result.uniqueNodes > nodeLimit) {
    result.budgetExhausted = true;
    return refuse("complete Phi-region footprint exceeds current node allowance");
  }
  if (!spend()) return result; // Entire-root cache publication is prepaid.
  admittedRegionRoots_.emplace(root, result.uniqueNodes);
  result.admitted = true;
  return refuse("complete opaque Phi-region dependency shape; Full only");
}

} // namespace dragon::llvmlifter::detail
