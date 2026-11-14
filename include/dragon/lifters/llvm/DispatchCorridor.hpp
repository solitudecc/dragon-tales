#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace llvm { class Function; class BasicBlock; }
namespace dragon::llvmlifter::detail {
// Opt-in candidate paths, not certificates. Every edge, PHI, scalar state,
// execution-time dependency, side exit and budget is checked again internally.
struct DispatchCorridorEdge {
  llvm::BasicBlock *source = nullptr;
  unsigned successorSlot = 0;
  std::vector<llvm::BasicBlock *> path; // head through the original switch
};
struct DispatchCorridorOptions {
  std::size_t maxCandidates = 512;
  std::size_t maxRewrites = 64;
  std::size_t maxClonedBlocks = 64;
  std::size_t maxClonedInstructions = 16384; // copied ops + input snapshots
  std::size_t maxRepairCells = 262144; // separately prepaid SSAUpdater growth
  std::size_t maxWork = 1000000;
};
struct DispatchCorridorStats {
  bool changed = false, complete = true, budgetExhausted = false;
  std::size_t candidatesVisited = 0, rejected = 0, rewrittenEdges = 0;
  std::size_t clonedBlocks = 0, clonedInstructions = 0, snapshots = 0;
  std::size_t repairPhis = 0, repairCells = 0, work = 0;
  std::string message;
};
// Valid input module required. The candidates must be snapshotted before this
// invocation's first mutation. Candidate order is original Function block /
// successor-slot order; all copies are excluded. One owned complete module
// trial is used, with full verify and a body transaction preserving the public
// Module/Function/Argument objects. Refused individual edges remain original;
// a failed trial/commit leaves the entire original module unchanged. Success
// invalidates instruction and block handles, including the supplied candidates.
// No state truth/definedness, memory alias fact, RO image or extra execution
// assumption comes from this API. Only one actual selector use, at the old
// switch execution point, can become a two-state comparison. Literal/constant
// defined states may branch directly. Pure scalar bounds, memory checks,
// reads/writes, flags and original error exits retain their execution order.
DispatchCorridorStats threadDispatchCorridor(
    llvm::Function &, const std::vector<DispatchCorridorEdge> &originalCandidates,
    const DispatchCorridorOptions & = {});

} // namespace dragon::llvmlifter::detail
