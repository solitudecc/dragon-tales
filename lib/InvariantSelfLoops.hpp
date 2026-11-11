#pragma once
#include <cstddef>
#include <string>
namespace llvm {class Module;}
namespace dragon::llvmlifter::detail {
struct InvariantSelfLoopsOptions {
  std::size_t maxLoops=64,maxWork=1000000,maxBlocks=65536;
};
struct InvariantSelfLoopsStats {
  bool available=false,complete=true,changed=false;
  std::size_t candidateLoops=0,isolatedLoops=0,removedSelfIncomings=0;
  std::size_t addedBlocks=0,rejected=0,budgetExhausted=0;
  std::size_t scannedInstructions=0,work=0;
  std::string message;
};
// Preserve the first dynamic comparison/branch and its false exit. Only a
// pure self-preserving PHI loop may redirect its true edge to a separate spin.
// This retains divergence; it does not prove a condition false or add progress.
InvariantSelfLoopsStats runInvariantSelfLoops(llvm::Module &,
    const InvariantSelfLoopsOptions & = {});
}
