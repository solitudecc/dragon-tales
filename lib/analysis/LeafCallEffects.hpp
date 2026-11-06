#pragma once
#include <cstdint>
#include <vector>
#include <utility>
namespace dragon { class Graph; }
namespace dragon::analysis::detail {
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
                            std::uint32_t maxFacts);
}
