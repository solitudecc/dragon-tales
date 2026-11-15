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
} // namespace dragon::llvmlifter::detail
