#include <dragon/disassembler/x86/x86Info.hpp>

#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrDesc.h>
#include <llvm/MC/MCRegisterInfo.h>

#include "InstructionImpl.hpp"


dragon::X86Info::X86Info(const Architecture architecture,
                         const llvm::MCInstrInfo &instrInfo,
                         const llvm::MCRegisterInfo &regInfo) :
    architecture_(architecture) {
  for (unsigned i = 0; i < instrInfo.getNumOpcodes(); ++i) {
    const std::string_view name = instrInfo.getName(i);

    // Use LLVM's own trap flag instead of relying on instruction names.
    if (instrInfo.get(i).isTrap()) {
      trapOpcodes_.insert(i);
      continue;
    }

    if (name == "PUSH64r")
      opPush64r_ = i;
    else if (name == "PUSH32r")
      opPush32r_ = i;
    else if (name == "MOV64rr")
      opMov64rr_ = i;
    else if (name == "MOV32rr")
      opMov32rr_ = i;
    else if (name == "POP64r")
      opPop64r_ = i;
    else if (name == "POP32r")
      opPop32r_ = i;
    else if (name == "LEAVE64")
      opLeave64_ = i;
    else if (name == "LEAVE")
      opLeave_ = i;
    else if (name == "RET64")
      opRet64_ = i;
    else if (name == "RETL" || name == "RET32")
      opRetL_ = i;
    else if (name == "RETI64")
      opRetI64_ = i;
    else if (name == "RETIL" || name == "RETI32")
      opRetIL_ = i;
    else if (name == "ADD64ri8" || name == "ADD64ri32")
      addSP64Opcodes_.insert(i);
    else if (name == "ADD32ri8" || name == "ADD32ri")
      addSP32Opcodes_.insert(i);
    else if (name == "HLT")
      opHlt_ = i;
    else if (name == "ENDBR64")
      opEndbr64_ = i;
    else if (name.starts_with("NOOP") || name == "FNOP")
      nopOpcodes_.insert(i);
    else if (name.starts_with("IRET"))
      iretOpcodes_.insert(i);
  }

  for (unsigned i = 0; i < regInfo.getNumRegs(); ++i) {
    const std::string_view name = regInfo.getName(i);
    if (name == "RBP")
      regRBP_ = i;
    else if (name == "RSP")
      regRSP_ = i;
    else if (name == "EBP")
      regEBP_ = i;
    else if (name == "ESP")
      regESP_ = i;
  }
}

size_t dragon::X86Info::isCommonFunctionPrologue(
        const std::span<const Instruction *> insns) const {
  if (architecture_ == Architecture::X86_32)
    return x8632bitIsCommonPrologue(insns);
  if (architecture_ == Architecture::X86_64)
    return x8664bitIsCommonPrologue(insns);
  return 0;
}

size_t dragon::X86Info::isCommonFunctionEpilogue(
        const std::span<const Instruction *> insns) const {
  if (architecture_ == Architecture::X86_32)
    return x8632bitIsCommonEpilogue(insns);
  if (architecture_ == Architecture::X86_64)
    return x8664bitIsCommonEpilogue(insns);
  return 0;
}

size_t dragon::X86Info::x8664bitIsCommonPrologue(
        const std::span<const Instruction *> insns) const {
  if (insns.empty())
    return 0;

  size_t offset = 0;

  // endbr64 alone is already a valid CET landing pad / function entry.
  // If followed by push rbp / mov rbp, rsp the full frame setup is consumed.
  if (const auto *mc0 = insns[0]->mcInst();
      mc0 && mc0->getOpcode() == opEndbr64_) {
    offset = 1;
  }

  // Try to match push rbp / mov rbp, rsp starting at offset.
  if (insns.size() >= offset + 2) {
    const auto *push = insns[offset]->mcInst();
    const auto *mov = insns[offset + 1]->mcInst();
    if (push && mov && push->getOpcode() == opPush64r_ &&
        push->getNumOperands() >= 1 &&
        push->getOperand(0).getReg() == regRBP_ &&
        mov->getOpcode() == opMov64rr_ && mov->getNumOperands() >= 2 &&
        mov->getOperand(0).getReg() == regRBP_ &&
        mov->getOperand(1).getReg() == regRSP_) {
      return offset + 2;
    }
  }

  // endbr64 alone - still a function entry, skip just that one instruction.
  return offset;
}

