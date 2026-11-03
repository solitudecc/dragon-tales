#include <dragon/lifters/llvm/Execution.hpp>
#include <dragon/Architecture.hpp>
#include <bit>
#include <cstring>

namespace dragon::llvmlifter {
void executeV2(GuestStateV2 &state, GuestMemoryV2 &memory,
               ExecContextV2 &context, ExecExitV2 &exit, std::uint64_t rootExitPC) {
  const auto arch = state.architecture;
  const bool a64 = arch == static_cast<std::uint64_t>(Architecture::ARM64);
  const bool x86 = arch == static_cast<std::uint64_t>(Architecture::X86_32) || arch == static_cast<std::uint64_t>(Architecture::X86_64);
  const auto pcOffset = a64 ? 264u : 128u;
  auto fail = [&](std::uint64_t reason, std::uint64_t detail) {
    exit.reason = reason; exit.detail = detail;
  };
  exit = {};
  if (sizeof(void *) != 8 || std::endian::native != std::endian::little ||
      state.abi_version != 2 || (!a64 && !x86) || !state.regs_bytes ||
      state.regs_size < (a64 ? 788u : 488u) || !memory.data ||
      (context.entry_count && !context.entries)) {
    fail(DRAGON_EXIT_UNSUPPORTED, DRAGON_DETAIL_INVALID_STATE); return;
  }
  std::uint64_t pc;
  std::memcpy(&pc, state.regs_bytes + pcOffset, 8);
  exit.source_pc = exit.next_pc = pc; exit.next_pc_valid = 1;
  while (true) {
    if (!context.budget) { fail(DRAGON_EXIT_BUDGET, 0); return; }
    TargetV2 target = nullptr;
    for (std::uint64_t i = 0; i < context.entry_count; ++i)
      if (context.entries[i].guest_address == pc) {
        target = context.entries[i].implementation; break;
      }
    if (!target) { fail(DRAGON_EXIT_NEED_TARGET, 0); return; }
    --context.budget;
    // A model that forgets to produce an exit must not reuse an earlier one.
    exit.reason = 0; exit.next_pc_valid = 0;
    target(&state, &memory, &context, &exit);
    std::memcpy(&pc, state.regs_bytes + pcOffset, 8);
    if (exit.reason < DRAGON_EXIT_RETURN || exit.reason > DRAGON_EXIT_BUDGET ||
        exit.next_pc_valid > 1 ||
        ((exit.reason == DRAGON_EXIT_RETURN || exit.reason == DRAGON_EXIT_TRANSFER || exit.reason == DRAGON_EXIT_NEED_TARGET) && !exit.next_pc_valid) ||
        (exit.reason == DRAGON_EXIT_UNKNOWN_TARGET && exit.next_pc_valid) ||
        (exit.next_pc_valid && exit.next_pc != pc)) {
      exit = {DRAGON_EXIT_UNSUPPORTED, DRAGON_TRANSFER_NONE, pc, pc, 1, 0,
              DRAGON_DETAIL_INVALID_EXIT};
      return;
    }
    if (exit.reason == DRAGON_EXIT_RETURN && exit.next_pc_valid && pc == rootExitPC) return;
    if ((exit.reason != DRAGON_EXIT_TRANSFER && exit.reason != DRAGON_EXIT_RETURN) ||
        !exit.next_pc_valid) return;
  }
}
}
