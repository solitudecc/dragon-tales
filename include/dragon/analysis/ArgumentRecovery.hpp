#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <dragon/Architecture.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

namespace dragon {
class Function;
class Graph;
} // namespace dragon

/// Function-signature recovery: which registers a function reads as inputs
/// (its arguments) and whether it produces a result (its return value).
///
/// This is the forward counterpart to BackwardSlicer. The slicer answers "what
/// does this one value depend on"; argument recovery answers "what does this
/// whole function consume and produce", which is what turns a lifted blob into
/// something with a real prototype - the first step toward readable,
/// reconstructed code.
///
/// The core is an ordinary live-variable analysis over the function's IGNIL
/// CFG: a register that is read along some path from the entry before it is
/// written is *live-in*, i.e. the function expects a caller to have set it.
/// Intersecting the live-in set with the target's calling convention (which
/// registers carry the first N integer arguments) yields the argument list;
/// the convention's return register, if the body ever writes it, yields the
/// return value.
///
/// Scope and documented limitations (this recovers the common integer/pointer
/// ABI shape, not every corner of it):
///   - Integer class only. Floating-point/vector arguments (XMM/SIMD), structs
///     passed in registers, and stack-passed arguments beyond the register
///     count are not recovered.
///   - Register-offset granularity. A read of EDI counts as a use of the RDI
///     slot; sub-register width is not tracked through the dataflow (it is only
///     consulted to size the return value).
///   - Arguments are reported as the contiguous convention prefix up to the
///     highest live-in argument register, matching how compilers allocate them.
///   - Conventions modelled: x86-64 System V, Microsoft x64 (selected by the
///     graph's Configuration::callingConvention, which disassembleImage sets
///     for an x86-64 PE) and AArch64 AAPCS64.
namespace dragon::analysis {

/// Register-space byte offsets that are live on entry to `fn`: read along some
/// path before being written. Argument registers among these are the
/// function's parameters; RSP/RBP/flags appear here too and are filtered out by
/// recoverSignature(). Empty if `fn` is invalid or has no IGNIL.
[[nodiscard]] std::set<std::uint32_t>
liveInRegisters(const Function &fn, const Graph &graph);

/// One recovered argument: the register that carries it and its position in the
/// calling convention (0-based).
struct RecoveredArgument {
  ignil::Reg reg;
  unsigned index;
};

/// A recovered function prototype.
struct FunctionSignature {
  std::vector<RecoveredArgument> arguments;
  /// The return register (with its observed width), or nullopt for void - the
  /// convention's return register is never written in the body.
  std::optional<ignil::Reg> returnValue;

  [[nodiscard]] bool returnsVoid() const { return !returnValue.has_value(); }
};

/// Recover `fn`'s signature for `arch`. Returns an all-void, no-argument
/// signature for an architecture whose convention is not modelled, or for an
/// invalid function.
[[nodiscard]] FunctionSignature
recoverSignature(const Function &fn, const Graph &graph, Architecture arch);

/// A C-like prototype string, e.g. `i32 sub_1000(i32 RDI, i32 RSI)`. Widths are
/// rendered as LLVM integer types; argument names are the register names.
[[nodiscard]] std::string toString(const FunctionSignature &sig,
                                   std::uint64_t address);

} // namespace dragon::analysis
