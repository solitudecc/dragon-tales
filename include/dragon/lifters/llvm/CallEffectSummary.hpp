#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class CallBase;
}

namespace dragon::llvmlifter {

/// The kind of boundary represented by an explicit, caller-supplied effect
/// summary.  A syscall number is required for SystemCall summaries; an
/// unknown syscall or external call has no summary and remains a barrier.
enum class CallBoundaryKind : std::uint8_t { ExternalFunction = 0, SystemCall = 1 };

/// Memory precision supplied by a trusted summary. KnownRanges is a complete
/// write footprint after a normal return. Guest-memory SSA consumes
/// GuestMemory ranges and complete constants. Other bases stay barriers until
/// a consumer proves their address relation. A call's postconditions never
/// establish its callee's entry state.
enum class CallMemoryEffectKind : std::uint8_t {
  Unknown = 0,
  NoMemory = 1,
  ReadOnly = 2,
  KnownRanges = 3,
};

struct CallMemoryRange {
  enum class Base : std::uint8_t {
    GuestMemory = 0,
    Stack = 1,
    Absolute = 2,
    GuestState = 3,
  };
  Base base = Base::GuestMemory;
  std::uint64_t offset = 0;
  std::uint32_t bytes = 0;
};

/// A precise constant write made by a call.  The value is the little-endian
/// raw bits for the complete range (at most 64 bits); incomplete facts are
/// retained in metadata but are never consumed by an analysis.
struct CallMemoryConstant {
  CallMemoryRange range;
  std::uint64_t value = 0;
  bool complete = false;
};

/// A bounded scalar result effect. The value is optional because a summary may
/// describe only the destination location (for example errno) without
/// pretending to know its value.
struct CallValueEffect {
  enum class Location : std::uint8_t { ReturnRegister = 0, Errno = 1 };
  Location location = Location::ReturnRegister;
  std::uint32_t index = 0;     ///< ABI register index; ignored for Errno.
  std::uint32_t widthBits = 0; ///< Zero means location-only, otherwise 1..128.
  bool hasConstant = false;
  std::uint64_t constant = 0;
};

struct CallEffectSummary {
  std::uint32_t version = 1;
  CallBoundaryKind boundary = CallBoundaryKind::ExternalFunction;
  bool hasSyscallNumber = false;
  std::uint64_t syscallNumber = 0;
  bool proven = false;
  CallMemoryEffectKind memory = CallMemoryEffectKind::Unknown;
  std::vector<CallMemoryRange> reads;
  std::vector<CallMemoryRange> writes;
  std::vector<CallMemoryConstant> memoryConstants;
  std::vector<CallValueEffect> values;
};

/// Attach a bounded, explicit summary to one LLVM call. Invalid summaries are
/// rejected and do not modify the call. This API never infers effects from a
/// function name, linkage, import, or one observed execution.
bool attachCallEffectSummary(llvm::CallBase &, const CallEffectSummary &);

/// Decode a summary previously attached by attachCallEffectSummary(). Missing,
/// malformed, unsupported or unproven metadata returns std::nullopt.
std::optional<CallEffectSummary> getCallEffectSummary(const llvm::CallBase &);

} // namespace dragon::llvmlifter
