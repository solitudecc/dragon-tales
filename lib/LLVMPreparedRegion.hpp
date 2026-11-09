#pragma once

#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <memory>
#include <optional>

namespace dragon::llvmlifter::detail {

/// An instance identity, not a claim about values, paths or memory snapshots.
struct RegionInstance final {};
struct OpOrigin {
  std::shared_ptr<const RegionInstance> instance;
  std::uint64_t block;
  std::size_t opIndex;
  std::uint64_t machinePC; ///< Diagnostic only; multiple operations can share it.
  bool operator==(const OpOrigin &) const = default;
};

/// A frozen, owned machine/IGNIL view. Graph lives at a fixed heap address:
/// moving this owner cannot invalidate its BasicBlock/Function views.
/// Private to the lifter; no additional Graph edges or initial memory facts.
class PreparedRegion {
 public:
  static std::unique_ptr<PreparedRegion> prepare(const Function &, const Graph &,
      std::span<const std::uint64_t> additionalStarts, const StaticRegionOptions &);
  PreparedRegion(const PreparedRegion &) = delete;
  PreparedRegion &operator=(const PreparedRegion &) = delete;
  PreparedRegion(PreparedRegion &&) noexcept = default;
  PreparedRegion &operator=(PreparedRegion &&) noexcept = default;

  const Graph &graph() const { return *graph_; }
  Function root() const { return Function{graph_.get(), entry_}; }
  const std::map<std::uint64_t, BasicBlock> &blocks() const { return blocks_; }
  bool rebuilt() const { return rebuilt_; }
  std::shared_ptr<const RegionInstance> instance() const { return instance_; }
  std::optional<OpOrigin> origin(std::uint64_t block, std::size_t opIndex) const;
  bool owns(const OpOrigin &) const;

 private:
  PreparedRegion(std::unique_ptr<const Graph> graph, std::uint64_t entry,
      const std::set<std::uint64_t> &starts, bool rebuilt);
  std::unique_ptr<const Graph> graph_;
  std::uint64_t entry_;
  std::map<std::uint64_t, BasicBlock> blocks_;
  bool rebuilt_;
  std::shared_ptr<const RegionInstance> instance_ = std::make_shared<const RegionInstance>();
};

} // namespace dragon::llvmlifter::detail
