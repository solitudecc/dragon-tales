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

// Explicit bounded proof profile. Expanded raises the original-PHI projection
// ceiling to 4096 nodes / 128 depth / 1048576 work and local Guest range
// reasoning to 4096 nodes / 128 depth / 1024 memory-definition points.
// Expanded also permits an explicit request.maxWork up to 16000000; its default
// stays 1000000. Default and the old overload retain 256/32/65536 PHI limits,
// 128/32/128 Guest-range limits, and shared hard 1000000 work. Proof obligations,
// effect preservation and publication checks are unchanged. Expanded may also
// refine byte-width truncation masks in the second original-STORE query;
// independent anchor queries and small low-ones masks remain coarse.
enum class LoopImageProofProfile { Default, Expanded };
// Single source for the per-profile total work ceiling (request.maxWork and
// the guarded-memory consumer). Default stays 1000000.
constexpr std::size_t loopImageMaxWork(LoopImageProofProfile profile) {
  return profile == LoopImageProofProfile::Expanded ? 16000000 : 1000000;
}

// Requires the raw independent GuardedEntry view, before its register allocas
// and external-entry switch have been optimized. Only its named view function
// can change. Rejected proofs run entirely in owned module clones.
//
// v1: one scalar u64 absolute table; index scale eight, add/disjoint-or base;
// actual runtime equality for the full table and the initial x86-64 SP. Every
// access must be within the table under its original path conditions. Every
// write on a path to another table read must preserve those bytes. Unknown
// calls/aliases/ranges refuse closure. Terminal effects remain executable.
// Complete byte-word equality checks on other canonical initial register
// LOADs may constrain those exact SSA snapshots after the accepted guard.
// Partial checks, another LOAD at the same offset, future register values and
// Guest reloads gain no fact from those checks alone. A separate one-word
// induction may use an actual independently constant Guest STORE dominating
// the lookup and each selected reload. Every continuing write must preserve
// both the table and that word; terminal reloads receive no such fact. No
// memory/register noalias assumption is made.
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

// Separate opt-in loop proof. In an owned hypothetical checked-table CFG,
// first prove the existing SP/index control invariant, then thread one complete
// bounded batch of original controller inputs. Every surviving lookup LOAD is
// a cutpoint of a new joint table/constant-cell write proof. Local Guest LOAD
// ranges require complete nearest-STORE coverage; no global index-cell range,
// register alias or definedness fact is assumed. Only the complete verified
// proof body can be published. Rejection preserves the original module.
// The batch and local proof share request.maxWork (hard1M); existing view
// block/instruction caps and all older API layouts/defaults are unchanged.
ImageTableContextStats runLoopImageTableContext(
    llvm::Module &, const ImageTableContextRequest &);

// Explicit profile overload. The original overload retains its exact local
// ceilings and behavior. Unknown enum values refuse without changing IR.
ImageTableContextStats runLoopImageTableContext(
    llvm::Module &, const ImageTableContextRequest &, LoopImageProofProfile);

} // namespace dragon::llvmlifter::detail
