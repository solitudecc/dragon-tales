#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
// Forward declaration so we can use it here
// internally we need to use InstructionImpl.hpp
// to retrieve the real value
class MCInst;
} // namespace llvm

namespace dragon {

/// Architecture-agnostic representation of a single decoded instruction.
///
/// Instances are created and fully populated by the Disassembler. The
/// constructor takes the fields known at decode time; structural flags
/// (block/function start, prologue) and the chromosome mask are filled in
/// afterwards via setters once the surrounding CFG is understood.
///
/// String fields (mnemonic, operands, disassembly, pattern) are stored as
/// std::string internally and exposed as std::string_view - the views are
/// valid for the lifetime of the Instruction.
///
/// The opaque Impl holds the raw LLVM MCInst. It is null for instructions
/// that failed to decode; callers should not assume it is non-null. Internal
/// code that needs the MCInst directly should include InstructionImpl.hpp.
class Instruction {
  public:
  /// Load address of the first byte of this instruction.
  [[nodiscard]] std::uint64_t address() const { return address_; }

  /// Encoded size in bytes.
  [[nodiscard]] std::uint8_t size() const { return size_; }

  /// Instruction mnemonic, e.g. "mov", "jmp", "call".
  [[nodiscard]] std::string_view mnemonic() const { return mnemonic_; }

  /// Operand string as printed by the disassembler, e.g. "rax, rbx".
  [[nodiscard]] std::string_view operands() const { return operands_; }

  /// Full disassembly text (mnemonic + operands), e.g. "mov rax, rbx".
  [[nodiscard]] std::string_view disassembly() const { return disassembly_; }

  /// Normalised pattern string used for byte-level matching, where
  /// address-dependent bytes are replaced with wildcards.
  [[nodiscard]] std::string_view pattern() const { return pattern_; }

  /// Raw encoded bytes of this instruction.
  [[nodiscard]] const std::vector<std::uint8_t> &bytes() const {
    return bytes_;
  }

  /// Wildcard mask parallel to bytes(): 0x00 = fixed byte, 0xFF = wildcard.
  /// Used by pattern-matching / chromosome search routines.
  [[nodiscard]] const std::vector<std::uint8_t> &chromosomeMask() const {
    return chromosomeMask_;
  }

  /// Explicit decoder/caller targets and currently inferred indirect targets.
  /// Inference is retractable on reanalysis; explicit caller edges are trusted
  /// input evidence. An empty indirect set means unresolved, not unreachable.
  [[nodiscard]] const std::set<std::uint64_t> &branches() const {
    return branches_;
  }

  /// Known direct call targets (filled in by the disassembler for calls).
  /// Empty for indirect calls or non-call instructions.
  [[nodiscard]] const std::set<std::uint64_t> &callees() const {
    return callees_;
  }

  /// Number of outgoing CFG edges (0 for non-branch, 1 for unconditional
  /// branch, 2 for conditional branch).
  [[nodiscard]] std::size_t edges() const { return edges_; }

  /// Raw LLVM MCInst, or nullptr if decoding failed. Only meaningful to
  /// code that has included InstructionImpl.hpp.
  [[nodiscard]] const llvm::MCInst *mcInst() const;

  // -- Structural flags -------------------------------------------------------

  /// True if this instruction begins a basic block.
  [[nodiscard]] bool isBlockStart() const { return isBlockStart_; }

  /// True if this instruction begins a function (first instruction of a
  /// detected function entry point).
  [[nodiscard]] bool isFunctionStart() const { return isFunctionStart_; }

  /// True if this instruction is part of a recognised function prologue
  /// sequence (e.g. push rbp / mov rbp, rsp on x86).
  [[nodiscard]] bool isPrologue() const { return isPrologue_; }

  /// True if this instruction returns from the current function (ret / bx lr).
  [[nodiscard]] bool isReturn() const { return isReturn_; }

