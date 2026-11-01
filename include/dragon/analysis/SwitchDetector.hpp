#pragma once

#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dragon {
class Graph;
} // namespace dragon

namespace dragon::slice {

/// Everything extracted from a detected jump-table pattern.
struct SwitchCandidate {
    /// Base address of the jump table in the binary image.
    std::uint64_t tableBase;

    /// Bytes per table entry: 4 (32-bit pointers) or 8 (64-bit pointers).
    std::uint32_t entrySize;

    /// Number of table entries (cases).  0 means the bounds check was not
    /// found; readTableTargets will return an empty vector in that case.
    std::uint32_t numCases;

    /// Register used as the table index, as identified in the slice
    /// expression (inside any ZEXT/SEXT/TRUNC). Width 0 when the index is
    /// not a plain register, e.g. a value loaded from the stack.
    ignil::Reg indexReg;
};

/// Inspect a backward-slice result and determine whether it is a jump-table
/// indirect branch.
///
/// Recognition is two-phase:
///   1. Address match: the expression must be a LOAD whose address is a sum
///      of constants (the table base) and exactly one scaled index - SHL(idx,
///      k), how the x86 lifter writes a SIB scale, or MUL(idx, c) - with a
///      scale of 4 or 8, nested in any order. So both ADD(base, MUL(idx, 8))
///      and the lifter's ADD(ADD(0, SHL(idx, 3)), base) match. Width changes
///      around the index (ZEXT(EDI) after `mov edi, edi`) are looked through.
///      A second variable term (a base register, as in position-independent
///      tables of relative offsets) is not matched.
///   2. Bounds: the unique predecessor ending in a CJMP has its condition
///      sliced and *evaluated* with the index set to 0, 1, 2, ... - the shape
///      of the comparison differs per lifter and per ja/jae/b.hi, so it is
///      not pattern-matched. numCases is the first index that leaves the
///      dispatch block, accepted only if every larger index probed (including
///      ones a signed compare would let through) leaves it as well.
///
/// Returns nullopt when the expression does not match the pattern.
/// Returns a SwitchCandidate with numCases == 0 when the pattern is found
/// but bounds could not be extracted from a predecessor guard.
std::optional<SwitchCandidate> detectPossibleSwitch(const Expr &jumpTarget,
                                                    std::uint64_t blockAddr,
                                                    const Graph &graph);

/// Read numCases pointer-sized entries from the jump table encoded in bytes.
///
/// Entries are read as little-endian integers (suitable for x86/x86-64).
/// Returns an empty vector when numCases == 0 or the table extends outside
/// the provided byte range.
std::vector<std::uint64_t> readTableTargets(
        const SwitchCandidate &sw,
        std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr);

} // namespace dragon::slice
