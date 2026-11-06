#pragma once
#include <cstdint>
#include <vector>
namespace dragon { class Graph; }
namespace dragon::analysis::detail {
// A normal-return effect, derived afresh from the current graph. Not a promise
// of termination, fault freedom, ABI preservation or a callee result value.
struct LeafCallProof {
  bool proven = false;
  std::uint32_t operations = 0;
  std::vector<std::uint32_t> clobberedBytes;
};
LeafCallProof proveLeafCallee(const Graph &,std::uint64_t entry,
                            std::uint32_t maxBlocks,std::uint32_t maxOperations);
}
