#pragma once
#include <dragon/analysis/FiniteValues.hpp>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace dragon {
class Graph;
namespace analysis {
/// Trusted analysis used only by an explicitly configured recovery pass.
/// A returned set must cover every value on every reachable incoming path of
/// the supplied CFG. nullopt means unknown, never a truncated candidate list.
/// The CFG is borrowed only for prepare(); do not retain views across rounds.
class IndirectTargetResolver {
 public:
  virtual ~IndirectTargetResolver() = default;
  virtual bool prepare(const Graph &,const std::set<std::uint64_t> &entries)=0;
  virtual std::optional<std::vector<std::uint64_t>> targets(std::uint64_t block) const=0;
  /// Optional point-specific memory proof under the same analysis budgets.
  /// nullopt delegates to the disassembler's bounded default analysis; a
  /// returned incomplete/Unknown result is authoritative and must not cause
  /// a retry with larger default limits. Graph/views are synchronous only.
  virtual std::optional<FiniteValueResult> memoryFacts(
      const Graph &,const std::set<std::uint64_t> &,
      std::span<const FiniteMemoryQuery>) { return std::nullopt; }
  virtual std::uint32_t maxRounds() const {return 256;}
  virtual std::uint32_t maxTargets() const {return 256;}
  virtual void stopped(std::string_view) {}
  /// Per-case decode cap; a resolver may lower the built-in 1<<20 ceiling.
  /// Exhaustion abandons the tentative recovery rather than proving a
  /// candidate against a partially decoded direct-control-flow path.
  virtual std::uint32_t maxNewInstructions() const {return 1u<<20;}
};
} // namespace analysis
} // namespace dragon
