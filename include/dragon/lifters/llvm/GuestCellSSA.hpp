#pragma once

#include <cstddef>
#include <string>

namespace llvm {
class Function;
class LoadInst;
class Value;
}

namespace dragon::llvmlifter::detail {

// Selected-original-LOAD helper for C++ custom passes; no module scan or
// pipeline registration. Defaults are also hard ceilings: callers can lower,
// never raise, each cap.
struct GuestCellSSAOptions {
  std::size_t maxBlocks = 128;
  std::size_t maxNodes = 256;
  std::size_t maxPhis = 128;
  std::size_t maxWork = 16384;
};

struct GuestCellSSAStats {
  bool changed = false;
  bool refused = false;
  bool budgetExhausted = false;
  std::size_t blocks = 0;
  std::size_t nodes = 0;
  // Reserved planned PHIs, including on a late read-only refusal.
  std::size_t phis = 0;
  std::size_t work = 0;
  std::string message;
};

// Preconditions: valid LLVM IR, and selectedOriginalLoad was snapshotted by the
// caller before its first mutation (cloned/new LOADs are not candidates). All
// three arguments belong to the same function/module; guestBase is the exact
// pointer SSA identity, not an alias/noalias or immutable-memory assertion.
//
// Supports simple integer i8/16/32/64/128 cells at constant modular-i64 GEP
// offsets. Every actual predecessor slot must reach a nearest exact STORE;
// other-base/partial/unknown writes, calls, fences, volatile/atomic operations,
// and a reverse definition-graph cycle without a STORE refuse. STORE values
// remain their original SSA values, including loop-carried arithmetic/PHIs.
// No value/range/definedness or table-immutability facts are exported.
//
// Success builds fully covered PHIs, replaces and erases only the selected
// LOAD. CFG, Function/Argument identity, all original STOREs and side effects
// remain. Every refusal, including late budget/dominance failure, is read-only.
GuestCellSSAStats forwardGuestCellLoad(
    llvm::Function &, llvm::Value &guestBase,
    llvm::LoadInst &selectedOriginalLoad,
    const GuestCellSSAOptions & = {});

} // namespace dragon::llvmlifter::detail
