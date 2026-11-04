#pragma once

#include <dragon/lifters/ignil/IGNIL.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace dragon::ignil {

/// What changed during a simplifyBlock() run. Useful for tests and future
/// coverage/audit tooling (mirrors the role
/// scripts/audit_x86_lifter_coverage.py plays for the lifter itself).
struct SimplifyStats {
  std::size_t constantsFolded = 0; ///< Ops replaced by a folded Const COPY.
  std::size_t identitiesApplied = 0; ///< Ops replaced via an algebraic
                                      ///< identity/annihilator/no-op rule.
  std::size_t opsRemoved = 0; ///< Dead-Temp-defining ops dropped.
  std::size_t iterations = 0; ///< Fixed-point iterations actually run.

  [[nodiscard]] bool changed() const {
    return constantsFolded || identitiesApplied || opsRemoved;
  }
};

/// Peephole-simplifies `block` and returns a new Block: forward constant
/// folding/propagation, algebraic identity/annihilator elimination, no-op
/// width-conversion elimination, and dead-Temp elimination. Runs to a fixed
/// point (bounded by maxIterations) since one rewrite can expose another.
///
/// Block-local only: does not cross into predecessor/successor blocks (same
/// boundary BackwardSlicer already stops at), does not canonicalize memory
/// addresses, and never removes a Reg-defining op even if apparently unused
/// within the block (a Reg may be live-out across the block boundary; only
/// Temps are provably block-local).
[[nodiscard]] Block simplifyBlock(const Block &block, int maxIterations = 8);

/// Same as above, but also reports what changed via `stats`.
[[nodiscard]] Block simplifyBlock(const Block &block, SimplifyStats &stats,
                                  int maxIterations = 8);

// ---------------------------------------------------------------------------
// Dead-store elimination
// ---------------------------------------------------------------------------

/// Tuning knobs for deadStoreEliminate().
struct DeadStoreOptions {
  /// Model the x86-64 / AArch64 rule that a 32-bit write to a register clears
  /// the rest of its parent (`mov eax, ...` zeroes RAX[63:32], `mov w0, ...`
  /// zeroes X0[63:32]). With this on, a width-32 write is treated as
  /// clobbering its *whole parent register*, which is required for
  /// *correctness* of the removal decision - dropping such a write would
  /// otherwise leak the parent's stale upper bytes. 8- and 16-bit writes always
  /// preserve the surrounding bytes on these targets, so they are unaffected.
  /// Turn off only for a target where a 32-bit write is a merge, not a zero.
  bool zeroExtendOn32BitWrite = true;

  /// The target's maximal registers, which is how a 32-bit write's parent - and
  /// therefore the byte range it zero-extends over - is resolved exactly. Pass
  /// dragon::x86::registerFile() or dragon::arm::registerFile().
  ///
  /// Supplying it MATTERS whenever a 32-bit register can be a lane view of
  /// something wider than 8 bytes. On AArch64 `S0` is a 32-bit view of the
  /// 128-bit `V0` at the same offset, and `fmov s0, ...` zeroes all 16 bytes of
  /// V0, not 8. Left empty, an 8-byte parent is assumed, which is exact for
  /// x86-64 GPRs (EAX->RAX) and AArch64 W registers (W0->X0) but
  /// *under*-estimates an S register's clobber - and since a store is removed
  /// only when every byte it clobbers is dead, under-estimating it can drop a
  /// write whose upper parent bytes were still live. Pass the register file for
  /// any block that can touch FP/SIMD registers.
  std::vector<RegExtent> registerFile;
};

/// Removes dead register/flag stores from a single block, byte-precisely.
///
/// This is the IGNIL analogue of Triton's block dead-store elimination
/// (BasicBlock `simplify`): a value written to a register or flag that is
/// overwritten before it is ever read - and is not live on exit from the block
/// - carries no information and its producing op can be dropped. Unlike
/// simplifyBlock's dead-*Temp* elimination, this also removes dead *Reg* stores,
/// which is what collapses the junk-instruction noise VM protectors emit
/// (a wall of rcl/shld/setcc/and writing registers that are immediately
/// popped over).
///
/// The register address space is treated as a flat byte array, so overlapping
/// sub-registers alias correctly: writing AX (bytes [0,2)) kills AL and AH but
/// leaves the rest of RAX live; writing AL (byte 0) leaves AH (byte 1) live.
/// Flags are ordinary single-byte registers and fold into the same analysis.
///
/// `liveOut` names the registers whose value is observable after the block:
///   - std::nullopt (the default) means "assume everything is live on exit",
///     the conservative model Triton uses. A store is then removed only when it
///     is provably overwritten *within this block* before any read or interior
///     control transfer, which needs no ABI knowledge. This reproduces Salwan's VMProtect
///     example: every junk write is clobbered by a later junk write or a pop,
///     while the pops themselves are the last writers and survive.
///   - a Reg list means precisely those registers (their byte ranges) are live
///     out; anything else is dead at exit and may be eliminated more
///     aggressively (e.g. a RET-terminated block whose live-out is just the ABI
///     return + callee-saved registers). Correctness then rests entirely on the
///     caller supplying a sound live-out set.
///
/// CALL may read registers implicitly; caller live-out does not describe callee
/// inputs. Without a callee summary, every register byte mentioned by the block
/// is conservatively live at CALL, including a final CALL with empty live-out.
/// Interior JMP/CJMP/RET likewise retain the register state at that prefix exit.
/// A final non-CALL transfer still honors the caller's explicit live-out set.
///
/// Block-local, semantics-preserving, and conservative: side-effecting ops
/// (STORE/CALL/JMP/CJMP/RET, and per hasSideEffect any control transfer) are
/// never removed, and a store is dropped only when *every* byte it clobbers is
/// dead. Also drops dead Temps (block-local by construction), subsuming the
/// Temp DCE in simplifyBlock so a lifted machine instruction whose only live
/// effect was the removed register write collapses entirely.
[[nodiscard]] Block deadStoreEliminate(
        const Block &block,
        const std::optional<std::vector<Reg>> &liveOut = std::nullopt,
        DeadStoreOptions opts = {});

/// Same as above, but also reports what changed via `stats` (opsRemoved).
[[nodiscard]] Block deadStoreEliminate(
        const Block &block, const std::optional<std::vector<Reg>> &liveOut,
        SimplifyStats &stats, DeadStoreOptions opts = {});

} // namespace dragon::ignil
