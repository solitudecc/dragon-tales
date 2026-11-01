#pragma once

#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

#include <cstdint>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace dragon {

class Graph;

/// Backward-slices an IGNIL value through a CFG to build a symbolic
/// SliceExpr tree.
///
/// The slicer walks backwards through IGNIL ops.  Within a block,
/// Temp definitions are single-assignment so tracing is trivial.
/// When a Reg value is not defined in the current block the slicer
/// follows the unique predecessor (if one exists).  At a join node - more
/// than one predecessor - the register is sliced along each (within a per-call budget); if
/// every path yields the same expression made of constants only, that is
/// the value, otherwise the Reg is returned as SymReg (unknown). A path that
/// loops back to the same join without touching the register is skipped, so
/// a loop-invariant value set before the loop (a jump-table base hoisted
/// out of it) is found. Only constant results are merged: a SymReg or a load
/// stands for a value at the point the walk stopped, which differs between
/// paths.
/// If the full value disagrees but every path proves that a 64-bit register's
/// upper 32 bits are zero, that fact is retained as ZEXT(TRUNC(SymReg, 32)).
/// The low bits remain an unknown value at the join, not a path's expression.
///
/// Registers alias by byte, as in the IGNIL register space: the definition of
/// RAX is the latest write to RAX, EAX, AX, AL or AH. A 32-bit write zeroes
/// the rest of its parent (EAX -> RAX, W0 -> X0); an 8/16-bit write is merged
/// into the value the register had before it.
///
/// The depth parameter caps the number of inter-block steps, so the
/// search terminates in bounded time even for deep chains.
class BackwardSlicer {
  public:
  explicit BackwardSlicer(int maxBlocks = 5);

  /// Slice `root` as it appears at the end of the block at `blockAddr`.
  /// Returns a SliceExpr tree suitable for Z3 lowering.
  [[nodiscard]] slice::Expr slice(const ignil::Value &root,
                                  std::uint64_t blockAddr,
                                  const Graph &graph) const;

  private:
  // Every predecessor of each block, and the join-node queries in progress
  // (block, register offset, width) used to recognise a loop back to one.
  struct PredMap {
    using JoinKey =
            std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> preds;
    std::set<JoinKey> joinsInProgress;
    // Negative Undef op indices are private recursion markers. Keep the
    // register identity as well as the join: COPY from another register must
    // never be mistaken for an unchanged path around this register's loop.
    std::unordered_map<int, JoinKey> loopMarkers;
    // A secondary query for known upper bits must not recursively launch
    // more secondary queries while evaluating a wide source of a write.
    bool probingUpper = false;
    // Predecessor blocks still allowed to be entered by this slice() call:
    // following every path through joins is exponential in the worst case.
    int budget = 4096;
  };

  [[nodiscard]] PredMap buildPredecessorMap(const Graph &graph) const;

  [[nodiscard]] std::optional<slice::ValueOrigin> memoryState(
          const ignil::Block &block, int beforeIdx, const Graph &graph,
          const PredMap &preds, int context) const;

  // Resolve `v` starting the backward scan at `beforeIdx` (exclusive) inside
  // `block`.  Crosses into predecessor blocks when `depth` > 0.
  [[nodiscard]] slice::Expr sliceValue(const ignil::Value &v,
                                       const ignil::Block &block, int beforeIdx,
                                       const Graph &graph, PredMap &preds,
                                       int depth) const;

  // Resolve register `r` before ops[beforeIdx], honouring sub-register
  // aliasing (a write to EAX/AL/AH is a definition of RAX too).
  [[nodiscard]] slice::Expr sliceReg(const ignil::Reg &r,
                                     const ignil::Block &block, int beforeIdx,
                                     const Graph &graph, PredMap &preds,
                                     int depth) const;

  // Turn the op that defines a value into a SliceExpr by recursively slicing
  // each source operand.
  [[nodiscard]] slice::Expr buildFromOp(const ignil::Op &op,
                                        const ignil::Block &block, int opIdx,
                                        const Graph &graph,
                                        PredMap &preds, int depth) const;
  // To keep the maximum number to go backward
  // this is selected by user with the configuration.
  int maxBlocks_;
};

} // namespace dragon
