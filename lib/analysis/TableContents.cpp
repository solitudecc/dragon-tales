#include "TableContents.hpp"
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/analysis/IndirectTargets.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace dragon::analysis::detail {
namespace {
const slice::Load *targetLoad(const slice::Expr &expression, unsigned depth = 0) {
  if (depth > 128) return nullptr;
  if (const auto *load = std::get_if<slice::Load>(&expression.data)) return load;
  if (const auto *unary = std::get_if<slice::UnaryOp>(&expression.data))
    return unary->operand ? targetLoad(*unary->operand, depth + 1) : nullptr;
  if (const auto *binary = std::get_if<slice::BinOp>(&expression.data)) {
    const auto *left = binary->left ? targetLoad(*binary->left, depth + 1) : nullptr;
    const auto *right = binary->right ? targetLoad(*binary->right, depth + 1) : nullptr;
    return left && right ? nullptr : left ? left : right;
  }
  return nullptr;
}
}

std::vector<std::uint64_t> provedTableTargets(
    const slice::SwitchCandidate &table, const slice::Expr &target,
    const Graph &graph, const std::set<std::uint64_t> &entries,
    const Disassembler::AnalysisHints &hints) {
  // Bound allocation, analysis and output independently of untrusted counts.
  constexpr std::uint32_t maxEntries = 4096;
  constexpr std::size_t maxTargets = 4096;
  if (!table.numCases || table.numCases > maxEntries || !table.entrySize ||
      table.entrySize > 8 || table.entryShift >= 64 ||
      std::uint64_t(table.numCases) * table.entrySize - 1 > UINT64_MAX - table.tableBase)
    return {};
  const auto *load = targetLoad(target);
  if (!load || load->opIndex < 0 || load->width != table.entrySize * 8) return {};
  const auto *block = graph.getIGNILBlock(load->blockAddress);
  const auto index = static_cast<std::size_t>(load->opIndex);
  if (!block || index >= block->ops().size()) return {};
  const auto &operation = block->ops()[index];
  if (operation.opcode != ignil::Opcode::LOAD || !operation.dst || operation.srcs.size()!=1 ||
      ignil::widthOf(*operation.dst) != load->width) return {};
  const Disassembler::MemoryReadSite site{load->blockAddress, index, operation.address};

  std::vector<FiniteMemoryQuery> queries;
  queries.reserve(table.numCases);
  for (std::uint32_t i = 0; i < table.numCases; ++i)
    queries.push_back({site.blockAddress, site.opIndex,
        table.tableBase + std::uint64_t(i) * table.entrySize, load->width});
  // Explicit content assertions are independent proofs, not seed bytes for
  // the forward analysis. In particular they do not initialise other entries.
  std::vector<std::optional<std::uint64_t>> asserted(queries.size());
  bool needFacts = false;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    std::array<std::uint8_t, 8> bytes{};
    if (hints.readKnownMemory && hints.readKnownMemory(site, queries[i].address,
            std::span(bytes).first(table.entrySize))) {
      std::uint64_t value = 0;
      for (unsigned b = 0; b < table.entrySize; ++b)
        value |= std::uint64_t(bytes[b]) << (8 * b);
      asserted[i] = value;
    } else needFacts = true;
  }
  FiniteValueResult facts;
  if (needFacts) {
    auto provided = hints.indirectTargets
        ? hints.indirectTargets->memoryFacts(graph, entries, queries) : std::nullopt;
    facts = provided ? std::move(*provided) : finiteMemoryValues(graph, entries, queries);
  }

  std::set<std::uint64_t> targets;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    std::vector<std::uint64_t> values;
    if (asserted[i]) values.push_back(*asserted[i]);
    else {
      // A budget failure invalidates every forward fact. A partial finite
      // result or an unreachable LOAD is never turned into a target list.
      if (!facts.complete || facts.values.size() != queries.size()) return {};
      const auto &fact = facts.values[i];
      if (fact.kind != ValueSetKind::Finite || fact.width != load->width) return {};
      values = fact.values;
    }
    if (values.empty()) return {};
    for (auto value : values) {
      std::array<std::uint8_t, 8> bytes{};
      for (unsigned b = 0; b < table.entrySize; ++b)
        bytes[b] = static_cast<std::uint8_t>(value >> (8 * b));
      auto single = table;
      single.tableBase = 0;
      single.numCases = 1;
      const auto decoded = slice::readTableTargets(single,
          std::span(bytes).first(table.entrySize), 0);
      if (decoded.size() != 1) return {};
      targets.insert(decoded.front());
      if (targets.size() > maxTargets) return {};
    }
  }
  return {targets.begin(), targets.end()};
}
} // namespace dragon::analysis::detail
