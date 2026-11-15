#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace llvm { class Function; class Instruction; class Value; }

namespace dragon::llvmlifter::detail {

struct PathValueRange {
  std::uint64_t low, high;
  unsigned bits;
};

// Explicit traversal accounting. The historical mode also counts every
// instruction through BasicBlock::size(); neither mode infers facts from it.
enum class PathRangeTraversal { WholeFunction, BranchConditions };

// Private consumer of independently proved, stable, defined SSA observations.
// A witness is not an address-wide fact or a caller-provided integer range.
// No facts are inferred from other observations, separate freezes, or LOADs.
// The function/CFG/SSA must stay unchanged for this object's lifetime.
class PathValueRanges {
public:
  PathValueRanges(llvm::Function &, const std::vector<llvm::Value *> &witnesses,
                  std::size_t maxWork);
  PathValueRanges(llvm::Function &, const std::vector<llvm::Value *> &witnesses,
                  std::size_t maxWork, PathRangeTraversal);
  ~PathValueRanges();
  PathValueRanges(const PathValueRanges &) = delete;
  PathValueRanges &operator=(const PathValueRanges &) = delete;

  // Bounds hold at this exact program point only. PHI incoming values and
  // stored values must be queried at their own predecessor/STORE, never at a
  // later LOAD's use point. No rewrite or general value fact is exported.
  std::optional<PathValueRange> get(llvm::Value *, llvm::Instruction *at,
                                   std::size_t availableWork = 65536);
  std::size_t work() const;
  bool budgetExhausted() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace dragon::llvmlifter::detail
