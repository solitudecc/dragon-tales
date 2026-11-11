#pragma once
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
namespace dragon::llvmlifter::detail {
// Shared discovery/preflight rule. This is a current-IR SSA proof, not a hint.
// Caller provides a current-IR membership lookup and charges its cost.
// Core reuses its preflight map; mutations happen only in a trial.
template<class IsRegionMember>
inline bool eliminableSingleIncomingRootPHI(const llvm::PHINode &phi,
    llvm::BasicBlock &seed,IsRegionMember isRegionMember,
    const llvm::DominatorTree &dom) {
  if(phi.getNumIncomingValues()!=1 || phi.getIncomingBlock(0)!=&seed)return false;
  auto *value=phi.getIncomingValue(0);
  if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value))return false;
  if(auto *instruction=llvm::dyn_cast<llvm::Instruction>(value))
    return !isRegionMember(instruction->getParent()) &&
           dom.dominates(instruction,seed.getTerminator());
  if(auto *argument=llvm::dyn_cast<llvm::Argument>(value))
    return argument->getParent()==seed.getParent();
  return llvm::isa<llvm::ConstantInt,llvm::ConstantPointerNull>(value);
}
}
