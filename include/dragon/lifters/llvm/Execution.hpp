#pragma once
#include <dragon/lifters/llvm/Execution.h>
#include <cstdint>
namespace dragon::llvmlifter {
using GuestStateV2 = dragon_guest_state_v2_t;
using GuestMemoryV2 = dragon_guest_memory_v2_t;
using ExecContextV2 = dragon_exec_context_v2_t;
using ExecExitV2 = dragon_exec_exit_v2_t;
using TargetV2 = dragon_target_v2_t;
using TargetEntryV2 = dragon_target_entry_v2_t;
// Dispatch from the PC in state. Missing implementations and exhausted budgets
// are explicit exits. Return only completes the root at rootExitPC.
void executeV2(GuestStateV2 &state, GuestMemoryV2 &memory,
               ExecContextV2 &context, ExecExitV2 &exit, std::uint64_t rootExitPC);
}
