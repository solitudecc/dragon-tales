#pragma once
#include <dragon/passes/PassPipeline.hpp>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace dragon::passes {
/// Opt-in x86 CFG recovery. Owns code and entry points, never borrows callbacks.
/// All independent entries in this code must be supplied; an empty list uses
/// the Graph's discovered functions. Only explicit guest stores supply table
/// values. Image bytes are used for decoding, not as immutable memory facts.
class FiniteDispatchPass {
 public:
  FiniteDispatchPass(std::span<const std::uint8_t> code,std::uint64_t base,
                     std::span<const std::uint64_t> entries = {});
  Result operator()(GraphContext &,const Options &) const;
  static PassPipeline::Validator validator();
 private:
  std::vector<std::uint8_t> code_;
  std::uint64_t base_;
  std::set<std::uint64_t> entries_;
};
} // namespace dragon::passes
