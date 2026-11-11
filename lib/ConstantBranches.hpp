#pragma once
#include <cstddef>
#include <string>
namespace llvm { class Module; }
namespace dragon::llvmlifter::detail {
struct ConstantBranchesOptions {
  std::size_t maxFolds=4096,maxInstructions=1000000,maxBlocks=65536;
};
struct ConstantBranchesStats {
  bool available=false,complete=true,changed=false;
  std::size_t candidateTerminators=0,foldedBranches=0,foldedSwitches=0;
  std::size_t removedBlocks=0,remainingConstants=0,rejected=0,budgetExhausted=0;
  std::size_t scannedInstructions=0;
  std::string message;
};
// One frozen sweep of literal ConstantInt branch/switch conditions. No symbolic
// evaluator, SimplifyCFG, threading or branch-merging pass is invoked. A later
// explicit invocation may consume constants exposed by PHI simplification.
ConstantBranchesStats runConstantBranches(llvm::Module &,
    const ConstantBranchesOptions & = {});
}
