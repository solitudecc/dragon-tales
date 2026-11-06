#pragma once
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
  virtual std::uint32_t maxRounds() const {return 256;}
  virtual std::uint32_t maxTargets() const {return 256;}
  virtual void stopped(std::string_view) {}
};
} // namespace analysis
} // namespace dragon
