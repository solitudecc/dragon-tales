#pragma once

#include <dragon/analysis/FiniteValues.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace dragon::analysis {

/// Monotonic finite-value join for bounded analyses.
///
/// Finite inputs are unioned and normalized. Unknown, width conflicts and
/// budget overflow move the accumulator to Unknown permanently; a caller must
/// never publish the values collected before that transition as a complete set.
class FiniteValueAccumulator {
 public:
  explicit FiniteValueAccumulator(ignil::Width width = 0,
                                  std::uint32_t maxValues = 64)
      : width_(width), maxValues_(maxValues) {}

  /// Merge one path result. Returns true when the abstract state changed.
  bool merge(const FiniteValues &incoming) {
    if (incoming.kind == ValueSetKind::Unreachable) return false;
    if (kind_ == ValueSetKind::Unknown) return false;
    bool changed = false;
    if (!width_) {
      width_ = incoming.width;
      changed = true;
    }
    if (!incoming.width || incoming.width != width_) {
      markUnknown();
      return true;
    }
    if (incoming.kind == ValueSetKind::Unknown || incoming.values.empty()) {
      markUnknown();
      return true;
    }
    if (incoming.values.size() > maxValues_) {
      markUnknown();
      return true;
    }
    for (const auto value : incoming.values) {
      if (width_ < 64 && (value >> width_) != 0) {
        markUnknown();
        return true;
      }
      if (!std::binary_search(values_.begin(), values_.end(), value)) {
        values_.insert(std::upper_bound(values_.begin(), values_.end(), value), value);
        changed = true;
        if (values_.size() > maxValues_) {
          markUnknown();
          return true;
        }
      }
    }
    if (kind_ == ValueSetKind::Unreachable) {
      kind_ = ValueSetKind::Finite;
      changed = true;
    }
    return changed;
  }

  bool add(std::uint64_t value) {
    return merge(FiniteValues{ValueSetKind::Finite, width_, {value}});
  }

  void markUnknown() {
    kind_ = ValueSetKind::Unknown;
    values_.clear();
  }

  [[nodiscard]] FiniteValues result() const {
    return {kind_, width_, kind_ == ValueSetKind::Finite ? values_ : std::vector<std::uint64_t>{}};
  }
  [[nodiscard]] ValueSetKind kind() const { return kind_; }
  [[nodiscard]] ignil::Width width() const { return width_; }
  [[nodiscard]] std::size_t size() const { return values_.size(); }
  [[nodiscard]] bool complete() const { return kind_ == ValueSetKind::Finite; }

 private:
  ValueSetKind kind_ = ValueSetKind::Unreachable;
  ignil::Width width_ = 0;
  std::uint32_t maxValues_ = 0;
  std::vector<std::uint64_t> values_;
};

struct IndirectTargetResult {
  std::vector<std::uint64_t> targets;
  bool complete = false;
  std::string reason;
};

/// Collect target values from all reachable paths without ever exposing a
/// truncated candidate list. Any unknown/incomplete path makes the final
/// result incomplete, even if other paths were finite.
class IndirectTargetAccumulator {
 public:
  explicit IndirectTargetAccumulator(std::uint32_t maxTargets = 256)
      : values_(64, maxTargets), maxTargets_(maxTargets) {}

  bool mergePath(const FiniteValues &path, bool pathComplete = true) {
    if (path.kind == ValueSetKind::Unreachable) return false;
    sawPath_ = true;
    bool changed = values_.merge(path);
    if (!pathComplete) markIncomplete("path_incomplete");
    if (!values_.complete()) markIncomplete("target_set_unknown_or_budget");
    return changed;
  }

  void markIncomplete(std::string_view why) {
    complete_ = false;
    if (reason_.empty()) reason_ = std::string(why);
    values_.markUnknown();
  }

  [[nodiscard]] IndirectTargetResult result() const {
    if (!sawPath_) return {{}, false, "no_reachable_path"};
    if (!complete_ || !values_.complete())
      return {{}, false, reason_.empty() ? "target_set_incomplete" : reason_};
    return {values_.result().values, true, {}};
  }

  [[nodiscard]] bool complete() const { return result().complete; }
  [[nodiscard]] std::uint32_t maxTargets() const { return maxTargets_; }

 private:
  FiniteValueAccumulator values_;
  std::uint32_t maxTargets_ = 0;
  bool sawPath_ = false;
  bool complete_ = true;
  std::string reason_;
};

}  // namespace dragon::analysis
