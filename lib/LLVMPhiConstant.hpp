#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace llvm { class ConstantInt; class Function; class PHINode; class Value; }

namespace dragon::llvmlifter::detail {

struct PhiConstantResult {
  llvm::ConstantInt *value = nullptr;
  std::size_t work = 0, nodes = 0;
  bool budgetExhausted = false;
  std::string message;
};

// Read-only, selected-root proof in a valid Function. The strict constructor
// admits only same-typed scalar
// integer PHIs (<=64 bits) and one literal ConstantInt may occur. Every actual
// predecessor slot, including duplicates/unreachable predecessors, is checked.
// All dependency nodes must reach a real constant anchor. This is not a range,
// arithmetic, memory, metadata or general loop-fixed-point analysis.
//
// Reconstruct this object after ANY Function/CFG/SSA mutation. Cached values
// are exact constants independent of execution/history; no partial or budget-
// refused graph is cached. Each get spends at most min(availableWork,65536)
// work and collects at most 512 PHIs. Even cache lookup costs one work unit;
// a cache hit expands zero nodes. Caller must charge returned work to its
// unchanged shared budget. Refusal never mutates IR or asserts definedness.
class PhiConstantGraph {
 public:
  explicit PhiConstantGraph(llvm::Function &function) : function_(function) {}
  // CONDITIONAL proof only: nonliteral leaves may additionally be exact SSA
  // keys for simple, unannotated integer LOADs in this Function. No address
  // equivalence, fresh-read reuse, arithmetic/argument/freeze anchor or IR
  // mutation is admitted. The referenced map must outlive this object and
  // remain unchanged. Reconstruct after changing H, Function, CFG or SSA.
  // All returned/cached values in this mode are conditional on H, not new
  // independent facts. The consumer must independently close H before any
  // publication; in particular this helper is NOT a memory certificate.
  PhiConstantGraph(llvm::Function &function,
                   const std::map<llvm::Value *, llvm::ConstantInt *> &hypotheses)
      : function_(function), hypotheses_(&hypotheses) {}
  PhiConstantResult get(llvm::PHINode *root, std::size_t availableWork);

 private:
  llvm::Function &function_;
  std::map<llvm::PHINode *, llvm::ConstantInt *> proven_;
  std::map<llvm::PHINode *, std::string> refusedRoots_;
  const std::map<llvm::Value *, llvm::ConstantInt *> *hypotheses_ = nullptr;
};

} // namespace dragon::llvmlifter::detail
