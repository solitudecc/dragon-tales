#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace llvm { class Function; class TruncInst; class PHINode; }
namespace dragon::llvmlifter::detail {
struct PhiLowBitsOptions {
  std::size_t maxCandidates = 128;
  std::size_t maxNodes = 256;
  std::size_t maxDepth = 32;
  std::size_t maxWork = 65536;
};
enum class PhiLowBitsProofProfile { Default, Expanded };
// Single source for the per-profile admission ceilings. Callers that want the
// largest admitted request must copy these values instead of restating them,
// otherwise a drifted literal is refused as an invalid (not budget) request.
constexpr PhiLowBitsOptions phiLowBitsCeiling(PhiLowBitsProofProfile profile) {
  return profile == PhiLowBitsProofProfile::Expanded
             ? PhiLowBitsOptions{128, 4096, 128, 1048576}
             : PhiLowBitsOptions{128, 256, 32, 65536};
}
static_assert(phiLowBitsCeiling(PhiLowBitsProofProfile::Default).maxNodes ==
              PhiLowBitsOptions{}.maxNodes &&
              phiLowBitsCeiling(PhiLowBitsProofProfile::Default).maxDepth ==
              PhiLowBitsOptions{}.maxDepth &&
              phiLowBitsCeiling(PhiLowBitsProofProfile::Default).maxWork ==
              PhiLowBitsOptions{}.maxWork,
              "default options must equal the Default ceiling");
struct PhiLowBitsStats {
  bool changed = false, complete = true, budgetExhausted = false;
  std::size_t candidatesVisited = 0, pairsVisited = 0;
  std::size_t rewrittenTruncs = 0, rewrittenUses = 0, nodes = 0, work = 0;
  // Only closed, defined small PHIs from a fully successful original-IR plan.
  // No body/node is replaced, so these handles remain valid until the caller
  // later mutates that function. This is not a range or wide-value witness.
  std::vector<llvm::PHINode *> provedLowPhis;
  std::string message;
};
// Original-IR proof only: no optimizer/shadow, range metadata or memory facts.
// Explicit original Trunc candidates are frozen before proof. All candidates
// must obtain a completely defined same-block PHI projection before any use is
// changed. Caps are shared by the whole call, including failed pair attempts.
// Freeze is a defined stable leaf, never merged with another Freeze. Numeric
// PHIs require all actual predecessor slots; cycles require real PHI seeds.
// Uses in effects/calls/terminators are refused. Only pure SSA operands change;
// every original Trunc/CFG/effect node is retained. Any failure leaves all
// original uses unchanged, including verification failure after application.
PhiLowBitsStats replacePhiLowTruncs(llvm::Function &,
    const std::vector<llvm::TruncInst *> &, const PhiLowBitsOptions & = {});
// Separate opt-in admission ceiling; options/defaults and the old overload
// stay unchanged. Expanded permits at most 4096 nodes / 128 depth / 1048576
// work. It does not reset shared counters or weaken any semantic check.
PhiLowBitsStats replacePhiLowTruncs(llvm::Function &,
    const std::vector<llvm::TruncInst *> &, const PhiLowBitsOptions &,
    PhiLowBitsProofProfile);
} // namespace dragon::llvmlifter::detail
