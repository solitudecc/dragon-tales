#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace llvm { class ConstantInt; class Function; class Value; }

namespace dragon::llvmlifter::detail {

struct OpaqueIntegerAdmissionResult {
  bool admitted = false, budgetExhausted = false;
  std::size_t work = 0, uniqueNodes = 0;
  std::string message;
};

// Read-only INTEGER SHAPE admission, never a constant/range/definedness/stable
// observation certificate. Success permits only the ordinary full bit set;
// all original operations, flags, poison propagation and consumes must remain.
// Valid Function required. H is consulted only for exact-SSA, point-independent
// singleton shift counts; H proves nothing by itself and must be closed by the
// consumer. Its map must outlive this object and remain immutable. Reconstruct
// after any Function/CFG/SSA/H mutation.
//
// get() has at most 128 unique physical dependency nodes. Actual returned work consumes the parent's
// unchanged budget (at most min(maxWork,1M)). Cache only a completed root's full
// footprint N: a hit costs one work but still reports N and refuses if N exceeds
// the current allowance. On that budget refusal uniqueNodes is the required N;
// on an uncached failure it is the visited partial footprint. Zero allowances
// may return zero nodes. Callers must not clear budget failure or subtract one
// from zero. No partial graph/cache facts are published on any failure.
class OpaqueIntegerAdmission {
 public:
  OpaqueIntegerAdmission(llvm::Function &function,
      const std::map<llvm::Value *, llvm::ConstantInt *> &hypotheses)
      : function_(function), hypotheses_(hypotheses) {}
  OpaqueIntegerAdmissionResult get(llvm::Value *root,
      std::size_t maxNodes, std::size_t maxWork);

  // Separate opt-in shape policy: validate every original dependency and
  // actual PHI slot, then count a direct PHI-to-PHI connected region as ONE
  // structural node. Each non-PHI dependency remains a separate node. A region
  // conveys only Full integer bits, never equal/stable/defined values. maxNodes
  // is at most 128 for this COMPLETE compressed footprint; a refusal reports
  // the required footprint if collection completed (otherwise zero). Physical
  // collection is additionally capped at 65536 values and the same maxWork.
  // Every node/edge/region operation is charged before cache publication.
  // Cached hits retain the full compressed footprint and use one work. This
  // cache is separate from get(), whose original raw-node contract is intact.
  OpaqueIntegerAdmissionResult getPhiRegions(llvm::Value *root,
      std::size_t maxNodes, std::size_t maxWork);

 private:
  llvm::Function &function_;
  const std::map<llvm::Value *, llvm::ConstantInt *> &hypotheses_;
  std::map<llvm::Value *, std::size_t> admittedRoots_;
  std::map<llvm::Value *, std::size_t> admittedRegionRoots_;
};

} // namespace dragon::llvmlifter::detail
