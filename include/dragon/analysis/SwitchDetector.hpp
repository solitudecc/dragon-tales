#pragma once

#include <dragon/analysis/BackwardSlicer.hpp>
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

    /// Bytes per table entry: 4 or 8 for a table of pointers; 1, 2, 4 or 8
    /// for a table of offsets (`relative`).
    std::uint32_t entrySize;

    /// Number of table entries (cases).  0 means the bounds check was not
    /// found; readTableTargets will return an empty vector in that case.
    std::uint32_t numCases;

    /// Register used as the table index, as identified in the slice
    /// expression (inside any ZEXT/SEXT/TRUNC). Width 0 when the index is
    /// not a plain register, e.g. a value loaded from the stack.
    ignil::Reg indexReg;

    /// A table of offsets rather than of pointers: case i is at
    ///   targetBase + (ext(entry[i]) << entryShift)
    /// where ext sign-extends (entrySigned) or zero-extends the low
    /// entryBits bits of the entry. x86-64 PIC code (`movsxd rax,
    /// [rdx+rax*4]; add rax, rdx`) has 4-byte signed entries relative to the
    /// table itself; gcc's AArch64 tables (`ldrh w3, [x3, w20, uxtw #1]; adr
    /// x0, L; add x3, x0, w3, sxth #2`) 1- or 2-byte signed entries counted
    /// in instructions (shift 2) from a label after the dispatch.
    bool relative = false;
    std::uint64_t targetBase = 0;
    std::uint8_t entryShift = 0;
    std::uint8_t entryBits = 0; ///< 0 = entrySize * 8
    bool entrySigned = false;
};

/// Inspect a backward-slice result and determine whether it is a jump-table
/// indirect branch.
///
/// Two target shapes are recognised: LOAD(table) - a table of pointers - and
/// targetBase + (ext(LOAD(table)) << shift) - a table of offsets (see
/// SwitchCandidate::relative).
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
///   2. Bounds: the sole predecessor must end in a CJMP; its condition is
///      sliced and evaluated at a prefix of index values to propose numCases.
///      The candidate then needs a complete interval proof or an SMT proof
///      that no guarded input reaches an out-of-range index. Wider register
///      views retain their free high bits. Timeout/UNKNOWN rejects the bound;
///      width/masking alone is not evidence of the table length.
///
/// Returns nullopt when the expression does not match the pattern.
/// Returns a SwitchCandidate with numCases == 0 when the pattern is found
/// but bounds could not be extracted from a predecessor guard.
///
/// `sliceDepth` is the BackwardSlicer depth `jumpTarget` was sliced with; the
/// guard is sliced with one less (it sits one block earlier) so that the
/// index reads the same in both.
/// `predecessors`, when given, is BackwardSlicer::predecessors(graph), shared
/// with the caller's own slices of the unchanged graph.
std::optional<SwitchCandidate> detectPossibleSwitch(
        const Expr &jumpTarget, std::uint64_t blockAddr, const Graph &graph,
        int sliceDepth = 5,
        const BackwardSlicer::Predecessors *predecessors = nullptr);

/// Read numCases pointer-sized entries from the jump table encoded in bytes.
///
/// Entries are read as little-endian integers; for a table of offsets each is
/// turned into targetBase + (ext(entry) << entryShift).
/// Returns an empty vector when numCases == 0 or the table extends outside
/// the provided byte range.
std::vector<std::uint64_t> readTableTargets(
        const SwitchCandidate &sw,
        std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr);

} // namespace dragon::slice
