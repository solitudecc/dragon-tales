#pragma once

#include <cstddef>
#include <string>

namespace llvm { class Function; class SwitchInst; }

namespace dragon::llvmlifter::detail {
// Opt-in selected-original-switch component; no default pass. Only lower caps.
struct DispatchDecoderSSAOptions {
  std::size_t maxNodes = 256;
  std::size_t maxPhis = 128;
  std::size_t maxInstructions = 512;
  std::size_t maxWork = 16384;
};
struct DispatchDecoderSSAStats {
  bool changed = false, refused = false, budgetExhausted = false;
  std::size_t nodes = 0, phis = 0, instructions = 0, work = 0;
  std::string message;
};
// Move a scalar constant-side decoder through a finite, acyclic PHI/select
// graph. Each dynamic arithmetic leaf may depend on one existing i1 or AND-1
// SSA observation, whose entire 0/1 domain is evaluated with APInt, including
// the original operations' poison-producing flags. No observation is cloned
// or assumed defined. A new select retains its original atom dependency even
// when both decoded values agree. Every actual PHI predecessor slot is covered.
// Success changes only the selected switch condition and inserts scalar PHIs /
// selects / bit comparisons; all original operations and other liveouts remain.
// Unknown shapes, gray cycles, unsafe flags, budgets and verification failures
// refuse transactionally without changing the module.
DispatchDecoderSSAStats normalizeDispatchDecoder(
    llvm::Function &, llvm::SwitchInst &selectedOriginalSwitch,
    const DispatchDecoderSSAOptions & = {});
// Explicit proof consumer: replace every snapshotted ORIGINAL instruction use
// of exactly the selected decoder-root SSA value. Never matches another LOAD
// or a merely similar expression. Original operations remain; each ordinary
// use / PHI incoming edge must be dominated by the decoded value. At most 512
// original uses, charged to the same maxWork; any failure restores all uses.
DispatchDecoderSSAStats normalizeDispatchDecoderUses(
    llvm::Function &, llvm::SwitchInst &selectedOriginalSwitch,
    const DispatchDecoderSSAOptions & = {});
} // namespace dragon::llvmlifter::detail
