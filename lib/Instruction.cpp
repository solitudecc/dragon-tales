#include <dragon/disassembler/Instruction.hpp>

// Must come after Instruction.hpp so that Instruction::Impl is defined before
// unique_ptr needs to destroy it.
#include "InstructionImpl.hpp"

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------

dragon::Instruction::Instruction(std::uint64_t address, std::uint8_t size,
                                 std::string mnemonic, std::string operands,
                                 std::vector<std::uint8_t> bytes, bool isReturn,
                                 bool isCall, bool isJump, bool isConditional,
                                 bool isTrap, bool hasIndirectTarget) :
    address_(address), size_(size), mnemonic_(std::move(mnemonic)),
    operands_(std::move(operands)), bytes_(std::move(bytes)),
    isReturn_(isReturn), isCall_(isCall), isJump_(isJump),
    isConditional_(isConditional), isTrap_(isTrap),
    hasIndirectTarget_(hasIndirectTarget) {
  disassembly_ = mnemonic_;
  if (!operands_.empty()) {
    disassembly_ += ' ';
    disassembly_ += operands_;
  }
}

// -----------------------------------------------------------------------
// Special members - all defined here so Impl is complete at point of use
// -----------------------------------------------------------------------

// Destructor: unique_ptr<Impl> needs Impl to be complete to call its deleter.
dragon::Instruction::~Instruction() = default;

// Move: unique_ptr is movable; trivially correct.
dragon::Instruction::Instruction(Instruction &&) noexcept = default;
dragon::Instruction &
dragon::Instruction::operator=(Instruction &&) noexcept = default;

// Copy: deep-copy the Impl so each Instruction owns its own MCInst.
// impl_ may be null for instructions that failed to decode - handle that case.
dragon::Instruction::Instruction(const Instruction &other) :
    address_(other.address_), size_(other.size_), mnemonic_(other.mnemonic_),
    operands_(other.operands_), disassembly_(other.disassembly_),
    pattern_(other.pattern_), bytes_(other.bytes_),
    chromosomeMask_(other.chromosomeMask_), branches_(other.branches_),
    inferredBranches_(other.inferredBranches_),
    callees_(other.callees_), edges_(other.edges_),
    isBlockStart_(other.isBlockStart_),
    isFunctionStart_(other.isFunctionStart_), isPrologue_(other.isPrologue_),
    isReturn_(other.isReturn_), isCall_(other.isCall_), isJump_(other.isJump_),
    isConditional_(other.isConditional_), isTrap_(other.isTrap_),
    hasIndirectTarget_(other.hasIndirectTarget_),
    impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

dragon::Instruction &dragon::Instruction::operator=(const Instruction &other) {
  if (this == &other)
    return *this;

  address_ = other.address_;
  size_ = other.size_;
  mnemonic_ = other.mnemonic_;
  operands_ = other.operands_;
  disassembly_ = other.disassembly_;
  pattern_ = other.pattern_;
  bytes_ = other.bytes_;
  chromosomeMask_ = other.chromosomeMask_;
  branches_ = other.branches_;
  inferredBranches_ = other.inferredBranches_;
  callees_ = other.callees_;
  edges_ = other.edges_;
  isBlockStart_ = other.isBlockStart_;
  isFunctionStart_ = other.isFunctionStart_;
  isPrologue_ = other.isPrologue_;
  isReturn_ = other.isReturn_;
  isCall_ = other.isCall_;
  isJump_ = other.isJump_;
  isConditional_ = other.isConditional_;
  isTrap_ = other.isTrap_;
  hasIndirectTarget_ = other.hasIndirectTarget_;
  impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;

  return *this;
}

const llvm::MCInst *dragon::Instruction::mcInst() const {
  return impl_ ? (&impl_->inst) : nullptr;
}
