#include "ConstantBranches.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Local.h>
#include <set>
#include <vector>
namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Generated="dragon.guarded-transitions.generated";
bool constantTerminator(const llvm::BasicBlock &block) {
  auto *term=block.getTerminator();
  if(auto *branch=llvm::dyn_cast<llvm::BranchInst>(term))
    return branch->isConditional() && llvm::isa<llvm::ConstantInt>(branch->getCondition());
  if(auto *jump=llvm::dyn_cast<llvm::SwitchInst>(term))
    return llvm::isa<llvm::ConstantInt>(jump->getCondition());
  return false;
}
}
ConstantBranchesStats runConstantBranches(llvm::Module &module,const ConstantBranchesOptions &options) {
  ConstantBranchesStats result;
  auto reject=[&](const char *message,bool budget=false) {
    result.complete=false;++result.rejected;result.budgetExhausted+=budget;
    result.message=message;return result;
  };
  if(options.maxFolds>65536 || options.maxInstructions>1000000 || options.maxBlocks>65536)
    return reject("constant-branches option hard limit exceeded",true);
  std::vector<llvm::BasicBlock *> candidates;
  std::size_t blocks=0;
  // All rejection and configured-budget checks precede the first mutation.
  for(auto &function:module) {
    if(function.isDeclaration())continue;
    result.available=true;
    if(function.hasPersonalityFn() || function.getSubprogram())
      return reject("constant-branches does not support EH or debug functions");
    for(auto &block:function) {
      if(++blocks>options.maxBlocks)return reject("constant-branches block scan budget exhausted",true);
      if(!block.getTerminator())return reject("invalid constant-branches input block");
      if(block.isEHPad() || block.hasAddressTaken())
        return reject("constant-branches does not support EH or blockaddress identity");
      for(auto &instruction:block) {
        if(++result.scannedInstructions>options.maxInstructions)
          return reject("constant-branches instruction scan budget exhausted",true);
        if(instruction.getDebugLoc() || instruction.hasDbgRecords() || instruction.isUsedByMetadata())
          return reject("constant-branches does not support debug or local metadata identity");
      }
      if(constantTerminator(block))candidates.push_back(&block);
    }
  }
  if(!result.available)return result;
  if(llvm::verifyModule(module))return reject("invalid constant-branches input module");
  result.candidateTerminators=candidates.size();
  if(candidates.size()>options.maxFolds) {
    result.remainingConstants=candidates.size();
    return reject("constant-branches fold budget exhausted",true);
  }
  std::set<llvm::Function *> changedFunctions;
  for(auto *block:candidates) {
    // Other folds may simplify PHIs but never delete blocks at this stage.
    if(!constantTerminator(*block))continue;
    auto *old=block->getTerminator();
    const bool branch=llvm::isa<llvm::BranchInst>(old);
    auto *marker=old->getMetadata(Generated);
    if(!llvm::ConstantFoldTerminator(block,false))continue;
    if(marker)block->getTerminator()->setMetadata(Generated,marker);
    result.foldedBranches+=branch;result.foldedSwitches+=!branch;
    result.changed=true;changedFunctions.insert(block->getParent());
  }
  for(auto *function:changedFunctions) {
    const auto before=function->size();
    // This utility only deletes blocks outside the actual entry-reachable CFG;
    // unlike removeUnreachableBlocks/SimplifyCFG, it performs no general
    // terminator simplification. Keep one-input PHIs rather than inventing an
    // additional value-propagation policy here.
    llvm::EliminateUnreachableBlocks(*function,nullptr,true);
    result.removedBlocks+=before-function->size();
  }
  for(auto &function:module)for(auto &block:function)
    result.remainingConstants+=constantTerminator(block);
  if(result.remainingConstants) {
    result.complete=false;
    result.message="new constant terminators require another explicit pass";
  }
  return result;
}
}
