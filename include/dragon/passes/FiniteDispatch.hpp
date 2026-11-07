#pragma once
#include <dragon/passes/PassPipeline.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace dragon::passes {
/// Opt-in x86 CFG recovery. Owns code and entry points.
/// All independent entries in this code must be supplied; an empty list uses
/// the Graph's discovered functions. Only explicit guest stores supply table
/// values. Image bytes are used for decoding, not as immutable memory facts.
class FiniteDispatchPass {
 public:
  FiniteDispatchPass(std::span<const std::uint8_t> code,std::uint64_t base,
                     std::span<const std::uint64_t> entries = {});
  /// Copies explicit hint functions and merges their independent entries.
  /// Captured external data must remain alive until this executor is destroyed.
  FiniteDispatchPass(std::span<const std::uint8_t> code,std::uint64_t base,
                     std::span<const std::uint64_t> entries,
                     const Disassembler::AnalysisHints &hints);
  Result operator()(GraphContext &,const Options &) const;
  static PassPipeline::Validator validator();
 private:
  std::vector<std::uint8_t> code_;
  std::uint64_t base_;
  std::set<std::uint64_t> entries_;
  Disassembler::AnalysisHints hints_;
};
} // namespace dragon::passes
