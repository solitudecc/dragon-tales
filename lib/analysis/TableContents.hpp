#pragma once

#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <set>

namespace dragon::analysis::detail {

// A bound proves which entries can be read, not what those entries contain.
// Resolve only from Guest STORE facts at the producing LOAD or an explicit
// trusted current-content assertion. Initial code/image bytes are not inputs.
std::vector<std::uint64_t> provedTableTargets(
    const slice::SwitchCandidate &table, const slice::Expr &target,
    const Graph &graph, const std::set<std::uint64_t> &entries,
    const Disassembler::AnalysisHints &hints);

} // namespace dragon::analysis::detail
