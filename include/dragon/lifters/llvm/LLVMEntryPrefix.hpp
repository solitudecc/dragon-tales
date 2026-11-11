#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dragon::llvmlifter {
/// Explicit opt-in module-wide prefix-copy budgets; no existing pass changes.
struct EntryPrefixOptions {
  std::uint32_t maxBlocks = 32; ///< At most 4096; zero disables copies.
  std::uint32_t maxInstructions = 2048; ///< At most 65536; zero disables copies.
};
struct EntryPrefixStats {
  bool available = false;
  bool complete = true; ///< False on any rejected candidate or exhausted budget.
  bool changed = false;
  std::size_t candidateEdges = 0;
  std::size_t peeledEdges = 0;
  std::size_t clonedBlocks = 0;
  std::size_t clonedInstructions = 0; ///< Copied-region instructions plus new exit PHIs; not net IR growth.
  std::size_t originalExits = 0;
  std::size_t rejected = 0;
  std::size_t budgetExhausted = 0;
  std::size_t skippedSingleEntry = 0;
  std::size_t internalEdges = 0;
  std::size_t backedges = 0;
  std::size_t terminalEdges = 0;
  std::string message;
};
}
