#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace dragon::llvmlifter::detail {

// Opt-in C++ custom-pass component. These are candidates, not facts from ELF section
// permissions or a caller-authored metadata certificate. Admission rechecks the
// executable entry guards and a joint base/step memory-preservation invariant.
struct ImageTableContextRequest {
  std::string viewSymbol;
  std::uint64_t tableOffset = 0;
  std::vector<std::uint64_t> targets;
  std::size_t maxBlocks = 4096;
  std::size_t maxInstructions = 65536;
  std::size_t maxWork = 1000000;
};

struct ImageTableContextStats {
  bool changed = false;
  bool closed = false;
  bool budgetExhausted = false;
  std::size_t guardedBytes = 0;
  std::size_t preservedStores = 0;
  std::size_t constantValues = 0;
  std::size_t work = 0;
  std::string message;
};

// Requires the raw independent GuardedEntry view, before its register allocas
// and external-entry switch have been optimized. Only its named view function
// can change. Rejected proofs run entirely in owned module clones.
//
// v1: one scalar u64 absolute table; index scale eight, add/disjoint-or base;
// actual runtime equality for the full table and the initial x86-64 SP. Every
// access must be within the table under its original path conditions. Every
// write on a path to another table read must preserve those bytes. Unknown
// calls/aliases/ranges refuse closure. Terminal effects remain executable.
//
// A successful rewrite uses the actual index, not a predicted value. Its
// default retains the original target LOAD/switch. Existing bounds, errors,
// flags and business effects are not replaced. This is not a full CFF-removal
// claim; later control-flow consumers still have to meet that precision gate.
ImageTableContextStats runImageTableCheckedContext(
    llvm::Module &, const ImageTableContextRequest &);

// Explicit opt-in proof consumer. The two-argument overload remains the
// original full-target-switch fallback mode. With consumeClosedCFG=true,
// publish the candidate table CFG only after the identical complete base/step
// closure: its actual target LOAD and original UnknownTarget exit remain,
// while proved-impossible old target-switch case edges are removed. Neither
// metadata nor candidate targets alone can authorize this transformation.
ImageTableContextStats runImageTableCheckedContext(
    llvm::Module &, const ImageTableContextRequest &, bool consumeClosedCFG);

// Named spelling for callers that explicitly consume the full closure.
ImageTableContextStats runClosedImageTableContext(
    llvm::Module &, const ImageTableContextRequest &);

} // namespace dragon::llvmlifter::detail
