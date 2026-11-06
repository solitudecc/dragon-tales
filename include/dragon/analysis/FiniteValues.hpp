#pragma once

#include <dragon/lifters/ignil/IGNIL.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace dragon {
class Graph;
namespace analysis {

/// A complete over-approximation, never a list truncated to fit a budget.
/// Unreachable is bottom; Unknown is top. Finite can include infeasible values.
enum class ValueSetKind { Unreachable, Finite, Unknown };
struct FiniteValues {
  ValueSetKind kind = ValueSetKind::Unknown;
  ignil::Width width = 0;
  std::vector<std::uint64_t> values;
  bool operator==(const FiniteValues &) const = default;
};
struct FiniteValueLimits {
  std::uint32_t maxValues = 64;
  std::uint32_t maxBlocks = 4096;
  std::uint32_t maxOperations = 1000000;
  std::uint32_t maxFacts = 262144;
  bool proveLeafCalls = false; ///< Legacy name: opt-in bounded x86 call-tree effects.
};
struct FiniteValueQuery {
  std::uint64_t block;
  ignil::Value value; ///< Value at the end of this block.
};
struct FiniteValueResult {
  bool complete = true; ///< Fixed point reached; not CFG/deobfuscation completeness.
  std::string message;
  std::uint64_t blockVisits = 0;
  std::uint64_t operations = 0;
  std::map<std::uint64_t, FiniteValues> jumpTargets; ///< Key: IGNIL block start.
  std::vector<FiniteValues> values; ///< Same order as the requested queries.
  std::uint64_t provenCalls = 0; ///< Unique reachable direct call sites proved this run.
  std::uint64_t rejectedCalls = 0; ///< Attempted sites retaining the full barrier.
  std::string toJSON() const; ///< Unsigned addresses/values are hexadecimal strings.
};

/// Forward finite-set analysis of IGNIL, independent of the constant-only slicer.
/// Every supplied entry admits arbitrary registers/memory. Empty entries use
/// valid function starts, then predecessor-free blocks, or all blocks in a
/// rootless cyclic IGNIL graph (never infer that the entire graph is dead).
/// CALL clobbers registers and memory by default. proveLeafCalls can retain
/// untouched bytes across proved near-call/C3 bodies, including stack frames and
/// bounded internal call trees, and propagate constants agreed by every return.
/// Every return restores SP and protects its return slot; all proved stack writes
/// and the machine CALL return slot invalidate caller memory facts. No ABI guesses.
/// Only preceding guest
/// STOREs supply memory values; image bytes are not initial-state facts.
/// Stack addresses retain a symbolic entry-SP base. Distinct symbolic bases and
/// concrete vs symbolic addresses may alias; unknown stores invalidate memory.
/// Any global budget failure makes every reported value Unknown.
FiniteValueResult finiteValues(
    const Graph &graph, const std::set<std::uint64_t> &entries = {},
    std::span<const FiniteValueQuery> queries = {},
    const FiniteValueLimits &limits = {});

} // namespace analysis
} // namespace dragon
