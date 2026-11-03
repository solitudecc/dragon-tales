#pragma once
#include <stdint.h>

/* V2 host ABI: 64-bit little-endian hosts; all objects and mappings are
 * caller-owned for the entire execution. Guest addresses index memory.data.
 * No host ABI assumptions are made about guest functions. */
typedef enum dragon_llvm_execution_mode {
  DRAGON_LLVM_LEGACY = 0, DRAGON_LLVM_STATEFUL_V2 = 1
} dragon_llvm_execution_mode_t;
typedef enum dragon_exec_reason_v2 {
  DRAGON_EXIT_RETURN = 1, DRAGON_EXIT_TRANSFER = 2,
  DRAGON_EXIT_NEED_TARGET = 3, DRAGON_EXIT_UNKNOWN_TARGET = 4,
  DRAGON_EXIT_UNSUPPORTED = 5, DRAGON_EXIT_BUDGET = 6
} dragon_exec_reason_v2_t;
typedef enum dragon_transfer_kind_v2 {
  DRAGON_TRANSFER_NONE = 0, DRAGON_TRANSFER_BRANCH = 1,
  DRAGON_TRANSFER_CALL = 2, DRAGON_TRANSFER_RETURN = 3
} dragon_transfer_kind_v2_t;
typedef enum dragon_exec_detail_v2 {
  DRAGON_DETAIL_NONE = 0, DRAGON_DETAIL_INVALID_STATE = 1,
  DRAGON_DETAIL_INSTRUCTION = 2, DRAGON_DETAIL_MEMORY = 3,
  DRAGON_DETAIL_INVALID_EXIT = 4, DRAGON_DETAIL_ENTRY = 5
} dragon_exec_detail_v2_t;
typedef struct dragon_guest_state_v2 {
  uint64_t abi_version, architecture, regs_size;
  uint8_t *regs_bytes;
} dragon_guest_state_v2_t;
typedef struct dragon_guest_memory_v2 {
  uint8_t *data;
  uint64_t size;
} dragon_guest_memory_v2_t;
typedef struct dragon_exec_exit_v2 {
  uint64_t reason, transfer_kind, source_pc, next_pc;
  uint64_t next_pc_valid, return_pc, detail;
} dragon_exec_exit_v2_t;
typedef struct dragon_exec_context_v2 dragon_exec_context_v2_t;
typedef void (*dragon_target_v2_t)(dragon_guest_state_v2_t *,
    dragon_guest_memory_v2_t *, dragon_exec_context_v2_t *, dragon_exec_exit_v2_t *);
typedef struct dragon_target_entry_v2 {
  uint64_t guest_address;
  dragon_target_v2_t implementation;
} dragon_target_entry_v2_t;
struct dragon_exec_context_v2 {
  const dragon_target_entry_v2_t *entries;
  uint64_t entry_count, budget;
  void *user_data;
};