size_t dragon::X86Info::x8664bitIsCommonEpilogue(
        const std::span<const Instruction *> insns) const {
  if (insns.empty())
    return 0;

  const auto *i0 = insns[0]->mcInst();
  if (!i0)
    return 0;

  // Single-instruction endings.
  const unsigned op0 = i0->getOpcode();
  if (op0 == opRet64_ || op0 == opRetI64_)
    return 1;
  if (op0 == opHlt_)
    return 1;
  if (trapOpcodes_.contains(op0))
    return 1;

  if (insns.size() < 2)
    return 0;

  const auto *i1 = insns[1]->mcInst();
  if (!i1)
    return 0;

  // All 2-instruction patterns require ret[N] as the second instruction.
  const unsigned op1 = i1->getOpcode();
  if (op1 != opRet64_ && op1 != opRetI64_)
    return 0;

  // pop rbp ; ret[N]
  if (op0 == opPop64r_ && i0->getNumOperands() >= 1 &&
      i0->getOperand(0).getReg() == regRBP_)
    return 2;

  // leave ; ret[N]
  if (op0 == opLeave64_)
    return 2;

  // add rsp, N ; ret[N]  (frameless function with locals)
  if (addSP64Opcodes_.contains(op0) && i0->getNumOperands() >= 1 &&
      i0->getOperand(0).getReg() == regRSP_)
    return 2;

  return 0;
}

size_t dragon::X86Info::x8632bitIsCommonPrologue(
        const std::span<const Instruction *> insns) const {
  if (insns.size() < 2)
    return 0;
  const auto *i0 = insns[0]->mcInst();
  const auto *i1 = insns[1]->mcInst();
  if (!i0 || !i1)
    return 0;
  // push ebp
  if (i0->getOpcode() != opPush32r_)
    return 0;
  if (i0->getNumOperands() < 1 || i0->getOperand(0).getReg() != regEBP_)
    return 0;
  // mov ebp, esp
  if (i1->getOpcode() != opMov32rr_)
    return 0;
  if (i1->getNumOperands() < 2)
    return 0;
  if (i1->getOperand(0).getReg() != regEBP_)
    return 0;
  if (i1->getOperand(1).getReg() != regESP_)
    return 0;
  return 2;
}

size_t dragon::X86Info::x8632bitIsCommonEpilogue(
        const std::span<const Instruction *> insns) const {
  if (insns.empty())
    return 0;

  const auto *i0 = insns[0]->mcInst();
  if (!i0)
    return 0;

  // Single-instruction endings.
  const unsigned op0 = i0->getOpcode();
  if (op0 == opRetL_ || op0 == opRetIL_) // ret N covers stdcall / fastcall
    return 1;
  if (op0 == opHlt_)
    return 1;
  if (trapOpcodes_.contains(op0))
    return 1;

  if (insns.size() < 2)
    return 0;

  const auto *i1 = insns[1]->mcInst();
  if (!i1)
    return 0;

  // All 2-instruction patterns require ret[N] as the second instruction.
  const unsigned op1 = i1->getOpcode();
  if (op1 != opRetL_ && op1 != opRetIL_)
    return 0;

  // pop ebp ; ret[N]
  if (op0 == opPop32r_ && i0->getNumOperands() >= 1 &&
      i0->getOperand(0).getReg() == regEBP_)
    return 2;

  // leave ; ret[N]
  if (op0 == opLeave_)
    return 2;

  // add esp, N ; ret[N]  (cdecl frameless function with locals)
  if (addSP32Opcodes_.contains(op0) && i0->getNumOperands() >= 1 &&
      i0->getOperand(0).getReg() == regESP_)
    return 2;

  return 0;
}

bool dragon::X86Info::isTrap(const Instruction &insn) const {
  const auto *mc = insn.mcInst();
  return mc && trapOpcodes_.contains(mc->getOpcode());
}

bool dragon::X86Info::isNop(const Instruction &insn) const {
  const auto *mc = insn.mcInst();
  return mc && nopOpcodes_.contains(mc->getOpcode());
}

bool dragon::X86Info::isInterruptReturn(const Instruction &insn) const {
  const auto *mc = insn.mcInst();
  return mc && iretOpcodes_.contains(mc->getOpcode());
}

bool dragon::X86Info::isHalt(const Instruction &insn) const {
  const auto *mc = insn.mcInst();
  return mc && mc->getOpcode() == opHlt_;
}
