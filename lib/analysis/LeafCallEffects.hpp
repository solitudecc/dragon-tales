#pragma once
#include <dragon/Architecture.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <compare>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>
namespace dragon { class Graph; }
namespace dragon::analysis::detail {
// Already-proved caller facts, normalized to callee-entry SP. These are byte
// constraints, not architectural writes; omitted bytes remain arbitrary.
struct CallBinding {
  std::uint32_t offset = 0;
  ignil::Width width = 0;
  enum Kind { Constant, EntrySPRelative } kind = Constant;
  std::uint64_t value = 0;
  auto operator<=>(const CallBinding &) const = default;
};
struct CallContext {
  std::vector<CallBinding> bindings;
  // Optional actual callee-entry SP, used only to recognize numeric addresses.
  // The symbolic value of SP itself remains EntrySPRelative(0).
  std::optional<std::uint64_t> entrySP;
  auto operator<=>(const CallContext &) const = default;
};
// One fact per binding descriptor plus its bytes, and one for a present entrySP.
// Returns 8193 when the hard context cap is exceeded, without unbounded work.
std::uint32_t contextFacts(const CallContext &);
// Bounded, canonical owned contexts: only actual x86 register-file extents;
// nonoverlapping whole-byte constraints; no SP/RIP bytes. False means reject.
bool canonicalizeCallContext(CallContext &,Architecture);
// A normal-return effect, derived afresh from the current graph. Not a promise
// of termination, fault freedom, ABI preservation or a callee result value.
struct LeafCallProof {
  bool proven = false;
  std::uint32_t operations = 0;
  std::vector<std::uint32_t> clobberedBytes;
  // Byte offsets relative to callee-entry SP, modulo the machine pointer width.
  // All writes count, including saved registers and subsequently overwritten slots.
  std::vector<std::uint64_t> stackWriteBytes;
  // Constants agreed by every normal return; absence never means unchanged.
  std::vector<std::pair<std::uint32_t,std::uint8_t>> registerConstants;
  std::vector<std::pair<std::uint64_t,std::uint8_t>> memoryConstants;
};
LeafCallProof proveLeafCallee(const Graph &,std::uint64_t entry,
                            std::uint32_t maxBlocks,std::uint32_t maxOperations,
                            std::uint32_t maxFacts,const CallContext & = {});
}