  /// True if this instruction transfers control to another function (call /
  /// bl). May be direct or indirect.
  [[nodiscard]] bool isCall() const { return isCall_; }

  /// True if this instruction is a branch (conditional or unconditional).
  /// Calls are NOT considered jumps.
  [[nodiscard]] bool isJump() const { return isJump_; }

  /// True if the branch is conditional (e.g. je, jne, b.ne).
  [[nodiscard]] bool isConditional() const { return isConditional_; }

  /// True if this instruction raises an exception or halts execution
  /// unconditionally (ud2, hlt, int3, udf).
  [[nodiscard]] bool isTrap() const { return isTrap_; }

  /// True if the branch or call target is not statically known (e.g.
  /// jmp rax, call [rbx + rcx*8]).
  [[nodiscard]] bool hasIndirectTarget() const { return hasIndirectTarget_; }

  // -- Setters (called by the Disassembler after initial decode) --------------

  void setBlockStart(bool v) { isBlockStart_ = v; }
  void setFunctionStart(bool v) { isFunctionStart_ = v; }
  void setPrologue(bool v) { isPrologue_ = v; }

  /// Register a direct branch target address.
  void addBranch(std::uint64_t addr) {
    branches_.insert(addr);
    inferredBranches_.erase(addr); // Explicit caller/decoder evidence owns it.
  }

  /// Analysis-derived edges are retractable when their proof context changes.
  /// Never replace or erase an explicitly supplied branch with inference.
  void addInferredBranch(std::uint64_t addr) {
    if (branches_.insert(addr).second) inferredBranches_.insert(addr);
  }
  void clearInferredBranches() {
    if (inferredBranches_.empty()) return;
    for (auto address : inferredBranches_) branches_.erase(address);
    inferredBranches_.clear();
    edges_ = branches_.size();
  }
  [[nodiscard]] const std::set<std::uint64_t> &inferredBranches() const {
    return inferredBranches_;
  }

  /// Register a direct call target address.
  void addCallee(std::uint64_t addr) { callees_.insert(addr); }

  void setEdges(std::size_t n) { edges_ = n; }
  void setPattern(std::string p) { pattern_ = std::move(p); }
  void setChromosomeMask(std::vector<std::uint8_t> m) {
    chromosomeMask_ = std::move(m);
  }

  Instruction(std::uint64_t address, std::uint8_t size, std::string mnemonic,
              std::string operands, std::vector<std::uint8_t> bytes,
              bool isReturn, bool isCall, bool isJump, bool isConditional,
              bool isTrap, bool hasIndirectTarget);

  // Declared here so the compiler does not try to destroy the incomplete Impl
  // type in an inline destructor.  Defined in Instruction.cpp after Impl is
  // fully defined via InstructionImpl.hpp.
  ~Instruction();
  Instruction(const Instruction &);
  Instruction &operator=(const Instruction &);
  Instruction(Instruction &&) noexcept;
  Instruction &operator=(Instruction &&) noexcept;

  private:
  /// Opaque LLVM state (MCInst and related data).
  /// Null for instructions that failed to decode.
  /// Internal code can access the full definition by including
  /// InstructionImpl.hpp.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  /// Disassembler creates and populates Instruction objects, including impl_.
  friend class Disassembler;

  std::uint64_t address_{};
  std::uint8_t size_{};

  std::string mnemonic_;
  std::string operands_;
  std::string disassembly_;
  std::string pattern_;

  std::vector<std::uint8_t> bytes_;
  std::vector<std::uint8_t> chromosomeMask_;

  std::set<std::uint64_t> branches_;
  std::set<std::uint64_t> inferredBranches_;
  std::set<std::uint64_t> callees_;
  std::size_t edges_{};

  bool isBlockStart_{};
  bool isFunctionStart_{};
  bool isPrologue_{};
  bool isReturn_{};
  bool isCall_{};
  bool isJump_{};
  bool isConditional_{};
  bool isTrap_{};
  bool hasIndirectTarget_{};
};

} // namespace dragon
