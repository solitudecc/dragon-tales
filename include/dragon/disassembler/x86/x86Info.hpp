#pragma once

#include <dragon/Architecture.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <span>
#include <unordered_set>

#include <llvm/MC/MCInstrInfo.h>


namespace dragon {

/// Instruction classification utilities for the x86 and x86_64 architectures.
///
/// X86Info operates on already decoded Instruction objects, using the mnemonic
/// and operand strings produced by the disassembler. It does not depend on LLVM
/// headers or on the Disassembler class and can be used independently by
/// library users who want to query instruction properties directly.
///
/// Mode dependent methods (prologue and epilogue detection) consult the
/// Architecture value supplied at construction time. All other methods are
/// stateless and are exposed as static members.
class X86Info {
  public:
  /// Constructs an X86Info for the given architecture.
  /// The architecture must be Architecture::X86_32 or Architecture::X86_64.
  explicit X86Info(Architecture architecture,
                   const llvm::MCInstrInfo &instrInfo,
                   const llvm::MCRegisterInfo &regInfo);

  /// Returns the number of instructions consumed if the sequence matches a
  /// common x86 function prologue, or 0 if no match.
  ///
  /// x86_64: endbr64                            -> 1 (landing pad only; not independent entry proof)
  ///         push rbp / mov rbp, rsp            -> 2
  ///         endbr64 / push rbp / mov rbp, rsp  -> 3 (CET + full frame setup)
  /// x86_32: push ebp / mov ebp, esp            -> 2
  [[nodiscard]] size_t
  isCommonFunctionPrologue(std::span<const Instruction *> insns) const;

  /// Returns the number of instructions consumed if the sequence matches a
  /// common x86 function epilogue, or 0 if no match.
  ///
  /// Recognised patterns (both modes, longer matches take priority):
  ///   ret / ret N                   -> 1
  ///   hlt                           -> 1  (kernel idle / OS halt)
  ///   ud2 / int3 / any trap         -> 1  (noreturn sink)
  ///   pop rbp/ebp ; ret[N]          -> 2
  ///   leave[64] ; ret[N]            -> 2
  ///   add rsp/esp, N ; ret[N]       -> 2  (frameless stack cleanup)
  [[nodiscard]] size_t
  isCommonFunctionEpilogue(std::span<const Instruction *> insns) const;

  /// Returns true if the instruction is an unconditional software trap
  /// (int3, ud2, ud1, or any opcode flagged as a trap by LLVM).
  [[nodiscard]] bool isTrap(const Instruction &insn) const;

  /// Returns true if the instruction is any NOP variant
  /// (nop, multi-byte nops, fnop).
  [[nodiscard]] bool isNop(const Instruction &insn) const;

  /// Returns true if the instruction is a return from an interrupt handler
  /// (iret, iretd, iretq).
  [[nodiscard]] bool isInterruptReturn(const Instruction &insn) const;

  /// Returns true if the instruction halts the processor (hlt).
  [[nodiscard]] bool isHalt(const Instruction &insn) const;

  private:
  [[nodiscard]] size_t
  x8664bitIsCommonPrologue(std::span<const Instruction *> insns) const;
  [[nodiscard]] size_t
  x8664bitIsCommonEpilogue(std::span<const Instruction *> insns) const;
  [[nodiscard]] size_t
  x8632bitIsCommonPrologue(std::span<const Instruction *> insns) const;
  [[nodiscard]] size_t
  x8632bitIsCommonEpilogue(std::span<const Instruction *> insns) const;

  Architecture architecture_;

  // Cached opcode integers (filled in constructor by name scan)
  unsigned opPush64r_{}, opPush32r_{};
  unsigned opMov64rr_{}, opMov32rr_{};
  unsigned opPop64r_{}, opPop32r_{};
  unsigned opLeave64_{}, opLeave_{};
  unsigned opRet64_{}, opRetL_{};
  unsigned opRetI64_{}, opRetIL_{}; // ret with immediate (pop N bytes off stack)
  unsigned opHlt_{};
  unsigned opEndbr64_{};

  // Opcode sets for variable-count instruction families
  std::unordered_set<unsigned> trapOpcodes_;    // int3, ud2, ud1, ...
  std::unordered_set<unsigned> nopOpcodes_;     // nop, multibyte nops, fnop
  std::unordered_set<unsigned> iretOpcodes_;    // iret, iretd, iretq
  std::unordered_set<unsigned> addSP64Opcodes_; // add rsp, imm (imm8 and imm32)
  std::unordered_set<unsigned> addSP32Opcodes_; // add esp, imm (imm8 and imm32)

  // Cached register integers
  unsigned regRBP_{}, regRSP_{};
  unsigned regEBP_{}, regESP_{};
};

} // namespace dragon
