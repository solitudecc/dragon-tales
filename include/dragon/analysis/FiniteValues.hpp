#pragma once

#include <dragon/lifters/ignil/IGNIL.hpp>
#include <cstddef>
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
/// Read a fixed Guest address from the state immediately before an IGNIL LOAD.
/// The queried address can be another table cell than the LOAD's runtime
/// address. beforeOpIndex must identify a LOAD with a destination of exactly
/// width bits; widths outside 1..64 and invalid program points yield Unknown.
struct FiniteMemoryQuery {
  std::uint64_t block;
  std::size_t beforeOpIndex;
  std::uint64_t address;
  ignil::Width width;
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

/// Initial byte constraints, not machine register writes: a 32-bit fact does
/// not constrain or zero the upper half of a 64-bit register. Facts must have
/// a whole-byte width in 8..64, fit that width and not overlap other facts.
struct FiniteRegisterFact {
  std::uint32_t offset;
  ignil::Width width;
  std::uint64_t value;
};
struct FiniteMemoryFact {
  std::uint64_t address;
  std::vector<std::uint8_t> bytes;
};
struct FiniteEntryState {
  std::uint64_t entry;
  std::vector<FiniteRegisterFact> registers;
  std::vector<FiniteMemoryFact> memory;
};
struct FiniteInitialStateResult {
  FiniteValueResult analysis;
  /// Owned, bounded caller assumptions; never independently proved true.
  std::vector<FiniteEntryState> assumptions;
  bool assumptionsValid = false; ///< Structural validity, not truth of assumptions.
  /// Original analysis fields plus conditional:true, assumptions_valid and
  /// entry_states. Entries/offsets/values are hex strings, bytes are hex pairs.
  std::string toJSON() const;
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

/// Same fixed-point/root/alias/budget rules as finiteValues(), with .values
/// corresponding to fixed-address reads immediately before the queried LOADs.
/// Only preceding Guest STOREs supply memory facts: code/image bytes and a
/// readMemory callback are never initial-memory assumptions. Invalid queries
/// yield Unknown even in an unreachable block; valid unreachable queries yield
/// Unreachable. A global budget failure makes every result Unknown.
/// Entire blocks containing unlifted markers or local control remain barriers.
FiniteValueResult finiteMemoryValues(
    const Graph &graph, const std::set<std::uint64_t> &entries,
    std::span<const FiniteMemoryQuery> queries,
    const FiniteValueLimits &limits = {});

/// Conditional read-only analyses. Initial facts constrain only already
/// admitted valid roots; they never add roots or constrain undeclared entries
/// or independent callees. Backedges merge with, rather than reset, entry facts.
/// Unknown CALL/unlifted/local-control effects retain the ordinary barriers.
/// No image bytes are read and no Graph or target publication is changed.
/// Invalid assumptions or initialization budget exhaustion make the entire
/// analysis incomplete/Unknown; at most 262144 initialization bytes/facts are
/// admitted, additionally bounded by maxFacts/maxOperations. Owned assumptions
/// are retained only after bounded structural validation succeeds.
FiniteInitialStateResult finiteValuesWithInitialState(
    const Graph &graph, const std::set<std::uint64_t> &entries,
    std::span<const FiniteValueQuery> queries,
    std::span<const FiniteEntryState> initialStates,
    const FiniteValueLimits &limits = {});
FiniteInitialStateResult finiteMemoryValuesWithInitialState(
    const Graph &graph, const std::set<std::uint64_t> &entries,
    std::span<const FiniteMemoryQuery> queries,
    std::span<const FiniteEntryState> initialStates,
    const FiniteValueLimits &limits = {});

} // namespace analysis
} // namespace dragon
