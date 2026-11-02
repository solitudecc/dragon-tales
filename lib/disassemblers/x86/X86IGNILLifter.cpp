#include "disassemblers/x86/X86IGNILLifter.hpp"

#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <span>
#include <tuple>
#include <vector>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <llvm/MC/MCInst.h>
#include <llvm/Support/raw_ostream.h>

// Authoritative IGNIL register address space for the x86 lifter.
// Entries are defined in x86registers.def via the REG(name, offset, width)
// macro.
static const std::unordered_map<std::string_view, dragon::ignil::Reg>
        kRegisterTable = {
#define REG(name, offset, width) {name, {offset, width, name}},
#include "x86registers.def"


#undef REG
};


dragon::x86::X86IGNILLifter::X86IGNILLifter(
        internal::Logger &logger, Architecture architecture,
        const llvm::MCInstrInfo &instrInfo,
        const llvm::MCRegisterInfo &regInfo) :
    logger_(logger), architecture_(architecture), instrInfo_(&instrInfo) {

  // Build LLVM register integer -> ignil::Reg mapping
  for (unsigned i = 0; i < regInfo.getNumRegs(); ++i) {
    std::string_view name = regInfo.getName(i);
    auto it = kRegisterTable.find(name);
    if (it != kRegisterTable.end())
      regMap_.emplace(i, it->second);
  }

  // Flags are not LLVM architectural registers; look them up directly in
  // kRegisterTable so we always get the right IGNIL address regardless of
  // what LLVM's register info contains.
  auto findFlagReg = [](std::string_view name) -> ignil::Reg {
    auto it = kRegisterTable.find(name);
    return it != kRegisterTable.end() ? it->second : ignil::Reg{0, 0};
  };
  // General-purpose registers that are needed structurally (RSP/ESP for
  // push/pop) are also real LLVM registers, so use regMap_ for those.
  auto findGprReg = [&](std::string_view name) -> ignil::Reg {
    for (unsigned i = 0; i < regInfo.getNumRegs(); ++i) {
      if (regInfo.getName(i) == name) {
        if (auto it = regMap_.find(i); it != regMap_.end())
          return it->second;
      }
    }
    return ignil::Reg{0, 0};
  };

  regCF_ = findFlagReg("CF");
  regPF_ = findFlagReg("PF");
  regAF_ = findFlagReg("AF");
  regZF_ = findFlagReg("ZF");
  regSF_ = findFlagReg("SF");
  regOF_ = findFlagReg("OF");
  regDF_ = findFlagReg("DF");
  regRSP_ = findGprReg("RSP");
  regRIP_ = findGprReg("RIP");
  regAH_ = findGprReg("AH");
  for (unsigned i = 0; i < regInfo.getNumRegs(); ++i)
    if (std::string_view(regInfo.getName(i)) == "EFLAGS")
      llvmEflags_ = i;
  regESP_ = findGprReg("ESP");
  regRBP_ = findGprReg("RBP");
  regEBP_ = findGprReg("EBP");
  regCL_ = findGprReg("CL");
  regAL_ = findGprReg("AL");
  regAX_ = findGprReg("AX");
  regEAX_ = findGprReg("EAX");
  regRAX_ = findGprReg("RAX");
  regDX_ = findGprReg("DX");
  regEDX_ = findGprReg("EDX");
  regEBX_ = findGprReg("EBX");
  regECX_ = findGprReg("ECX");
  regCX_ = findGprReg("CX");
  regESI_ = findGprReg("ESI");
  regEDI_ = findGprReg("EDI");
  regRCX_ = findGprReg("RCX");
  regRDX_ = findGprReg("RDX");

  // Scan opcodes by name: map LLVM mnemonic string -> pointer-to-member field.
  using OpcField = unsigned X86IGNILLifter::*;
  static const std::unordered_map<std::string_view, OpcField> kOpcodeFields = {
          {"MOV64rr", &X86IGNILLifter::opMov64rr_},
          {"MOV32rr", &X86IGNILLifter::opMov32rr_},
          {"MOV64ri", &X86IGNILLifter::opMov64ri_},
          {"MOV32ri", &X86IGNILLifter::opMov32ri_},
          {"MOV64ri32", &X86IGNILLifter::opMov64ri32_},
          {"MOV64rm", &X86IGNILLifter::opMov64rm_},
          {"MOV32rm", &X86IGNILLifter::opMov32rm_},
          {"MOV16rm", &X86IGNILLifter::opMov16rm_},
          {"MOV8rm", &X86IGNILLifter::opMov8rm_},
          {"MOV64mr", &X86IGNILLifter::opMov64mr_},
          {"MOV32mr", &X86IGNILLifter::opMov32mr_},
          {"MOV16mr", &X86IGNILLifter::opMov16mr_},
          {"MOV8mr", &X86IGNILLifter::opMov8mr_},
          {"MOV64mi32", &X86IGNILLifter::opMov64mi_},
          {"MOV32mi", &X86IGNILLifter::opMov32mi_},
          {"MOV8mi", &X86IGNILLifter::opMov8mi_},
          {"LEA64r", &X86IGNILLifter::opLea64r_},
          {"LEA32r", &X86IGNILLifter::opLea32r_},
          {"ADD64rr", &X86IGNILLifter::opAdd64rr_},
          {"ADD32rr", &X86IGNILLifter::opAdd32rr_},
          {"ADD64ri32", &X86IGNILLifter::opAdd64ri_},
          {"ADD64ri8", &X86IGNILLifter::opAdd64ri8_},
          {"ADD32ri", &X86IGNILLifter::opAdd32ri_},
          {"ADD32ri8", &X86IGNILLifter::opAdd32ri8_},
          {"ADD64mr", &X86IGNILLifter::opAdd64mr_},
          {"ADD32mr", &X86IGNILLifter::opAdd32mr_},
          {"ADD8mr", &X86IGNILLifter::opAdd8mr_},
          {"ADD64mi32", &X86IGNILLifter::opAdd64mi_},
          {"ADD64mi8", &X86IGNILLifter::opAdd64mi8_},
          {"ADD32mi", &X86IGNILLifter::opAdd32mi_},
          {"ADD32mi8", &X86IGNILLifter::opAdd32mi8_},
          {"ADD64rm", &X86IGNILLifter::opAdd64rm_},
          {"ADD32rm", &X86IGNILLifter::opAdd32rm_},
          {"SUB64rr", &X86IGNILLifter::opSub64rr_},
          {"SUB32rr", &X86IGNILLifter::opSub32rr_},
          {"SUB64ri32", &X86IGNILLifter::opSub64ri_},
          {"SUB64ri8", &X86IGNILLifter::opSub64ri8_},
          {"SUB32ri", &X86IGNILLifter::opSub32ri_},
          {"SUB32ri8", &X86IGNILLifter::opSub32ri8_},
          {"SUB64mr", &X86IGNILLifter::opSub64mr_},
          {"SUB32mr", &X86IGNILLifter::opSub32mr_},
          {"SUB64mi32", &X86IGNILLifter::opSub64mi_},
          {"SUB64mi8", &X86IGNILLifter::opSub64mi8_},
          {"SUB32mi", &X86IGNILLifter::opSub32mi_},
          {"SUB32mi8", &X86IGNILLifter::opSub32mi8_},
          {"SUB64rm", &X86IGNILLifter::opSub64rm_},
          {"SUB32rm", &X86IGNILLifter::opSub32rm_},
          {"XOR64rr", &X86IGNILLifter::opXor64rr_},
          {"XOR32rr", &X86IGNILLifter::opXor32rr_},
          {"XOR64ri32", &X86IGNILLifter::opXor64ri_},
          {"XOR64ri8", &X86IGNILLifter::opXor64ri8_},
          {"XOR32ri", &X86IGNILLifter::opXor32ri_},
          {"XOR32ri8", &X86IGNILLifter::opXor32ri8_},
          {"XOR32rm", &X86IGNILLifter::opXor32rm_},
          {"XOR32mi", &X86IGNILLifter::opXor32mi_},
          {"XOR32mr", &X86IGNILLifter::opXor32mr_},
          {"AND64rr", &X86IGNILLifter::opAnd64rr_},
          {"AND32rr", &X86IGNILLifter::opAnd32rr_},
          {"AND64ri32", &X86IGNILLifter::opAnd64ri_},
          {"AND64ri8", &X86IGNILLifter::opAnd64ri8_},
          {"AND32ri", &X86IGNILLifter::opAnd32ri_},
          {"AND32ri8", &X86IGNILLifter::opAnd32ri8_},
          {"AND32mr", &X86IGNILLifter::opAnd32mr_},
          {"OR64rr", &X86IGNILLifter::opOr64rr_},
          {"OR32rr", &X86IGNILLifter::opOr32rr_},
          {"OR64ri32", &X86IGNILLifter::opOr64ri_},
          {"OR64ri8", &X86IGNILLifter::opOr64ri8_},
          {"OR32ri", &X86IGNILLifter::opOr32ri_},
          {"OR32ri8", &X86IGNILLifter::opOr32ri8_},
          {"PUSH64r", &X86IGNILLifter::opPush64r_},
          {"PUSH32r", &X86IGNILLifter::opPush32r_},
          {"PUSH32rmm", &X86IGNILLifter::opPush32rmm_},
          {"POP64r", &X86IGNILLifter::opPop64r_},
          {"POP32r", &X86IGNILLifter::opPop32r_},
          {"POP32rmm", &X86IGNILLifter::opPop32rmm_},
          {"POPF64", &X86IGNILLifter::opPopf64_},
          {"POPF32", &X86IGNILLifter::opPopf32_},
          {"PUSHF64", &X86IGNILLifter::opPushf64_},
          {"PUSHF32", &X86IGNILLifter::opPushf32_},
          {"PUSHA32", &X86IGNILLifter::opPusha32_},
          {"POPA32", &X86IGNILLifter::opPopa32_},
          {"PUSHSS32", &X86IGNILLifter::opPushSS32_},
          {"POPSS32", &X86IGNILLifter::opPopSS32_},
          {"CMP64rr", &X86IGNILLifter::opCmp64rr_},
          {"CMP32rr", &X86IGNILLifter::opCmp32rr_},
          {"CMP64ri32", &X86IGNILLifter::opCmp64ri_},
          {"CMP64ri8", &X86IGNILLifter::opCmp64ri8_},
          {"CMP32ri", &X86IGNILLifter::opCmp32ri_},
          {"CMP32ri8", &X86IGNILLifter::opCmp32ri8_},
          {"CMP64rm", &X86IGNILLifter::opCmp64rm_},
          {"CMP32rm", &X86IGNILLifter::opCmp32rm_},
          {"CMP64mr", &X86IGNILLifter::opCmp64mr_},
          {"CMP32mr", &X86IGNILLifter::opCmp32mr_},
          {"CMP64mi32", &X86IGNILLifter::opCmp64mi_},
          {"CMP64mi8", &X86IGNILLifter::opCmp64mi8_},
          {"CMP32mi", &X86IGNILLifter::opCmp32mi_},
          {"CMP32mi8", &X86IGNILLifter::opCmp32mi8_},
          {"CMP8mi", &X86IGNILLifter::opCmp8mi_},
          {"TEST64rr", &X86IGNILLifter::opTest64rr_},
          {"TEST32rr", &X86IGNILLifter::opTest32rr_},
          {"TEST64ri32", &X86IGNILLifter::opTest64ri_},
          {"TEST32ri", &X86IGNILLifter::opTest32ri_},
          {"JMP64r", &X86IGNILLifter::opJmp64r_},
          {"JMP_1", &X86IGNILLifter::opJmp_},
          {"LOOP", &X86IGNILLifter::opLoop_},
          {"LOOPE", &X86IGNILLifter::opLoope_},
          {"LOOPNE", &X86IGNILLifter::opLoopne_},
          {"JCXZ", &X86IGNILLifter::opJcxz_},
          {"JECXZ", &X86IGNILLifter::opJecxz_},
          {"JRCXZ", &X86IGNILLifter::opJrcxz_},
          {"JMP64m", &X86IGNILLifter::opJmp64m_},
          {"JMP32m", &X86IGNILLifter::opJmp32m_},
          {"CALL64r", &X86IGNILLifter::opCall64r_},
          {"CALL32r", &X86IGNILLifter::opCall32r_},
          {"CALL64m", &X86IGNILLifter::opCall64m_},
          {"CALL32m", &X86IGNILLifter::opCall32m_},
          {"CALL64pcrel32", &X86IGNILLifter::opCall_},
          {"CALLpcrel32", &X86IGNILLifter::opCall32_},
          {"RET64", &X86IGNILLifter::opRet64_},
          {"RET32", &X86IGNILLifter::opRet32_},
          {"RETI64", &X86IGNILLifter::opRetI64_},
          {"RETI32", &X86IGNILLifter::opRetI32_},
          {"LEAVE64", &X86IGNILLifter::opLeave64_},
          {"LEAVE", &X86IGNILLifter::opLeave_},
          {"SAR32ri", &X86IGNILLifter::opSar32ri_},
          {"SAR64ri", &X86IGNILLifter::opSar64ri_},
          {"IMUL64rri32", &X86IGNILLifter::opImul64rri32_},
          {"IMUL64rri8", &X86IGNILLifter::opImul64rri8_},
          {"IMUL32rri", &X86IGNILLifter::opImul32rri_},
          {"IMUL32rri8", &X86IGNILLifter::opImul32rri8_},
          {"IMUL32rr", &X86IGNILLifter::opImul32rr_},
          {"IMUL16rr", &X86IGNILLifter::opImul16rr_},
          {"IMUL16rri", &X86IGNILLifter::opImul16rri_},
          {"IMUL16rri8", &X86IGNILLifter::opImul16rri8_},
          {"IMUL64rr", &X86IGNILLifter::opImul64rr_},
          {"LEA64_32r", &X86IGNILLifter::opLea64_32r_},
          {"INC32r", &X86IGNILLifter::opInc32r_},
          {"INC64r", &X86IGNILLifter::opInc64r_},
          {"DEC32r", &X86IGNILLifter::opDec32r_},
          {"INC32r_alt", &X86IGNILLifter::opInc32r_alt_},
          {"DEC32r_alt", &X86IGNILLifter::opDec32r_alt_},
          {"DEC64r", &X86IGNILLifter::opDec64r_},
          {"NEG32r", &X86IGNILLifter::opNeg32r_},
          {"NEG64r", &X86IGNILLifter::opNeg64r_},
          {"SHL32ri", &X86IGNILLifter::opShl32ri_},
          {"SHL64ri", &X86IGNILLifter::opShl64ri_},
          {"SHR32ri", &X86IGNILLifter::opShr32ri_},
          {"SHR64ri", &X86IGNILLifter::opShr64ri_},
          {"SAR32r1", &X86IGNILLifter::opSar32r1_},
          {"SAR64r1", &X86IGNILLifter::opSar64r1_},
          {"SHL32r1", &X86IGNILLifter::opShl32r1_},
          {"SHL64r1", &X86IGNILLifter::opShl64r1_},
          {"SHR32r1", &X86IGNILLifter::opShr32r1_},
          {"SHR64r1", &X86IGNILLifter::opShr64r1_},
          {"NOT8r", &X86IGNILLifter::opNot8r_},
          {"NOT16r", &X86IGNILLifter::opNot16r_},
          {"NOT32r", &X86IGNILLifter::opNot32r_},
          {"NOT64r", &X86IGNILLifter::opNot64r_},
          {"NEG8r", &X86IGNILLifter::opNeg8r_},
          {"NEG16r", &X86IGNILLifter::opNeg16r_},
          {"SHL8ri", &X86IGNILLifter::opShl8ri_},
          {"SHL8r1", &X86IGNILLifter::opShl8r1_},
          {"SHL16ri", &X86IGNILLifter::opShl16ri_},
          {"SHL16r1", &X86IGNILLifter::opShl16r1_},
          {"SHL8rCL", &X86IGNILLifter::opShl8rCL_},
          {"SHL16rCL", &X86IGNILLifter::opShl16rCL_},
          {"SHL32rCL", &X86IGNILLifter::opShl32rCL_},
          {"SHL64rCL", &X86IGNILLifter::opShl64rCL_},
          {"SHR8ri", &X86IGNILLifter::opShr8ri_},
          {"SHR8r1", &X86IGNILLifter::opShr8r1_},
          {"SHR16ri", &X86IGNILLifter::opShr16ri_},
          {"SHR16r1", &X86IGNILLifter::opShr16r1_},
          {"SHR8rCL", &X86IGNILLifter::opShr8rCL_},
          {"SHR16rCL", &X86IGNILLifter::opShr16rCL_},
          {"SHR32rCL", &X86IGNILLifter::opShr32rCL_},
          {"SHR32mCL", &X86IGNILLifter::opShr32mCL_},
          {"SHR64rCL", &X86IGNILLifter::opShr64rCL_},
          {"SAR8ri", &X86IGNILLifter::opSar8ri_},
          {"SAR8r1", &X86IGNILLifter::opSar8r1_},
          {"SAR16ri", &X86IGNILLifter::opSar16ri_},
          {"SAR16r1", &X86IGNILLifter::opSar16r1_},
          {"SAR8rCL", &X86IGNILLifter::opSar8rCL_},
          {"SAR16rCL", &X86IGNILLifter::opSar16rCL_},
          {"SAR32rCL", &X86IGNILLifter::opSar32rCL_},
          {"SAR64rCL", &X86IGNILLifter::opSar64rCL_},
          {"CBW", &X86IGNILLifter::opCbw_},
          {"CWDE", &X86IGNILLifter::opCwde_},
          {"CDQE", &X86IGNILLifter::opCdqe_},
          {"CWD", &X86IGNILLifter::opCwd_},
          {"CDQ", &X86IGNILLifter::opCdq_},
          {"CQO", &X86IGNILLifter::opCqo_},
          {"STC", &X86IGNILLifter::opStc_},
          {"SETCCr", &X86IGNILLifter::opSetccr_},
          {"BT16ri8", &X86IGNILLifter::opBt16ri8_},
          {"BT32ri8", &X86IGNILLifter::opBt32ri8_},
          {"BT64ri8", &X86IGNILLifter::opBt64ri8_},
          {"BT16rr", &X86IGNILLifter::opBt16rr_},
          {"BT32rr", &X86IGNILLifter::opBt32rr_},
          {"BT64rr", &X86IGNILLifter::opBt64rr_},
          {"ROL8ri", &X86IGNILLifter::opRol8ri_},
          {"ROL16ri", &X86IGNILLifter::opRol16ri_},
          {"ROL32ri", &X86IGNILLifter::opRol32ri_},
          {"ROL64ri", &X86IGNILLifter::opRol64ri_},
          {"ROL8r1", &X86IGNILLifter::opRol8r1_},
          {"ROL16r1", &X86IGNILLifter::opRol16r1_},
          {"ROL32r1", &X86IGNILLifter::opRol32r1_},
          {"ROL64r1", &X86IGNILLifter::opRol64r1_},
          {"ROL8rCL", &X86IGNILLifter::opRol8rCL_},
          {"ROL16rCL", &X86IGNILLifter::opRol16rCL_},
          {"ROL32rCL", &X86IGNILLifter::opRol32rCL_},
          {"ROL64rCL", &X86IGNILLifter::opRol64rCL_},
          {"ROR8ri", &X86IGNILLifter::opRor8ri_},
          {"ROR16ri", &X86IGNILLifter::opRor16ri_},
          {"ROR32ri", &X86IGNILLifter::opRor32ri_},
          {"ROR64ri", &X86IGNILLifter::opRor64ri_},
          {"ROR8r1", &X86IGNILLifter::opRor8r1_},
          {"ROR16r1", &X86IGNILLifter::opRor16r1_},
          {"ROR32r1", &X86IGNILLifter::opRor32r1_},
          {"ROR64r1", &X86IGNILLifter::opRor64r1_},
          {"ROR8rCL", &X86IGNILLifter::opRor8rCL_},
          {"ROR16rCL", &X86IGNILLifter::opRor16rCL_},
          {"ROR32rCL", &X86IGNILLifter::opRor32rCL_},
          {"ROR32mCL", &X86IGNILLifter::opRor32mCL_},
          {"ROR64rCL", &X86IGNILLifter::opRor64rCL_},
          {"BTS16ri8", &X86IGNILLifter::opBts16ri8_},
          {"BTS32ri8", &X86IGNILLifter::opBts32ri8_},
          {"BTS64ri8", &X86IGNILLifter::opBts64ri8_},
          {"BTS16rr", &X86IGNILLifter::opBts16rr_},
          {"BTS32rr", &X86IGNILLifter::opBts32rr_},
          {"BTS64rr", &X86IGNILLifter::opBts64rr_},
          {"BTR16ri8", &X86IGNILLifter::opBtr16ri8_},
          {"BTR32ri8", &X86IGNILLifter::opBtr32ri8_},
          {"BTR32mi8", &X86IGNILLifter::opBtr32mi8_},
          {"BTR64ri8", &X86IGNILLifter::opBtr64ri8_},
          {"BTR16rr", &X86IGNILLifter::opBtr16rr_},
          {"BTR32rr", &X86IGNILLifter::opBtr32rr_},
          {"BTR64rr", &X86IGNILLifter::opBtr64rr_},
          {"BTC16ri8", &X86IGNILLifter::opBtc16ri8_},
          {"BTC32ri8", &X86IGNILLifter::opBtc32ri8_},
          {"BTC64ri8", &X86IGNILLifter::opBtc64ri8_},
          {"BTC16rr", &X86IGNILLifter::opBtc16rr_},
          {"BTC32rr", &X86IGNILLifter::opBtc32rr_},
          {"BTC64rr", &X86IGNILLifter::opBtc64rr_},
          {"RCL8ri", &X86IGNILLifter::opRcl8ri_},
          {"RCL16ri", &X86IGNILLifter::opRcl16ri_},
          {"RCL32ri", &X86IGNILLifter::opRcl32ri_},
          {"RCL64ri", &X86IGNILLifter::opRcl64ri_},
          {"RCL8r1", &X86IGNILLifter::opRcl8r1_},
          {"RCL16r1", &X86IGNILLifter::opRcl16r1_},
          {"RCL32r1", &X86IGNILLifter::opRcl32r1_},
          {"RCL64r1", &X86IGNILLifter::opRcl64r1_},
          {"RCR8ri", &X86IGNILLifter::opRcr8ri_},
          {"RCR16ri", &X86IGNILLifter::opRcr16ri_},
          {"RCR32ri", &X86IGNILLifter::opRcr32ri_},
          {"RCR64ri", &X86IGNILLifter::opRcr64ri_},
          {"RCR8r1", &X86IGNILLifter::opRcr8r1_},
          {"RCR16r1", &X86IGNILLifter::opRcr16r1_},
          {"RCR32r1", &X86IGNILLifter::opRcr32r1_},
          {"RCR64r1", &X86IGNILLifter::opRcr64r1_},
          {"RCL8rCL", &X86IGNILLifter::opRcl8rCL_},
          {"RCL16rCL", &X86IGNILLifter::opRcl16rCL_},
          {"RCL32rCL", &X86IGNILLifter::opRcl32rCL_},
          {"RCL64rCL", &X86IGNILLifter::opRcl64rCL_},
          {"RCR8rCL", &X86IGNILLifter::opRcr8rCL_},
          {"RCR16rCL", &X86IGNILLifter::opRcr16rCL_},
          {"RCR32rCL", &X86IGNILLifter::opRcr32rCL_},
          {"RCR64rCL", &X86IGNILLifter::opRcr64rCL_},
          {"XCHG16ar", &X86IGNILLifter::opXchg16ar_},
          {"XCHG32ar", &X86IGNILLifter::opXchg32ar_},
          {"XCHG64ar", &X86IGNILLifter::opXchg64ar_},
          {"BSF16rr", &X86IGNILLifter::opBsf16rr_},
          {"BSF32rr", &X86IGNILLifter::opBsf32rr_},
          {"BSF64rr", &X86IGNILLifter::opBsf64rr_},
          {"BSR16rr", &X86IGNILLifter::opBsr16rr_},
          {"BSR32rr", &X86IGNILLifter::opBsr32rr_},
          {"BSR64rr", &X86IGNILLifter::opBsr64rr_},
          {"CMOV16rr", &X86IGNILLifter::opCmov16rr_},
          {"CMOV32rr", &X86IGNILLifter::opCmov32rr_},
          {"CMOV64rr", &X86IGNILLifter::opCmov64rr_},
          {"XADD8rr", &X86IGNILLifter::opXadd8rr_},
          {"XADD16rr", &X86IGNILLifter::opXadd16rr_},
          {"XADD32rr", &X86IGNILLifter::opXadd32rr_},
          {"XADD64rr", &X86IGNILLifter::opXadd64rr_},
          {"INC8m", &X86IGNILLifter::opInc8m_},
          {"INC16m", &X86IGNILLifter::opInc16m_},
          {"INC32m", &X86IGNILLifter::opInc32m_},
          {"INC64m", &X86IGNILLifter::opInc64m_},
          {"DEC8m", &X86IGNILLifter::opDec8m_},
          {"DEC16m", &X86IGNILLifter::opDec16m_},
          {"DEC32m", &X86IGNILLifter::opDec32m_},
          {"DEC64m", &X86IGNILLifter::opDec64m_},
          {"MOV8ri_alt", &X86IGNILLifter::opMov8riAlt_},
          {"LAHF", &X86IGNILLifter::opLahf_},
          {"SAHF", &X86IGNILLifter::opSahf_},
          {"CLC", &X86IGNILLifter::opClc_},
          {"CMC", &X86IGNILLifter::opCmc_},
          {"CLD", &X86IGNILLifter::opCld_},
          {"CPUID", &X86IGNILLifter::opCpuid_},
          {"RDTSC", &X86IGNILLifter::opRdtsc_},
          {"RDTSCP", &X86IGNILLifter::opRdtscp_},
          {"PUSH32i", &X86IGNILLifter::opPush32I_},
          {"PUSH32i8", &X86IGNILLifter::opPush32I8_},
          {"PUSH64i32", &X86IGNILLifter::opPush64I32_},
          {"PUSH64i8", &X86IGNILLifter::opPush64I8_},
          {"MOV8rr", &X86IGNILLifter::opMov8rr_},
          {"MOV8ri", &X86IGNILLifter::opMov8ri_},
          {"ADD8rr", &X86IGNILLifter::opAdd8rr_},
          {"ADD8ri", &X86IGNILLifter::opAdd8ri_},
          {"ADD8ri8", &X86IGNILLifter::opAdd8ri8_},
          {"SUB8rr", &X86IGNILLifter::opSub8rr_},
          {"SUB8ri", &X86IGNILLifter::opSub8ri_},
          {"SUB8ri8", &X86IGNILLifter::opSub8ri8_},
          {"AND8rr", &X86IGNILLifter::opAnd8rr_},
          {"AND8ri", &X86IGNILLifter::opAnd8ri_},
          {"AND8ri8", &X86IGNILLifter::opAnd8ri8_},
          {"OR8rr", &X86IGNILLifter::opOr8rr_},
          {"OR8ri", &X86IGNILLifter::opOr8ri_},
          {"OR8ri8", &X86IGNILLifter::opOr8ri8_},
          {"XOR8rr", &X86IGNILLifter::opXor8rr_},
          {"XOR8ri", &X86IGNILLifter::opXor8ri_},
          {"XOR8ri8", &X86IGNILLifter::opXor8ri8_},
          {"CMP8rr", &X86IGNILLifter::opCmp8rr_},
          {"CMP8ri", &X86IGNILLifter::opCmp8ri_},
          {"CMP8ri8", &X86IGNILLifter::opCmp8ri8_},
          {"TEST8rr", &X86IGNILLifter::opTest8rr_},
          {"TEST8ri", &X86IGNILLifter::opTest8ri_},
          {"INC8r", &X86IGNILLifter::opInc8r_},
          {"DEC8r", &X86IGNILLifter::opDec8r_},
          {"MOV16rr", &X86IGNILLifter::opMov16rr_},
          {"MOV16ri", &X86IGNILLifter::opMov16ri_},
          {"ADD16rr", &X86IGNILLifter::opAdd16rr_},
          {"ADD16ri", &X86IGNILLifter::opAdd16ri_},
          {"ADD16ri8", &X86IGNILLifter::opAdd16ri8_},
          {"SUB16rr", &X86IGNILLifter::opSub16rr_},
          {"SUB16ri", &X86IGNILLifter::opSub16ri_},
          {"SUB16ri8", &X86IGNILLifter::opSub16ri8_},
          {"AND16rr", &X86IGNILLifter::opAnd16rr_},
          {"AND16ri", &X86IGNILLifter::opAnd16ri_},
          {"AND16ri8", &X86IGNILLifter::opAnd16ri8_},
          {"OR16rr", &X86IGNILLifter::opOr16rr_},
          {"OR16ri", &X86IGNILLifter::opOr16ri_},
          {"OR16ri8", &X86IGNILLifter::opOr16ri8_},
          {"XOR16rr", &X86IGNILLifter::opXor16rr_},
          {"XOR16ri", &X86IGNILLifter::opXor16ri_},
          {"XOR16ri8", &X86IGNILLifter::opXor16ri8_},
          {"CMP16rr", &X86IGNILLifter::opCmp16rr_},
          {"CMP16ri", &X86IGNILLifter::opCmp16ri_},
          {"CMP16ri8", &X86IGNILLifter::opCmp16ri8_},
          {"TEST16rr", &X86IGNILLifter::opTest16rr_},
          {"TEST16ri", &X86IGNILLifter::opTest16ri_},
          {"INC16r", &X86IGNILLifter::opInc16r_},
          {"DEC16r", &X86IGNILLifter::opDec16r_},
          {"INC16r_alt", &X86IGNILLifter::opInc16r_alt_},
          {"DEC16r_alt", &X86IGNILLifter::opDec16r_alt_},
          {"XCHG8rr", &X86IGNILLifter::opXchg8rr_},
          {"XCHG16rr", &X86IGNILLifter::opXchg16rr_},
          {"XCHG32rr", &X86IGNILLifter::opXchg32rr_},
          {"XCHG32rm", &X86IGNILLifter::opXchg32rm_},
          {"XCHG8rm", &X86IGNILLifter::opXchg8rm_},
          {"XCHG16rm", &X86IGNILLifter::opXchg16rm_},
          {"XCHG64rm", &X86IGNILLifter::opXchg64rm_},
          {"XCHG64rr", &X86IGNILLifter::opXchg64rr_},
          {"BSWAP32r", &X86IGNILLifter::opBswap32r_},
          {"BSWAP64r", &X86IGNILLifter::opBswap64r_},
          {"SHLD16rrCL", &X86IGNILLifter::opShld16rrCL_},
          {"SHLD16rri8", &X86IGNILLifter::opShld16rri8_},
          {"SHRD16rrCL", &X86IGNILLifter::opShrd16rrCL_},
          {"SHRD16rri8", &X86IGNILLifter::opShrd16rri8_},
          {"SHLD32rrCL", &X86IGNILLifter::opShld32rrCL_},
          {"SHLD32rri8", &X86IGNILLifter::opShld32rri8_},
          {"SHRD32rrCL", &X86IGNILLifter::opShrd32rrCL_},
          {"SHRD32rri8", &X86IGNILLifter::opShrd32rri8_},
          {"SHLD64rrCL", &X86IGNILLifter::opShld64rrCL_},
          {"SHLD64rri8", &X86IGNILLifter::opShld64rri8_},
          {"SHRD64rrCL", &X86IGNILLifter::opShrd64rrCL_},
          {"SHRD64rri8", &X86IGNILLifter::opShrd64rri8_},
          {"CVTSI2SDrr_Int", &X86IGNILLifter::opCvtsi2sd32_},
          {"CVTSI642SDrr_Int", &X86IGNILLifter::opCvtsi2sd64_},
          {"CVTTSD2SIrr_Int", &X86IGNILLifter::opCvttsd2si32_},
          {"CVTTSD2SI64rr_Int", &X86IGNILLifter::opCvttsd2si64_},
          {"ADDSDrr_Int", &X86IGNILLifter::opAddsd_},
          {"SUBSDrr_Int", &X86IGNILLifter::opSubsd_},
          {"MULSDrr_Int", &X86IGNILLifter::opMulsd_},
          {"MULSDrm_Int", &X86IGNILLifter::opMulsdMem_},
          {"MOVSDrr", &X86IGNILLifter::opMovsdRR_},
          {"MOVSDrm", &X86IGNILLifter::opMovsdLoad_},
          {"MOVSDmr", &X86IGNILLifter::opMovsdStore_},
  };

  // MOVZX/MOVSX are kept out of kOpcodeFields: there are twenty-odd spellings
  // between them, all dispatching to one handler, and each needs the width of
  // the value it extends. A name -> width table keyed by opcode says both
  // things at once, where twenty named members would say neither.
  //
  // The width is the *source* width from the mnemonic (MOVZX32rm8 loads one
  // byte into a 32-bit register). For the memory-source forms it is the only
  // place that width exists: an x86 memory reference carries no size. The
  // rr/rm spelling is equally load-bearing - op[1] is a register in both
  // forms (the memory operand's base), so the name is the only thing that
  // distinguishes them.
  static const std::vector<std::pair<std::string_view, ignil::Width>>
          kMovzxSpellings = {
                  {"MOVZX16rr8", 8},        {"MOVZX16rm8", 8},
                  {"MOVZX16rr16", 16},      {"MOVZX16rm16", 16},
                  {"MOVZX32rr8", 8},        {"MOVZX32rm8", 8},
                  {"MOVZX32rr8_NOREX", 8},  {"MOVZX32rm8_NOREX", 8},
                  {"MOVZX32rr16", 16},      {"MOVZX32rm16", 16},
                  {"MOVZX64rr8", 8},        {"MOVZX64rm8", 8},
                  {"MOVZX64rr16", 16},      {"MOVZX64rm16", 16},
          };
  static const std::vector<std::pair<std::string_view, ignil::Width>>
          kMovsxSpellings = {
                  {"MOVSX16rr8", 8},        {"MOVSX16rm8", 8},
                  {"MOVSX16rr16", 16},      {"MOVSX16rm16", 16},
                  {"MOVSX16rr32", 32},      {"MOVSX16rm32", 32},
                  {"MOVSX32rr8", 8},        {"MOVSX32rm8", 8},
                  {"MOVSX32rr8_NOREX", 8},  {"MOVSX32rm8_NOREX", 8},
                  {"MOVSX32rr16", 16},      {"MOVSX32rm16", 16},
                  {"MOVSX32rr32", 32},      {"MOVSX32rm32", 32},
                  {"MOVSX64rr8", 8},        {"MOVSX64rm8", 8},
                  {"MOVSX64rr16", 16},      {"MOVSX64rm16", 16},
                  {"MOVSX64rr32", 32},      {"MOVSX64rm32", 32},
          };

  // Index them by name, tagging each with the operand form its spelling
  // implies. Every entry above is either ...rr<N> or ...rm<N>.
  const auto indexSpellings =
          [](const std::vector<std::pair<std::string_view, ignil::Width>> &in) {
            std::unordered_map<std::string_view, MovExtendForm> out;
            for (const auto &[name, width]: in)
              out.emplace(name, MovExtendForm{
                                        width,
                                        name.find("rm") != std::string_view::npos});
            return out;
          };
  const auto movzxByName = indexSpellings(kMovzxSpellings);
  const auto movsxByName = indexSpellings(kMovsxSpellings);

  // *_REV load-direction encodings mapped to their base form (see header).
  // Only the widths whose base rr handler exists (32/64) are listed; an
  // unmapped base would just leave the *_REV as-is (still UNDEF, no harm).
  static const std::pair<std::string_view, std::string_view> kRevAliases[] = {
          {"MOV32rr_REV", "MOV32rr"}, {"MOV64rr_REV", "MOV64rr"},
          {"ADD32rr_REV", "ADD32rr"}, {"ADD64rr_REV", "ADD64rr"},
          {"SUB32rr_REV", "SUB32rr"}, {"SUB64rr_REV", "SUB64rr"},
          {"XOR32rr_REV", "XOR32rr"}, {"XOR64rr_REV", "XOR64rr"},
          {"AND32rr_REV", "AND32rr"}, {"AND64rr_REV", "AND64rr"},
          {"OR32rr_REV", "OR32rr"},   {"OR64rr_REV", "OR64rr"},
          {"CMP32rr_REV", "CMP32rr"}, {"CMP64rr_REV", "CMP64rr"},
          {"MOV8rr_REV", "MOV8rr"},   {"MOV16rr_REV", "MOV16rr"},
          {"ADD8rr_REV", "ADD8rr"},   {"ADD16rr_REV", "ADD16rr"},
          {"SUB8rr_REV", "SUB8rr"},   {"SUB16rr_REV", "SUB16rr"},
          {"XOR8rr_REV", "XOR8rr"},   {"XOR16rr_REV", "XOR16rr"},
          {"AND8rr_REV", "AND8rr"},   {"AND16rr_REV", "AND16rr"},
          {"OR8rr_REV", "OR8rr"},     {"OR16rr_REV", "OR16rr"},
          {"CMP8rr_REV", "CMP8rr"},   {"CMP16rr_REV", "CMP16rr"},
  };

  using S = SseKind;
  static const std::unordered_map<std::string_view, SseOp> kSseOps = {
          {"COMISDrr", {S::Comisd}},           {"COMISDrr_Int", {S::Comisd}},
          {"UCOMISDrr", {S::Comisd}},          {"UCOMISDrr_Int", {S::Comisd}},
          {"COMISDrm", {S::Comisd, true}},     {"COMISDrm_Int", {S::Comisd, true}},
          {"UCOMISDrm", {S::Comisd, true}},    {"UCOMISDrm_Int", {S::Comisd, true}},
          {"ANDPDrr", {S::And}},   {"ANDPSrr", {S::And}},   {"PANDrr", {S::And}},
          {"ANDNPDrr", {S::Andn}}, {"ANDNPSrr", {S::Andn}}, {"PANDNrr", {S::Andn}},
          {"ORPDrr", {S::Or}},     {"ORPSrr", {S::Or}},     {"PORrr", {S::Or}},
          {"XORPDrr", {S::Xor}},   {"XORPSrr", {S::Xor}},   {"PXORrr", {S::Xor}},
          {"ANDPDrm", {S::And, true}},   {"ANDPSrm", {S::And, true}},
          {"ANDNPDrm", {S::Andn, true}}, {"ANDNPSrm", {S::Andn, true}},
          {"ORPDrm", {S::Or, true}},     {"ORPSrm", {S::Or, true}},
          {"XORPDrm", {S::Xor, true}},   {"XORPSrm", {S::Xor, true}},
          {"PANDrm", {S::And, true}},    {"PANDNrm", {S::Andn, true}},
          {"PORrm", {S::Or, true}},      {"PXORrm", {S::Xor, true}},
          {"MOVAPDrr", {S::Move}},  {"MOVUPDrr", {S::Move}},  {"MOVAPSrr", {S::Move}},
          {"MOVUPSrr", {S::Move}},  {"MOVDQArr", {S::Move}},  {"MOVDQUrr", {S::Move}},
          {"MOVAPDrr_REV", {S::Move}}, {"MOVUPDrr_REV", {S::Move}},
          {"MOVAPSrr_REV", {S::Move}}, {"MOVUPSrr_REV", {S::Move}},
          {"MOVDQArr_REV", {S::Move}}, {"MOVDQUrr_REV", {S::Move}},
          {"MOVAPDrm", {S::Move, true}}, {"MOVUPDrm", {S::Move, true}},
          {"MOVAPSrm", {S::Move, true}}, {"MOVUPSrm", {S::Move, true}},
          {"MOVDQArm", {S::Move, true}}, {"MOVDQUrm", {S::Move, true}},
          {"MOV64toPQIrr", {S::Movq}}, {"MOVPQIto64rr", {S::Movq}},
          {"MOVZPQILo2PQIrr", {S::Movq}}, {"MOVQI2PQIrm", {S::Movq, true}},
          {"MOVPQI2QImr", {S::Movq, false, true}},
          {"MOVDI2PDIrr", {S::MovdTo}}, {"MOVDI2PDIrm", {S::MovdTo, true, false, 32}},
          {"MOVPDI2DIrr", {S::MovdFrom}},
          {"MOVPDI2DImr", {S::MovdFrom, false, true, 32}},
          {"ADDSDrm_Int", {S::Addsd, true}}, {"ADDSDrm", {S::Addsd, true}},
          {"SUBSDrm_Int", {S::Subsd, true}}, {"SUBSDrm", {S::Subsd, true}},
          {"DIVSDrr_Int", {S::Divsd}},       {"DIVSDrr", {S::Divsd}},
          {"DIVSDrm_Int", {S::Divsd, true}}, {"DIVSDrm", {S::Divsd, true}},
          {"CVTSI2SDrm_Int", {S::Cvtsi2sd, true, false, 32}},
          {"CVTSI2SDrm", {S::Cvtsi2sd, true, false, 32}},
          {"CVTSI642SDrm_Int", {S::Cvtsi2sd, true, false, 64}},
          {"CVTSI642SDrm", {S::Cvtsi2sd, true, false, 64}},
          {"CVTSS2SDrr_Int", {S::Cvtss2sd}}, {"CVTSS2SDrr", {S::Cvtss2sd}},
          {"CVTSS2SDrm_Int", {S::Cvtss2sd,true,false,32}}, {"CVTSS2SDrm", {S::Cvtss2sd,true,false,32}},
          {"CVTSD2SSrr_Int", {S::Cvtsd2ss}}, {"CVTSD2SSrr", {S::Cvtsd2ss}},
          {"CVTSD2SSrm_Int", {S::Cvtsd2ss,true}}, {"CVTSD2SSrm", {S::Cvtsd2ss,true}},
  };

  std::unordered_map<std::string_view, unsigned> nameToOpc;
  for (unsigned i = 0; i < instrInfo.getNumOpcodes(); ++i) {
    const std::string_view name = instrInfo.getName(i);
    nameToOpc.emplace(name, i);

    // nop in all its encodings: NOOP (90), NOOPW/NOOPL/NOOPQ - the multi-byte
    // `nop word ptr cs:[rax + rax]` alignment padding - and their register
    // forms. FNOP is the x87 one.
    if (name.starts_with("NOOP") || name == "FNOP" || name.starts_with("PREFETCH"))
      nopOpcodes_.insert(i);
    if (auto op = classifyMemOperandOp(name))
      memOperandOps_.emplace(i, *op);
    if (auto it = kSseOps.find(name); it != kSseOps.end())
      sseOps_.emplace(i, it->second);

    if (auto it = kOpcodeFields.find(name); it != kOpcodeFields.end())
      this->*(it->second) = i;
    else if (auto zx = movzxByName.find(name); zx != movzxByName.end())
      movzxForms_.emplace(i, zx->second);
    else if (auto sx = movsxByName.find(name); sx != movsxByName.end())
      movsxForms_.emplace(i, sx->second);
  }

#ifndef NDEBUG
  // A name this LLVM does not have leaves its field 0 and the instruction
  // silently unlifted (RETL/RETIL were renamed RET32/RETI32). Say so.
  for (const auto &entry: kOpcodeFields)
    if (!nameToOpc.contains(entry.first))
      llvm::errs() << "X86IGNILLifter: no LLVM opcode named " << entry.first
                   << "\n";
#endif

  for (const auto &[rev, base]: kRevAliases) {
    auto r = nameToOpc.find(rev), b = nameToOpc.find(base);
    if (r != nameToOpc.end() && b != nameToOpc.end())
      revToBase_.emplace(r->second, b->second);
  }

  // ADC / SBB, and the one-operand multiply / divide family. Looked up by
  // name, so a spelling this LLVM does not have is simply skipped.
  {
    auto opcodeNamed = [&](const std::string &name) -> std::optional<unsigned> {
      auto it = nameToOpc.find(name);
      if (it == nameToOpc.end())
        return std::nullopt;
      return it->second;
    };
    static constexpr std::string_view kAdcSbbSuffixes[] = {
            "8rr",  "8ri",  "16rr", "16ri",   "16ri8", "32rr",
            "32ri", "32ri8", "64rr", "64ri32", "64ri8"};
    for (const bool isSbb: {false, true})
      for (std::string_view suffix: kAdcSbbSuffixes) {
        const std::string name = std::string(isSbb ? "SBB" : "ADC") + std::string(suffix);
        if (auto opc = opcodeNamed(name))
          adcSbbForms_.emplace(*opc, isSbb);
        // The load-direction reg-reg spelling shares the operand order.
        if (suffix.ends_with("rr"))
          if (auto rev = opcodeNamed(name + "_REV"))
            adcSbbForms_.emplace(*rev, isSbb);
      }

    using Kind = MulDivForm::Kind;
    static constexpr std::pair<std::string_view, Kind> kMulDiv[] = {
            {"MUL", Kind::Mul}, {"IMUL", Kind::Imul},
            {"DIV", Kind::Div}, {"IDIV", Kind::Idiv}};
    for (const auto &[mnemonic, kind]: kMulDiv)
      for (const unsigned width: {8u, 16u, 32u, 64u})
        if (auto opc = opcodeNamed(std::string(mnemonic) + std::to_string(width) + "r"))
          mulDivForms_.emplace(*opc, MulDivForm{kind, static_cast<ignil::Width>(width)});
  }

  // Accumulator short forms -> generic reg-imm forms. LLVM names them after
  // the immediate alone (SUB8i8, CMP32i32, TEST64i32); the generic spelling of
  // the same width is <OP>8ri / 16ri / 32ri / 64ri32.
  {
    auto llvmRegNamed = [&](std::string_view name) -> unsigned {
      for (unsigned i = 0; i < regInfo.getNumRegs(); ++i)
        if (regInfo.getName(i) == name)
          return i;
      return 0;
    };
    struct Width {
      std::string_view shortSuffix, genericSuffix, reg;
    };
    static constexpr Width kWidths[] = {{"8i8", "8ri", "AL"},
                                        {"16i16", "16ri", "AX"},
                                        {"32i32", "32ri", "EAX"},
                                        {"64i32", "64ri32", "RAX"}};
    struct Alu {
      std::string_view name;
      bool tiedDst;
    };
    static constexpr Alu kAlus[] = {{"ADD", true}, {"SUB", true},  {"AND", true},
                                    {"OR", true},  {"XOR", true},  {"CMP", false},
                                    {"TEST", false}, {"ADC", true}, {"SBB", true}};
    for (const Alu &alu: kAlus)
      for (const Width &w: kWidths) {
        const auto shortForm =
                nameToOpc.find(std::string(alu.name) + std::string(w.shortSuffix));
        const auto generic =
                nameToOpc.find(std::string(alu.name) + std::string(w.genericSuffix));
        const unsigned reg = llvmRegNamed(w.reg);
        if (shortForm != nameToOpc.end() && generic != nameToOpc.end() && reg != 0)
          accForms_.emplace(shortForm->second,
                            AccForm{generic->second, reg, alu.tiedDst});
      }
  }

  // Memory-operand forms lifted through their register form (MemViaReg).
  {
    auto llvmRegNamed = [&](std::string_view name) -> unsigned {
      if (name.empty())
        return 0;
      for (unsigned i = 0; i < regInfo.getNumRegs(); ++i)
        if (regInfo.getName(i) == name)
          return i;
      return 0;
    };
    auto add = [&](const std::string &memName, const std::string &regName,
                   unsigned width, bool bitString, bool xadd) {
      auto m = nameToOpc.find(memName);
      auto r = nameToOpc.find(regName);
      if (m == nameToOpc.end() || r == nameToOpc.end() ||
          memOperandOps_.contains(m->second))
        return;
      memViaReg_.emplace(m->second,
                         MemViaReg{r->second, static_cast<ignil::Width>(width),
                                   bitString, xadd});
    };
    static constexpr std::pair<std::string_view, std::string_view> kShiftForms[] = {
            {"m1", "r1"}, {"mi", "ri"}, {"mCL", "rCL"}};
    static constexpr std::pair<std::string_view, std::string_view> kImulForms[] = {
            {"rm", "rr"}, {"rmi", "rri"}, {"rmi8", "rri8"}};
    for (const unsigned width: {8u, 16u, 32u, 64u}) {
      const std::string w = std::to_string(width);
      for (const std::string op: {"SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR"})
        for (const auto &[m, r]: kShiftForms)
          add(op + w + std::string(m), op + w + std::string(r), width, false, false);
      for (const std::string op: {"MUL", "IMUL", "DIV", "IDIV"})
        add(op + w + "m", op + w + "r", width, false, false);
      for (const auto &[m, r]: kImulForms)
        add("IMUL" + w + std::string(m), "IMUL" + w + std::string(r), width, false, false);
      for (const std::string op: {"BT", "BTS", "BTR", "BTC"}) {
        add(op + w + "mr", op + w + "rr", width, true, false);
        add(op + w + "mi8", op + w + "ri8", width, false, false);
      }
      for (const std::string op: {"SHLD", "SHRD"}) {
        add(op + w + "mri8", op + w + "rri8", width, false, false);
        add(op + w + "mrCL", op + w + "rrCL", width, false, false);
      }
      for (const std::string op: {"BSF", "BSR", "TZCNT", "LZCNT", "POPCNT"})
        add(op + w + "rm", op + w + "rr", width, false, false);
      add("XADD" + w + "rm", "XADD" + w + "rr", width, false, true);
      for (const std::string evex: {"", "_EVEX"}) {
        for (const std::string op: {"POPCNT", "TZCNT", "LZCNT"})
          add(op + w + "rm" + evex, op + w + "rr" + evex, width, false, false);
        for (const std::string op:
             {"SHLX", "SHRX", "SARX", "ANDN", "BLSR", "BLSI", "BLSMSK", "BZHI", "MULX"})
          add(op + w + "rm" + evex, op + w + "rr" + evex, width, false, false);
        add("RORX" + w + "mi" + evex, "RORX" + w + "ri" + evex, width, false, false);
      }
    }

    using B = BmiKind;
    static constexpr std::pair<std::string_view, BmiKind> kBmi[] = {
            {"POPCNT", B::Popcnt}, {"TZCNT", B::Tzcnt}, {"LZCNT", B::Lzcnt},
            {"SHLX", B::Shlx},     {"SHRX", B::Shrx},   {"SARX", B::Sarx},
            {"ANDN", B::Andn},     {"BLSR", B::Blsr},   {"BLSI", B::Blsi},
            {"BLSMSK", B::Blsmsk}, {"BZHI", B::Bzhi},   {"MULX", B::Mulx},
            {"RORX", B::Rorx}};
    for (const auto &[mnemonic, kind]: kBmi)
      for (const std::string w: {"16", "32", "64"})
        for (const std::string evex: {"", "_EVEX"}) {
          const std::string name = std::string(mnemonic) + w +
                                   (kind == B::Rorx ? "ri" : "rr") + evex;
          if (auto it = nameToOpc.find(name); it != nameToOpc.end())
            bmiOps_.emplace(it->second, kind);
        }

    // XMM halves and the LLVM vector registers that alias them.
    for (unsigned n = 0; n < 16; ++n) {
      const std::string idx = std::to_string(n);
      if (auto lo = kRegisterTable.find("XMM" + idx); lo != kRegisterTable.end())
        regXmmLo_[n] = lo->second;
      if (auto hi = kRegisterTable.find("XMM" + idx + "H"); hi != kRegisterTable.end())
        regXmmHi_[n] = hi->second;
      for (const char *prefix: {"XMM", "YMM", "ZMM"})
        if (const unsigned r = llvmRegNamed(std::string(prefix) + idx))
          vecRegIndex_.emplace(r, n);
    }

    using V = VecKind;
    using Sh = VecShift;
    auto vec = [&](const std::string &name, VecOp op) {
      if (auto it = nameToOpc.find(name); it != nameToOpc.end())
        vecOps_.emplace(it->second, op);
    };
    // Both the register and the memory-source spelling.
    auto vecRM = [&](const std::string &base, VecOp op) {
      vec(base + "rr", op);
      op.memSrc = true;
      vec(base + "rm", op);
    };
    for (const char *st: {"MOVUPSmr", "MOVAPSmr", "MOVUPDmr", "MOVAPDmr", "MOVDQUmr",
                          "MOVDQAmr"})
      vec(st, {V::Store});
    static constexpr std::pair<std::string_view, ignil::Width> kLanes[] = {
            {"B", 8}, {"W", 16}, {"D", 32}, {"Q", 64}};
    for (const auto &[suffix, lw]: kLanes) {
      const std::string sfx(suffix);
      vecRM("PADD" + sfx, {V::Add, lw});
      vecRM("PSUB" + sfx, {V::Sub, lw});
      if (lw != 64) {
        vecRM("PCMPEQ" + sfx, {V::CmpEq, lw});
        vecRM("PCMPGT" + sfx, {V::CmpGt, lw});
      }
    }
    static constexpr std::pair<std::string_view, ignil::Width> kUnpack[] = {
            {"BW", 8}, {"WD", 16}, {"DQ", 32}, {"QDQ", 64}};
    for (const auto &[suffix, lw]: kUnpack) {
      vecRM("PUNPCKL" + std::string(suffix), {V::UnpackLo, lw});
      vecRM("PUNPCKH" + std::string(suffix), {V::UnpackHi, lw});
    }
    vecRM("UNPCKLPS", {V::UnpackLo, 32});
    vecRM("UNPCKHPS", {V::UnpackHi, 32});
    vecRM("UNPCKLPD", {V::UnpackLo, 64});
    vecRM("UNPCKHPD", {V::UnpackHi, 64});
    vec("PSHUFDri", {V::Pshufd});
    vec("PSHUFDmi", {V::Pshufd, 64, true});
    vec("PSHUFLWri", {V::Pshuflw});
    vec("PSHUFLWmi", {V::Pshuflw, 64, true});
    vec("PSHUFHWri", {V::Pshufhw});
    vec("PSHUFHWmi", {V::Pshufhw, 64, true});
    vec("SHUFPSrri", {V::Shufps});
    vec("SHUFPSrmi", {V::Shufps, 64, true});
    vec("SHUFPDrri", {V::Shufpd});
    vec("SHUFPDrmi", {V::Shufpd, 64, true});
    static constexpr std::pair<std::string_view, VecShift> kShifts[] = {
            {"PSLL", Sh::Left}, {"PSRL", Sh::Right}, {"PSRA", Sh::Arith}};
    for (const auto &[mnemonic, sh]: kShifts)
      for (const auto &[suffix, lw]: kLanes) {
        if (sh == Sh::Arith && lw == 64)
          continue; // no psraq before AVX-512
        if (lw == 8)
          continue;
        const std::string base = std::string(mnemonic) + std::string(suffix);
        vec(base + "ri", {V::ShiftImm, lw, false, sh});
        vec(base + "rr", {V::ShiftReg, lw, false, sh});
        vec(base + "rm", {V::ShiftReg, lw, true, sh});
      }
    vec("PSLLDQri", {V::ByteShiftLeft});
    vec("PSRLDQri", {V::ByteShiftRight});
    vec("PMOVMSKBrr", {V::Pmovmskb});
    for (const char *n: {"MOVHPSrm", "MOVHPDrm"})
      vec(n, {V::MovHiLoad, 64, true});
    for (const char *n: {"MOVHPSmr", "MOVHPDmr"})
      vec(n, {V::MovHiStore});
    for (const char *n: {"MOVLPSrm", "MOVLPDrm"})
      vec(n, {V::MovLoLoad, 64, true});
    for (const char *n: {"MOVLPSmr", "MOVLPDmr"})
      vec(n, {V::MovLoStore});
    vec("MOVHLPSrr", {V::Movhlps});
    vec("MOVLHPSrr", {V::Movlhps});
    vec("MOVSSrm", {V::MovssLoad, 32, true});
    vec("MOVSSrm_alt", {V::MovssLoad, 32, true});
    vec("MOVSSmr", {V::MovssStore, 32});
    vec("MOVSSrr", {V::MovssReg, 32});
    vec("PEXTRWrri", {V::Pextrw, 16});
    vec("PINSRWrri", {V::Pinsrw, 16});
    vec("CMPSDrri", {V::Cmpsd});
    vec("CMPSDrri_Int", {V::Cmpsd});
    vec("CMPSDrmi", {V::Cmpsd, 64, true});
    vec("CMPSDrmi_Int", {V::Cmpsd, 64, true});

    vecRM("PACKUSWB", {V::PackUnsigned,16});
    vecRM("PACKUSDW", {V::PackUnsigned,32});
    vecRM("PACKSSWB", {V::PackSigned,16});
    vecRM("PACKSSDW", {V::PackSigned,32});
    for (auto [suffix,lw] : {std::pair{"B",8},std::pair{"W",16}}) {
      vecRM(std::string("PADDUS")+suffix,{V::AddSatUnsigned,static_cast<ignil::Width>(lw)});
      vecRM(std::string("PSUBUS")+suffix,{V::SubSatUnsigned,static_cast<ignil::Width>(lw)});
      vecRM(std::string("PADDS")+suffix,{V::AddSatSigned,static_cast<ignil::Width>(lw)});
      vecRM(std::string("PSUBS")+suffix,{V::SubSatSigned,static_cast<ignil::Width>(lw)});
      vecRM(std::string("PAVG")+suffix,{V::Average,static_cast<ignil::Width>(lw)});
    }
    vecRM("PMULUDQ",{V::MulUnsignedDword,32});
    vecRM("PMULLW",{V::MulLow,16});
    vecRM("PMULLD",{V::MulLow,32});
    vecRM("PMULHW",{V::MulHighSigned,16});
    vecRM("PMULHUW",{V::MulHighUnsigned,16});
    vecRM("PMADDWD",{V::MaddSigned,16});
    vecRM("PSADBW",{V::Sad,8});
    for (const auto &[suffix,lw] : kLanes) {
      if (lw == 64) continue;
      const std::string sfx(suffix);
      vecRM("PMINS"+sfx,{V::MinSigned,lw});
      vecRM("PMAXS"+sfx,{V::MaxSigned,lw});
      vecRM("PMINU"+sfx,{V::MinUnsigned,lw});
      vecRM("PMAXU"+sfx,{V::MaxUnsigned,lw});
      vecRM("PABS"+sfx,{V::Abs,lw});
      vecRM("PSIGN"+sfx,{V::Sign,lw});
    }
    vecRM("PSHUFB",{V::ShuffleBytes,8});
    vec("PALIGNRrri",{V::AlignBytes,8});
    vec("PALIGNRrmi",{V::AlignBytes,8,true});
    vecRM("PTEST",{V::Test});
    for (const auto &[name,lw] : {std::pair{"BLENDPS",32},std::pair{"BLENDPD",64},std::pair{"PBLENDW",16}}) {
      vec(std::string(name)+"rri",{V::Blend,static_cast<ignil::Width>(lw)});
      vec(std::string(name)+"rmi",{V::Blend,static_cast<ignil::Width>(lw),true});
    }
    for (const auto &[suffix,lw] : kLanes) {
      const std::string sfx(suffix);
      vec("PEXTR"+sfx+"rri",{V::Extract,lw});
      VecOp extraction{V::Extract,lw}; extraction.memDst=true;
      vec("PEXTR"+sfx+"mri",extraction);
      vec("PINSR"+sfx+"rri",{V::Insert,lw});
      vec("PINSR"+sfx+"rmi",{V::Insert,lw,true});
    }
    for (const auto &[from,fw] : kLanes)
      for (const auto &[to,tw] : kLanes) {
        if (fw >= tw) continue;
        const std::string suffix=std::string(from)+std::string(to);
        VecOp sx{V::ExtendSigned,tw}; sx.sourceWidth=fw;
        VecOp zx{V::ExtendUnsigned,tw}; zx.sourceWidth=fw;
        vecRM("PMOVSX"+suffix,sx); vecRM("PMOVZX"+suffix,zx);
      }
    auto floatRM = [&](const std::string &base,VecOp operation) {
      for (const auto suffix : {"rr","rr_Int","r","r_Int"}) vec(base+suffix,operation);
      operation.memSrc=true;
      for (const auto suffix : {"rm","rm_Int","m","m_Int"}) vec(base+suffix,operation);
    };
    for (const auto &[suffix,width,scalar] :
         {std::tuple{"SS",32,true},std::tuple{"SD",64,true},std::tuple{"PS",32,false},std::tuple{"PD",64,false}}) {
      for (const auto &[name,kind] : {std::pair{"ADD",V::FloatAdd},std::pair{"SUB",V::FloatSub},
           std::pair{"MUL",V::FloatMul},std::pair{"DIV",V::FloatDiv},std::pair{"MIN",V::FloatMin},
           std::pair{"MAX",V::FloatMax},std::pair{"SQRT",V::FloatSqrt}}) {
        VecOp operation{kind,static_cast<ignil::Width>(width)}; operation.scalar=scalar;
        floatRM(std::string(name)+suffix,operation);
      }
      if (scalar) {
        VecOp compare{V::FloatCompareFlags,static_cast<ignil::Width>(width)};
        floatRM(std::string("COMI")+suffix,compare); floatRM(std::string("UCOMI")+suffix,compare);
        VecOp toInt{V::FloatToInt,static_cast<ignil::Width>(width)};
        floatRM(std::string("CVTT")+suffix+"2SI",toInt); floatRM(std::string("CVTT")+suffix+"2SI64",toInt);
        for (const unsigned iw : {32u,64u}) {
          VecOp fromInt{V::IntToFloat,static_cast<ignil::Width>(width)};
          fromInt.sourceWidth=iw;
          floatRM(std::string(iw == 64 ? "CVTSI642" : "CVTSI2")+suffix,fromInt);
        }
      }
      VecOp maskOp{V::FloatCompareMask,static_cast<ignil::Width>(width)}; maskOp.scalar=scalar;
      for (const auto tail : {"rri","rri_Int"}) vec(std::string("CMP")+suffix+tail,maskOp);
      maskOp.memSrc=true;
      for (const auto tail : {"rmi","rmi_Int"}) vec(std::string("CMP")+suffix+tail,maskOp);
    }
    floatRM("CVTPS2PD",{V::FloatWiden,64});
    floatRM("CVTPD2PS",{V::FloatNarrow,32});

    struct Names {
      std::string_view full, r8, r16, r32, r64;
    };
    static constexpr Names k64[] = {
            {"R11", "R11B", "R11W", "R11D", "R11"}, {"R10", "R10B", "R10W", "R10D", "R10"},
            {"R9", "R9B", "R9W", "R9D", "R9"},      {"R8", "R8B", "R8W", "R8D", "R8"},
            {"R15", "R15B", "R15W", "R15D", "R15"}, {"R14", "R14B", "R14W", "R14D", "R14"},
            {"R13", "R13B", "R13W", "R13D", "R13"}, {"R12", "R12B", "R12W", "R12D", "R12"},
            {"RBX", "BL", "BX", "EBX", "RBX"},      {"RSI", "SIL", "SI", "ESI", "RSI"},
            {"RDI", "DIL", "DI", "EDI", "RDI"}};
    static constexpr Names k32[] = {
            {"EBX", "BL", "BX", "EBX", ""}, {"EDX", "DL", "DX", "EDX", ""},
            {"ECX", "CL", "CX", "ECX", ""}, {"EAX", "AL", "AX", "EAX", ""},
            {"ESI", "", "SI", "ESI", ""},   {"EDI", "", "DI", "EDI", ""},
            {"EBP", "", "BP", "EBP", ""}};
    const bool is64 = architecture_ == Architecture::X86_64;
    for (const Names &n: is64 ? std::span<const Names>(k64) : std::span<const Names>(k32)) {
      ScratchReg c{llvmRegNamed(n.full),
                   {llvmRegNamed(n.r8), llvmRegNamed(n.r16), llvmRegNamed(n.r32),
                    llvmRegNamed(n.r64)}};
      if (c.full != 0 && toReg(c.full))
        scratchRegs_.push_back(c);
    }
  }
}

dragon::ignil::Block
dragon::x86::X86IGNILLifter::liftBlock(const BasicBlock &block,
                                       const Graph &graph) const {
  ignil::Block result{block.startAddress()};
  std::uint32_t tempId = 0;

  for (const Instruction *insn: block.instructions()) {
    liftInstruction(*insn, result, tempId);
  }

  // A block that ends without an explicit branch/return (execution simply
  // falls into the next block, e.g. an if/else arm with no trailing jump)
  // gets no JMP op from liftInstruction, since there is no branch
  // instruction to lift one from. Without a materialized terminator here,
  // LLVMLifter has no way to know where control flow goes next and emits a
  // basic block with no terminator at all - invalid LLVM IR that hangs
  // PromotePass's dominator-tree computation. Materialize the fall-through
  // edge explicitly using the CFG successor the disassembler already
  // computed.
  const bool alreadyTerminated =
          !result.empty() &&
          (result.ops().back().opcode == ignil::Opcode::JMP ||
           result.ops().back().opcode == ignil::Opcode::CJMP ||
           result.ops().back().opcode == ignil::Opcode::RET);
  if (!alreadyTerminated) {
    if (const auto &succ = block.successors(); succ.size() == 1) {
      result.append({ignil::Opcode::JMP,
                     std::nullopt,
                     {ignil::Const{*succ.begin(), 64}},
                     block.terminatorAddress()});
    }
  }

  return result;
}

dragon::ignil::Block dragon::x86::X86IGNILLifter::liftTrace(
        std::span<const Instruction> trace) const {
  if (trace.empty())
    return ignil::Block{0};

  ignil::Block result{trace.front().address()};
  std::uint32_t tempId = 0;

  for (const Instruction &insn: trace)
    liftInstruction(insn, result, tempId);

  return result;
}

bool dragon::x86::X86IGNILLifter::liftExtendedForm(unsigned opc,
                                                   const llvm::MCInst &mc,
                                                   std::uint64_t addr,
                                                   ignil::Block &out,
                                                   std::uint32_t &tmp) const {
  if (auto it = bmiOps_.find(opc); it != bmiOps_.end()) {
    liftBmi(mc, it->second, addr, out, tmp);
    return true;
  }
  if (auto it = vecOps_.find(opc); it != vecOps_.end()) {
    liftVec(mc, it->second, addr, out, tmp);
    return true;
  }
  // Early-return dispatch (no deep nesting) for the VMProtect-mutation forms.
  if (opc == opRol8ri_ || opc == opRol16ri_ || opc == opRol32ri_ ||
      opc == opRol64ri_) {
    liftRotate(mc, addr, out, tmp, /*left=*/true, ShiftCount::Imm);
    return true;
  }
  if (opc == opRol8r1_ || opc == opRol16r1_ || opc == opRol32r1_ ||
      opc == opRol64r1_) {
    liftRotate(mc, addr, out, tmp, /*left=*/true, ShiftCount::One);
    return true;
  }
  if (opc == opRol8rCL_ || opc == opRol16rCL_ || opc == opRol32rCL_ ||
      opc == opRol64rCL_) {
    liftRotate(mc, addr, out, tmp, /*left=*/true, ShiftCount::CL);
    return true;
  }
  if (opc == opRor8ri_ || opc == opRor16ri_ || opc == opRor32ri_ ||
      opc == opRor64ri_) {
    liftRotate(mc, addr, out, tmp, /*left=*/false, ShiftCount::Imm);
    return true;
  }
  if (opc == opRor8r1_ || opc == opRor16r1_ || opc == opRor32r1_ ||
      opc == opRor64r1_) {
    liftRotate(mc, addr, out, tmp, /*left=*/false, ShiftCount::One);
    return true;
  }
  if (opc == opRor8rCL_ || opc == opRor16rCL_ || opc == opRor32rCL_ ||
      opc == opRor64rCL_) {
    liftRotate(mc, addr, out, tmp, /*left=*/false, ShiftCount::CL);
    return true;
  }
  if (opc == opRor32mCL_) {
    liftRotate(mc, addr, out, tmp, /*left=*/false, ShiftCount::CL, 32);
    return true;
  }
  if (opc == opRcl8ri_ || opc == opRcl16ri_ || opc == opRcl32ri_ ||
      opc == opRcl64ri_) {
    liftRotateCarry(mc, addr, out, tmp, /*left=*/true, ShiftCount::Imm);
    return true;
  }
  if (opc == opRcl8r1_ || opc == opRcl16r1_ || opc == opRcl32r1_ ||
      opc == opRcl64r1_) {
    liftRotateCarry(mc, addr, out, tmp, /*left=*/true, ShiftCount::One);
    return true;
  }
  if (opc == opRcr8ri_ || opc == opRcr16ri_ || opc == opRcr32ri_ ||
      opc == opRcr64ri_) {
    liftRotateCarry(mc, addr, out, tmp, /*left=*/false, ShiftCount::Imm);
    return true;
  }
  if (opc == opRcr8r1_ || opc == opRcr16r1_ || opc == opRcr32r1_ ||
      opc == opRcr64r1_) {
    liftRotateCarry(mc, addr, out, tmp, /*left=*/false, ShiftCount::One);
    return true;
  }
  if (opc == opRcl8rCL_ || opc == opRcl16rCL_ || opc == opRcl32rCL_ ||
      opc == opRcl64rCL_) {
    liftRotateCarryCL(mc, addr, out, tmp, /*left=*/true);
    return true;
  }
  if (opc == opRcr8rCL_ || opc == opRcr16rCL_ || opc == opRcr32rCL_ ||
      opc == opRcr64rCL_) {
    liftRotateCarryCL(mc, addr, out, tmp, /*left=*/false);
    return true;
  }
  if (opc == opXchg16ar_ || opc == opXchg32ar_ || opc == opXchg64ar_) {
    liftXchgAcc(mc, addr, out, tmp);
    return true;
  }
  if (opc == opBsf16rr_ || opc == opBsf32rr_ || opc == opBsf64rr_) {
    liftBitScan(mc, addr, out, tmp, /*reverse=*/false);
    return true;
  }
  if (opc == opBsr16rr_ || opc == opBsr32rr_ || opc == opBsr64rr_) {
    liftBitScan(mc, addr, out, tmp, /*reverse=*/true);
    return true;
  }
  if (opc == opBts16ri8_ || opc == opBts32ri8_ || opc == opBts64ri8_ ||
      opc == opBts16rr_ || opc == opBts32rr_ || opc == opBts64rr_) {
    liftBitModify(mc, addr, out, tmp, BitOp::Set);
    return true;
  }
  if (opc == opBtr16ri8_ || opc == opBtr32ri8_ || opc == opBtr64ri8_ ||
      opc == opBtr16rr_ || opc == opBtr32rr_ || opc == opBtr64rr_) {
    liftBitModify(mc, addr, out, tmp, BitOp::Reset);
    return true;
  }
  if (opc == opBtr32mi8_) {
    liftBitModify(mc, addr, out, tmp, BitOp::Reset, 32);
    return true;
  }
  if (opc == opBtc16ri8_ || opc == opBtc32ri8_ || opc == opBtc64ri8_ ||
      opc == opBtc16rr_ || opc == opBtc32rr_ || opc == opBtc64rr_) {
    liftBitModify(mc, addr, out, tmp, BitOp::Complement);
    return true;
  }
  if (opc == opCmov16rr_ || opc == opCmov32rr_ || opc == opCmov64rr_) {
    liftCmov(mc, addr, out, tmp);
    return true;
  }
  if (opc == opXadd8rr_ || opc == opXadd16rr_ || opc == opXadd32rr_ ||
      opc == opXadd64rr_) {
    liftXadd(mc, addr, out, tmp);
    return true;
  }
  if (opc == opInc8m_ || opc == opInc16m_ || opc == opInc32m_ ||
      opc == opInc64m_) {
    liftIncDecMem(mc, addr, out, tmp, /*dec=*/false);
    return true;
  }
  if (opc == opDec8m_ || opc == opDec16m_ || opc == opDec32m_ ||
      opc == opDec64m_) {
    liftIncDecMem(mc, addr, out, tmp, /*dec=*/true);
    return true;
  }
  if (opMov8riAlt_ != 0 && opc == opMov8riAlt_) {
    liftMov(mc, addr, out, tmp);
    return true;
  }
  if (opLahf_ != 0 && opc == opLahf_) {
    liftLahf(addr, out, tmp);
    return true;
  }
  if (opSahf_ != 0 && opc == opSahf_) {
    liftSahf(addr, out, tmp);
    return true;
  }
  return false;
}

void dragon::x86::X86IGNILLifter::liftInstruction(const Instruction &insn,
                                                  ignil::Block &out,
                                                  std::uint32_t &tempId) const {
  const auto *mc = insn.mcInst();
  if (!mc)
    return;

  if (liftString(insn,out,tempId)) return;

  unsigned opc = mc->getOpcode();
  std::uint64_t addr = insn.address();

  // Normalize load-direction *_REV reg-reg encodings to their base opcode;
  // they share MCInst operand order, so the base handler lifts them correctly.
  if (auto it = revToBase_.find(opc); it != revToBase_.end())
    opc = it->second;

  // Accumulator short forms name their register implicitly; rebuild them as
  // the generic reg-imm instruction so the ALU lifters below apply unchanged.
  llvm::MCInst accumulatorForm;
  if (auto it = accForms_.find(opc); it != accForms_.end() &&
                                     mc->getNumOperands() == 1 &&
                                     mc->getOperand(0).isImm()) {
    const AccForm &form = it->second;
    accumulatorForm.setOpcode(form.genericOpcode);
    accumulatorForm.addOperand(llvm::MCOperand::createReg(form.llvmReg));
    if (form.tiedDst)
      accumulatorForm.addOperand(llvm::MCOperand::createReg(form.llvmReg));
    accumulatorForm.addOperand(mc->getOperand(0));
    mc = &accumulatorForm;
    opc = form.genericOpcode;
  }

  // A RIP-relative operand means "the address of the next instruction". The
  // operand lifters read it as the RIP register, so pin that register to the
  // right constant first - otherwise a trace (many instructions spliced into
  // one block) would read whatever RIP the caller last seeded, for every
  // instruction alike.
  for (unsigned i = 0; i < mc->getNumOperands(); ++i) {
    const llvm::MCOperand &operand = mc->getOperand(i);
    if (!operand.isReg())
      continue;
    const auto reg = toReg(operand.getReg());
    if (reg && regRIP_.width != 0 && reg->offset == regRIP_.offset) {
      out.append({ignil::Opcode::COPY,
                  regRIP_,
                  {ignil::Const{addr + insn.size(), 64}},
                  addr});
      break;
    }
  }

  // Memory-operand forms lifted through their register form: from here on
  // `mc` is that register form over a scratch register; the epilogue at the
  // end of this function stores the result back and restores the scratch.
  llvm::MCInst scratchForm;
  std::optional<ScratchState> scratch;
  if (auto it = memViaReg_.find(opc); it != memViaReg_.end()) {
    ScratchState state;
    if (auto rewritten = rewriteThroughScratch(*mc, it->second, addr, out,
                                               tempId, state)) {
      scratchForm = std::move(*rewritten);
      mc = &scratchForm;
      opc = it->second.regOpcode;
      scratch = state;
    }
  }

  if (opc == opMov64rr_ || opc == opMov32rr_ || opc == opMov64ri_ ||
      opc == opMov32ri_ || opc == opMov64ri32_)
    liftMov(*mc, addr, out, tempId);
  else if (opc == opMov64rm_ || opc == opMov32rm_ || opc == opMov16rm_ ||
           opc == opMov8rm_)
    liftMovLoad(*mc, addr, out, tempId);
  else if (opc == opMov64mr_ || opc == opMov32mr_ || opc == opMov16mr_ ||
           opc == opMov8mr_ || opc == opMov64mi_ || opc == opMov32mi_ ||
           opc == opMov8mi_)
    liftMovStore(*mc, addr, out, tempId);
  else if (opc == opAdd64rr_ || opc == opAdd32rr_ || opc == opAdd64ri_ ||
           opc == opAdd32ri_ || opc == opAdd64ri8_ || opc == opAdd32ri8_)
    liftAdd(*mc, addr, out, tempId);
  else if (opc == opAdd64mr_ || opc == opAdd32mr_ || opc == opAdd8mr_ ||
           opc == opAdd64mi_ || opc == opAdd32mi_ || opc == opAdd64mi8_ ||
           opc == opAdd32mi8_)
    liftAddMem(*mc, addr, out, tempId);
  else if (opc == opAdd64rm_ || opc == opAdd32rm_)
    liftAddRegMem(*mc, addr, out, tempId);
  else if (opc == opSub64rr_ || opc == opSub32rr_ || opc == opSub64ri_ ||
           opc == opSub32ri_ || opc == opSub64ri8_ || opc == opSub32ri8_)
    liftSub(*mc, addr, out, tempId);
  else if (opc == opSub64mr_ || opc == opSub32mr_ || opc == opSub64mi_ ||
           opc == opSub32mi_ || opc == opSub64mi8_ || opc == opSub32mi8_)
    liftSubMem(*mc, addr, out, tempId);
  else if (opc == opSub64rm_ || opc == opSub32rm_)
    liftSubRegMem(*mc, addr, out, tempId);
  else if (opc == opXor32rm_)
    liftLogicRegMem(*mc, addr, out, tempId, ignil::Opcode::XOR);
  else if (opc == opXor32mi_ || opc == opXor32mr_)
    liftLogicMem(*mc, addr, out, tempId, ignil::Opcode::XOR);
  else if (opc == opAnd32mr_)
    liftLogicMem(*mc, addr, out, tempId, ignil::Opcode::AND);
  else if (opc == opXor64rr_ || opc == opXor32rr_ || opc == opXor64ri_ ||
           opc == opXor32ri_ || opc == opXor64ri8_ || opc == opXor32ri8_)
    liftXor(*mc, addr, out, tempId);
  else if (opc == opAnd64rr_ || opc == opAnd32rr_ || opc == opAnd64ri_ ||
           opc == opAnd32ri_ || opc == opAnd64ri8_ || opc == opAnd32ri8_)
    liftAnd(*mc, addr, out, tempId);
  else if (opc == opOr64rr_ || opc == opOr32rr_ || opc == opOr64ri_ ||
           opc == opOr32ri_ || opc == opOr64ri8_ || opc == opOr32ri8_)
    liftOr(*mc, addr, out, tempId);
  else if (opc == opPush64r_ || opc == opPush32r_)
    liftPush(*mc, addr, out, tempId);
  else if (opc == opPush32rmm_)
    liftPushMem(*mc, addr, out, tempId);
  else if (opc == opPop64r_ || opc == opPop32r_)
    liftPop(*mc, addr, out, tempId);
  else if (opc == opPop32rmm_)
    liftPopMem(*mc, addr, out, tempId);
  else if (opc == opPopf64_ || opc == opPopf32_)
    liftPopf(*mc, addr, out, tempId);
  else if (opc == opPushf64_ || opc == opPushf32_)
    liftPushf(*mc, addr, out, tempId);
  else if (opc == opPusha32_)
    liftPusha(*mc, addr, out, tempId);
  else if (opc == opPopa32_)
    liftPopa(*mc, addr, out, tempId);
  else if (opc == opPushSS32_)
    liftPushSeg(*mc, addr, out, tempId);
  else if (opc == opPopSS32_)
    liftPopSeg(*mc, addr, out, tempId);
  else if (opc == opLea64r_ || opc == opLea32r_)
    liftLea(*mc, addr, out, tempId);
  else if (opc == opCmp64rr_ || opc == opCmp32rr_ || opc == opCmp64ri_ ||
           opc == opCmp32ri_ || opc == opCmp64ri8_ || opc == opCmp32ri8_)
    liftCmp(*mc, addr, out, tempId);
  else if (opc == opCmp64rm_ || opc == opCmp32rm_)
    liftCmpRegMem(*mc, addr, out, tempId);
  else if (opc == opCmp64mr_ || opc == opCmp32mr_ || opc == opCmp64mi_ ||
           opc == opCmp32mi_ || opc == opCmp64mi8_ || opc == opCmp32mi8_ ||
           opc == opCmp8mi_)
    liftCmpMem(*mc, addr, out, tempId);
  else if (opc == opTest64rr_ || opc == opTest32rr_ || opc == opTest64ri_ ||
           opc == opTest32ri_)
    liftTest(*mc, addr, out, tempId);
  else if (opc == opCall_ || opc == opCall32_ || opc == opCall64r_ ||
           opc == opCall32r_)
    liftCall(*mc, addr, insn.size(), out, tempId);
  // LOOP/LOOPE/LOOPNE and JCXZ/JECXZ/JRCXZ have no condition-code operand.
  // They must be dispatched before the generic isConditionalBranch()
  // fallback, whose liftJcc reads a missing operand as CondCode 0 (O) -
  // which lifted `jrcxz`/`loope`/`loopne` as `jo` (and dropped LOOPcc's
  // counter decrement).
  else if (opc == opLoop_ || opc == opLoope_ || opc == opLoopne_ ||
           opc == opJcxz_ || opc == opJecxz_ || opc == opJrcxz_)
    liftLoop(*mc, addr, insn.size(), hasAddressSizePrefix(insn), out, tempId);
  else if (opc == opRet64_ || opc == opRet32_ || opc == opRetI64_ ||
           opc == opRetI32_)
    liftRet(*mc, addr, out, tempId);
  else if (opc == opLeave64_ || opc == opLeave_)
    liftLeave(*mc, addr, out, tempId);
  else if (opc == opJmp64m_ || opc == opJmp32m_)
    liftJmpMem(*mc, addr, insn.size(), out, tempId);
  else if (opc == opCall64m_ || opc == opCall32m_)
    liftCallMem(*mc, addr, insn.size(), out, tempId);
  else if (auto zx = movzxForms_.find(opc); zx != movzxForms_.end())
    liftMovExtend(*mc, addr, zx->second, /*signExtend=*/false, out, tempId);
  else if (auto sx = movsxForms_.find(opc); sx != movsxForms_.end())
    liftMovExtend(*mc, addr, sx->second, /*signExtend=*/true, out, tempId);
  else if (opc == opSar32ri_ || opc == opSar64ri_)
    liftSarImm(*mc, addr, out, tempId);
  else if (opc == opImul64rri32_ || opc == opImul64rri8_ ||
           opc == opImul32rri_ || opc == opImul32rri8_ ||
           opc == opImul16rri_ || opc == opImul16rri8_)
    liftImulImm(*mc, addr, out, tempId);
  else if (opc == opImul32rr_ || opc == opImul64rr_ || opc == opImul16rr_)
    liftImulRR(*mc, addr, out, tempId);
  else if (opc == opLea64_32r_)
    liftLeaTrunc32(*mc, addr, out, tempId);
  else if (opc == opInc32r_ || opc == opInc32r_alt_ || opc == opInc64r_)
    liftInc(*mc, addr, out, tempId);
  else if (opc == opDec32r_ || opc == opDec32r_alt_ || opc == opDec64r_)
    liftDec(*mc, addr, out, tempId);
  else if (opc == opNeg32r_ || opc == opNeg64r_)
    liftNeg(*mc, addr, out, tempId);
  else if (opc == opShl32ri_ || opc == opShl64ri_)
    liftShlImm(*mc, addr, out, tempId);
  else if (opc == opShr32ri_ || opc == opShr64ri_)
    liftShrImm(*mc, addr, out, tempId);
  else if (opc == opSar32r1_ || opc == opSar64r1_)
    liftSarR1(*mc, addr, out, tempId);
  else if (opc == opShl32r1_ || opc == opShl64r1_)
    liftShlR1(*mc, addr, out, tempId);
  else if (opc == opShr32r1_ || opc == opShr64r1_)
    liftShrR1(*mc, addr, out, tempId);
  // LifterGaps.md #7 round 2 - 8/16-bit forms of the generic lifters.
  else if (opc == opMov8rr_ || opc == opMov16rr_ || opc == opMov8ri_ ||
           opc == opMov16ri_)
    liftMov(*mc, addr, out, tempId);
  else if (opc == opAdd8rr_ || opc == opAdd8ri_ || opc == opAdd8ri8_ ||
           opc == opAdd16rr_ || opc == opAdd16ri_ || opc == opAdd16ri8_)
    liftAdd(*mc, addr, out, tempId);
  else if (opc == opSub8rr_ || opc == opSub8ri_ || opc == opSub8ri8_ ||
           opc == opSub16rr_ || opc == opSub16ri_ || opc == opSub16ri8_)
    liftSub(*mc, addr, out, tempId);
  else if (opc == opAnd8rr_ || opc == opAnd8ri_ || opc == opAnd8ri8_ ||
           opc == opAnd16rr_ || opc == opAnd16ri_ || opc == opAnd16ri8_)
    liftAnd(*mc, addr, out, tempId);
  else if (opc == opOr8rr_ || opc == opOr8ri_ || opc == opOr8ri8_ ||
           opc == opOr16rr_ || opc == opOr16ri_ || opc == opOr16ri8_)
    liftOr(*mc, addr, out, tempId);
  else if (opc == opXor8rr_ || opc == opXor8ri_ || opc == opXor8ri8_ ||
           opc == opXor16rr_ || opc == opXor16ri_ || opc == opXor16ri8_)
    liftXor(*mc, addr, out, tempId);
  else if (opc == opCmp8rr_ || opc == opCmp8ri_ || opc == opCmp8ri8_ ||
           opc == opCmp16rr_ || opc == opCmp16ri_ || opc == opCmp16ri8_)
    liftCmp(*mc, addr, out, tempId);
  else if (opc == opTest8rr_ || opc == opTest16rr_ || opc == opTest8ri_ ||
           opc == opTest16ri_)
    liftTest(*mc, addr, out, tempId);
  else if (opc == opInc8r_ || opc == opInc16r_ || opc == opInc16r_alt_)
    liftInc(*mc, addr, out, tempId);
  else if (opc == opDec8r_ || opc == opDec16r_ || opc == opDec16r_alt_)
    liftDec(*mc, addr, out, tempId);
  else if (opc == opXchg8rr_ || opc == opXchg16rr_ || opc == opXchg32rr_ ||
           opc == opXchg64rr_)
    liftXchg(*mc, addr, out, tempId);
  else if (opc == opXchg32rm_ || opc == opXchg8rm_ || opc == opXchg16rm_ ||
           opc == opXchg64rm_)
    liftXchgMem(*mc, addr, out, tempId);
  else if (opc == opBswap32r_ || opc == opBswap64r_)
    liftBswap(*mc, addr, out, tempId);
  else if (opc == opShld16rrCL_ || opc == opShld32rrCL_ || opc == opShld64rrCL_)
    liftShiftDouble(*mc, addr, out, tempId, /*left=*/true, /*byCL=*/true);
  else if (opc == opShrd16rrCL_ || opc == opShrd32rrCL_ || opc == opShrd64rrCL_)
    liftShiftDouble(*mc, addr, out, tempId, /*left=*/false, /*byCL=*/true);
  else if (opc == opShld16rri8_ || opc == opShld32rri8_ || opc == opShld64rri8_)
    liftShiftDouble(*mc, addr, out, tempId, /*left=*/true, /*byCL=*/false);
  else if (opc == opShrd16rri8_ || opc == opShrd32rri8_ || opc == opShrd64rri8_)
    liftShiftDouble(*mc, addr, out, tempId, /*left=*/false, /*byCL=*/false);
  else if (opc == opPush32I_ || opc == opPush32I8_ || opc == opPush64I32_ || opc == opPush64I8_)
    liftPushImm(*mc, addr, out, tempId);
  // LifterGaps.md #7 - VMProtect handler forms.
  else if (opc == opNot8r_ || opc == opNot16r_ || opc == opNot32r_ ||
           opc == opNot64r_)
    liftNot(*mc, addr, out, tempId);
  else if (opc == opNeg8r_ || opc == opNeg16r_)
    liftNeg(*mc, addr, out, tempId);
  else if (opc == opShl8ri_ || opc == opShl16ri_)
    liftShlImm(*mc, addr, out, tempId);
  else if (opc == opShr8ri_ || opc == opShr16ri_)
    liftShrImm(*mc, addr, out, tempId);
  else if (opc == opSar8ri_ || opc == opSar16ri_)
    liftSarImm(*mc, addr, out, tempId);
  else if (opc == opShl8r1_ || opc == opShl16r1_)
    liftShlR1(*mc, addr, out, tempId);
  else if (opc == opShr8r1_ || opc == opShr16r1_)
    liftShrR1(*mc, addr, out, tempId);
  else if (opc == opSar8r1_ || opc == opSar16r1_)
    liftSarR1(*mc, addr, out, tempId);
  else if (opc == opShl8rCL_ || opc == opShl16rCL_ || opc == opShl32rCL_ ||
           opc == opShl64rCL_)
    liftShiftCL(*mc, addr, out, tempId, ignil::Opcode::SHL);
  else if (opc == opShr8rCL_ || opc == opShr16rCL_ || opc == opShr32rCL_ ||
           opc == opShr64rCL_)
    liftShiftCL(*mc, addr, out, tempId, ignil::Opcode::SHR);
  else if (opc == opShr32mCL_)
    liftShiftCL(*mc, addr, out, tempId, ignil::Opcode::SHR, 32);
  else if (opc == opSar8rCL_ || opc == opSar16rCL_ || opc == opSar32rCL_ ||
           opc == opSar64rCL_)
    liftShiftCL(*mc, addr, out, tempId, ignil::Opcode::SAR);
  else if (opc == opCbw_)
    liftAccExtend(addr, out, tempId, AccExtend::CBW);
  else if (opc == opCwde_)
    liftAccExtend(addr, out, tempId, AccExtend::CWDE);
  else if (opc == opCdqe_)
    liftAccExtend(addr, out, tempId, AccExtend::CDQE);
  else if (opc == opCwd_)
    liftAccExtend(addr, out, tempId, AccExtend::CWD);
  else if (opc == opCdq_)
    liftAccExtend(addr, out, tempId, AccExtend::CDQ);
  else if (opc == opCqo_)
    liftAccExtend(addr, out, tempId, AccExtend::CQO);
  else if (opc == opSetccr_ && opSetccr_ != 0)
    liftSetcc(*mc, addr, out, tempId);
  else if (auto adc = adcSbbForms_.find(opc); adc != adcSbbForms_.end())
    liftAdcSbb(*mc, addr, out, tempId, adc->second);
  else if (auto md = mulDivForms_.find(opc); md != mulDivForms_.end())
    liftMulDiv(*mc, addr, out, tempId, md->second);
  else if (opc == opBt16ri8_ || opc == opBt32ri8_ || opc == opBt64ri8_ ||
           opc == opBt16rr_ || opc == opBt32rr_ || opc == opBt64rr_)
    liftBt(*mc, addr, out, tempId);
  // The rotate / bit-op / CMOV / XADD / memory INC-DEC / LAHF-SAHF family is
  // dispatched in its own helper. Kept out of this else-if chain on purpose:
  // MSVC caps how deeply blocks may nest (C1061), and a long chain of `else
  // if` counts against that limit.
  else if (liftExtendedForm(opc, *mc, addr, out, tempId))
    ; // handled
  else if (opc == opStc_)
    liftCarryOp(addr, out, tempId, CarryOp::STC);
  else if (opc == opClc_)
    liftCarryOp(addr, out, tempId, CarryOp::CLC);
  else if (opc == opCmc_)
    liftCarryOp(addr, out, tempId, CarryOp::CMC);
  else if (opc == opCld_)
    liftCarryOp(addr, out, tempId, CarryOp::CLD);
  else if (opc == opCpuid_)
    liftOpaque(addr, out, tempId, ignil::OpaqueKind::CPUID);
  else if (opc == opRdtsc_)
    liftOpaque(addr, out, tempId, ignil::OpaqueKind::RDTSC);
  else if (opc == opRdtscp_)
    liftOpaque(addr, out, tempId, ignil::OpaqueKind::RDTSCP);
  else if (opc == opCvtsi2sd32_ || opc == opCvtsi2sd64_)
    liftCvtsi2sd(*mc, addr, out, tempId);
  else if (opc == opCvttsd2si32_ || opc == opCvttsd2si64_)
    liftCvttsd2si(*mc, addr, out, tempId);
  else if (opc == opAddsd_)
    liftAddsd(*mc, addr, out, tempId);
  else if (opc == opSubsd_)
    liftSubsd(*mc, addr, out, tempId);
  else if (opc == opMulsd_)
    liftMulsd(*mc, addr, out, tempId);
  else if (opc == opMulsdMem_)
    liftMulsdMem(*mc, addr, out, tempId);
  else if (opc == opMovsdRR_)
    liftMovsdRR(*mc, addr, out, tempId);
  else if (opc == opMovsdLoad_) {
    liftMovLoad(*mc, addr, out, tempId);
    // movsd xmm, m64 zeroes bits 127:64 (movsd xmm, xmm keeps them).
    if (auto d = toReg(mc->getOperand(0).getReg()))
      if (auto hi = xmmHigh(*d))
        out.append({ignil::Opcode::COPY, *hi, {ignil::Const{0, 64}}, addr});
  }
  else if (opc == opMovsdStore_)
    liftMovStore(*mc, addr, out, tempId);
  else if (auto so = sseOps_.find(opc); so != sseOps_.end())
    liftSse(*mc, so->second, addr, out, tempId);
  else if (auto mo = memOperandOps_.find(opc); mo != memOperandOps_.end())
    liftMemOperandForm(*mc, mo->second, addr, out, tempId);
  else if (nopOpcodes_.contains(opc)) {
    // No effect. A multi-byte nop has a memory operand it never accesses,
    // so the generic fallback below reported every one of them unlifted
    // (441 in an x86-64 mingw build of Lua - the most common "unlifted"
    // instruction there).
  } else {
    const auto &desc = instrInfo_->get(opc);
    if (desc.isConditionalBranch()) {
      liftJcc(*mc, addr, insn.size(), out, tempId);
    } else if (desc.isBranch()) {
      liftJmp(*mc, addr, insn.size(), out, tempId);
    } else {
      // Unknown instruction. Side-effect-free ones (prefetch, fence, nop,
      // ...) have no explicit register-def operands, so nothing is emitted
      // for them - preserves the existing "no UNDEF for no-op instructions"
      // behavior. Anything that *does* define a register but has no lifter
      // yet must still mark that register UNDEF, so downstream flag/value
      // reads see an honest "unknown" instead of a silently stale value
      // (this is what let a dropped mem-destination MOV/ADD/SUB/CMP corrupt
      // later flag reads before the dedicated lifters above existed).
      // An XMM/YMM/ZMM destination clobbers both modelled halves (a YMM
      // has no IGNIL register of its own, so toReg alone would clobber
      // nothing and leave its low 128 bits stale).
      auto clobber = [&](unsigned llvmReg) {
        if (auto v = vecRegIndex_.find(llvmReg); v != vecRegIndex_.end()) {
          emitUndef(regXmmLo_[v->second], addr, out);
          emitUndef(regXmmHi_[v->second], addr, out);
        } else if (auto dst = toReg(llvmReg)) {
          emitUndef(*dst, addr, out);
        }
      };
      for (unsigned i = 0; i < desc.getNumDefs(); ++i) {
        const auto &defOp = mc->getOperand(i);
        if (!defOp.isReg())
          continue;
        clobber(defOp.getReg());
      }
      // The same goes for registers the instruction defines implicitly - the
      // accumulator of `mul`, RCX/RSI/RDI of a string op. They are not MCInst
      // operands, so the loop above never sees them; without this an
      // unlifted `mul rbx` leaves RAX and RDX holding their old values and
      // every consumer downstream is confidently wrong (LifterGaps.md #5).
      bool definesFlags = false;
      for (const llvm::MCPhysReg implicitDef: desc.implicit_defs()) {
        if (implicitDef == llvmEflags_)
          definesFlags = true;
        else
          clobber(implicitDef);
      }
      // EFLAGS is not an IGNIL register (the flags are separate ones), so it
      // needs spelling out: an unlifted flag-writing instruction ahead of a
      // `pushfq` or a `jcc` must not leave the previous instruction's flags
      // in place.
      if (definesFlags && llvmEflags_ != 0)
        emitArithFlags(addr, out, tempId);

      // Anything that defines something, or has a memory operand it may write
      // through, was really not lifted - as opposed to a nop or a prefetch.
      constexpr unsigned kMemoryOperandCount = 5;
      if (desc.getNumDefs() != 0 || !desc.implicit_defs().empty() ||
          mc->getNumOperands() >= kMemoryOperandCount)
        out.noteUnlifted(addr);
    }
  }

  if (scratch) {
    if (scratch->storeBack) {
      auto result = newTemp(tempId, scratch->view.width);
      out.append({ignil::Opcode::COPY, result, {scratch->view}, addr});
      out.append({ignil::Opcode::STORE, std::nullopt,
                  {ignil::Value{scratch->ea}, ignil::Value{result}}, addr});
    }
    out.append({ignil::Opcode::COPY, scratch->full,
                {ignil::Value{scratch->saved}}, addr});
  }
}

void dragon::x86::X86IGNILLifter::liftBmi(const llvm::MCInst &mc, BmiKind kind,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  using Op = ignil::Opcode;
  using B = BmiKind;
  // op[0]=dst (MULX: op[0]=high, op[1]=low), then the sources.
  const unsigned firstSrc = kind == B::Mulx ? 2 : 1;
  std::vector<ignil::Reg> defs;
  for (unsigned i = 0; i < firstSrc; ++i)
    if (auto r = toReg(mc.getOperand(i).getReg()))
      defs.push_back(*r);
  auto srcReg = toReg(mc.getOperand(firstSrc).getReg());
  if (defs.size() != firstSrc || !srcReg) {
    logger_.log("Error liftBmi in addr {:#x} bad operands", addr);
    for (const auto &d: defs)
      emitUndef(d, addr, out);
    out.noteUnlifted(addr);
    return;
  }
  const ignil::Width w = defs[0].width;
  const std::uint64_t W = w;
  const std::uint64_t ones = W == 64 ? ~0ull : (1ull << W) - 1;
  auto k = [&](std::uint64_t v) { return ignil::Const{v & ones, w}; };
  auto bin = [&](Op op, ignil::Value a, ignil::Value b) {
    auto t = newTemp(tmp, w);
    out.append({op, t, {a, b}, addr});
    return ignil::Value{t};
  };
  auto un = [&](Op op, ignil::Value a) {
    auto t = newTemp(tmp, w);
    out.append({op, t, {a}, addr});
    return ignil::Value{t};
  };
  auto bit = [&](Op op, ignil::Value a, ignil::Value b) {
    auto t = newTemp(tmp, 1);
    out.append({op, t, {a, b}, addr});
    return ignil::Value{t};
  };
  auto read = [&](ignil::Reg r) {
    auto t = newTemp(tmp, r.width);
    out.append({Op::COPY, t, {r}, addr});
    return ignil::Value{t};
  };
  auto popcount = [&](ignil::Value x) {
    x = bin(Op::SUB, x, bin(Op::AND, bin(Op::SHR, x, k(1)), k(0x5555555555555555)));
    x = bin(Op::ADD, bin(Op::AND, x, k(0x3333333333333333)),
            bin(Op::AND, bin(Op::SHR, x, k(2)), k(0x3333333333333333)));
    x = bin(Op::AND, bin(Op::ADD, x, bin(Op::SHR, x, k(4))), k(0x0f0f0f0f0f0f0f0f));
    return bin(Op::SHR, bin(Op::MUL, x, k(0x0101010101010101)), k(W - 8));
  };
  auto setFlag = [this, addr](ignil::Reg flag, ignil::Value v) -> FlagSetter {
    return [flag, v, addr](ignil::Block &o, std::uint32_t &) {
      o.append({Op::COPY, flag, {v}, addr});
    };
  };
  const ignil::Value zero1 = ignil::Const{0, 1};

  const ignil::Value src = read(*srcReg);
  std::optional<ignil::Value> src2;
  if (firstSrc + 1 < mc.getNumOperands()) {
    const auto &op = mc.getOperand(firstSrc + 1);
    if (op.isReg()) {
      auto r = toReg(op.getReg());
      if (!r) {
        logger_.log("Error liftBmi in addr {:#x} bad operands", addr);
        emitUndef(defs[0], addr, out);
        out.noteUnlifted(addr);
        return;
      }
      src2 = read(*r);
    } else if (op.isImm()) {
      src2 = k(static_cast<std::uint64_t>(op.getImm()));
    }
  }

  ignil::Value result = src;
  std::map<X86Flag, FlagSetter> flags;
  bool writesFlags = true;
  switch (kind) {
    case B::Popcnt:
      result = popcount(src);
      flags = {{X86Flag::ZF, setFlag(regZF_, bit(Op::EQ, src, k(0)))},
               {X86Flag::CF, setFlag(regCF_, zero1)}, {X86Flag::OF, setFlag(regOF_, zero1)},
               {X86Flag::SF, setFlag(regSF_, zero1)}, {X86Flag::AF, setFlag(regAF_, zero1)},
               {X86Flag::PF, setFlag(regPF_, zero1)}};
      break;
    case B::Tzcnt:
    case B::Lzcnt: {
      if (kind == B::Tzcnt) {
        // (x & -x) - 1 has ones exactly below the lowest set bit; for x = 0
        // it is all ones, giving w.
        result = popcount(bin(Op::SUB, bin(Op::AND, src, un(Op::NEG, src)), k(1)));
      } else {
        ignil::Value s = src;
        for (std::uint64_t sh = 1; sh < W; sh <<= 1)
          s = bin(Op::OR, s, bin(Op::SHR, s, k(sh)));
        result = bin(Op::SUB, k(W), popcount(s));
      }
      flags = {{X86Flag::CF, setFlag(regCF_, bit(Op::EQ, src, k(0)))},
               {X86Flag::ZF, setFlag(regZF_, bit(Op::EQ, result, k(0)))}};
      break;
    }
    case B::Shlx:
    case B::Shrx:
    case B::Sarx: {
      const auto amount = bin(Op::AND, *src2, k(W - 1));
      result = bin(kind == B::Shlx ? Op::SHL : kind == B::Shrx ? Op::SHR : Op::SAR,
                   src, amount);
      writesFlags = false;
      break;
    }
    case B::Rorx: {
      const auto amount = bin(Op::AND, *src2, k(W - 1));
      const auto back = bin(Op::AND, bin(Op::SUB, k(W), amount), k(W - 1));
      result = bin(Op::OR, bin(Op::SHR, src, amount), bin(Op::SHL, src, back));
      writesFlags = false;
      break;
    }
    case B::Andn:
      result = bin(Op::AND, un(Op::NOT, src), *src2);
      flags = {{X86Flag::ZF, zfEqZero(result, addr)}, {X86Flag::SF, sfSignBit(result, addr)},
               {X86Flag::CF, setFlag(regCF_, zero1)}, {X86Flag::OF, setFlag(regOF_, zero1)}};
      break;
    case B::Blsr:
    case B::Blsi:
    case B::Blsmsk: {
      const auto minus1 = bin(Op::SUB, src, k(1));
      result = kind == B::Blsr  ? bin(Op::AND, src, minus1)
               : kind == B::Blsi ? bin(Op::AND, src, un(Op::NEG, src))
                                 : bin(Op::XOR, src, minus1);
      const auto cf = bit(kind == B::Blsi ? Op::NEQ : Op::EQ, src, k(0));
      flags = {{X86Flag::ZF, zfEqZero(result, addr)}, {X86Flag::SF, sfSignBit(result, addr)},
               {X86Flag::CF, setFlag(regCF_, cf)}, {X86Flag::OF, setFlag(regOF_, zero1)}};
      break;
    }
    case B::Bzhi: {
      // n = src2[7:0]; n < w clears bits n.., otherwise src passes through.
      const auto n = bin(Op::AND, *src2, k(0xff));
      const auto inRange = bit(Op::ULT, n, k(W));
      const auto keepLow = bin(Op::SUB, bin(Op::SHL, k(1), bin(Op::AND, n, k(W - 1))), k(1));
      const auto rangeMask = un(Op::NEG, un(Op::ZEXT, inRange));
      // mask = inRange ? keepLow : all ones
      const auto mask = bin(Op::OR, bin(Op::AND, keepLow, rangeMask), un(Op::NOT, rangeMask));
      result = bin(Op::AND, src, mask);
      flags = {{X86Flag::ZF, zfEqZero(result, addr)}, {X86Flag::SF, sfSignBit(result, addr)},
               {X86Flag::CF, setFlag(regCF_, bit(Op::UGE, n, k(W)))},
               {X86Flag::OF, setFlag(regOF_, zero1)}};
      break;
    }
    case B::Mulx: {
      const ignil::Reg dx = W == 64 ? regRDX_ : regEDX_;
      if (dx.width != w) {
        for (const auto &d: defs)
          emitUndef(d, addr, out);
        out.noteUnlifted(addr);
        return;
      }
      const auto wide = static_cast<ignil::Width>(2 * w);
      auto ext = [&](ignil::Value v) {
        auto t = newTemp(tmp, wide);
        out.append({Op::ZEXT, t, {v}, addr});
        return ignil::Value{t};
      };
      auto product = newTemp(tmp, wide);
      out.append({Op::MUL, product, {ext(read(dx)), ext(src)}, addr});
      auto lo = newTemp(tmp, w);
      out.append({Op::TRUNC, lo, {ignil::Value{product}}, addr});
      auto shifted = newTemp(tmp, wide);
      out.append({Op::SHR, shifted, {ignil::Value{product}, ignil::Const{W, 8}}, addr});
      auto hi = newTemp(tmp, w);
      out.append({Op::TRUNC, hi, {ignil::Value{shifted}}, addr});
      // Low first: with the same register for both, the high half wins.
      out.append({Op::COPY, defs[1], {ignil::Value{lo}}, addr});
      out.append({Op::COPY, defs[0], {ignil::Value{hi}}, addr});
      return;
    }
  }
  out.append({Op::COPY, defs[0], {result}, addr});
  if (writesFlags)
    emitArithFlags(addr, out, tmp, flags);
}

std::optional<dragon::ignil::Reg>
dragon::x86::X86IGNILLifter::xmmHigh(const ignil::Reg &low) const {
  if (low.width != 64)
    return std::nullopt;
  for (unsigned n = 0; n < 16; ++n)
    if (regXmmLo_[n].width == 64 && regXmmLo_[n].offset == low.offset &&
        regXmmHi_[n].width == 64)
      return regXmmHi_[n];
  return std::nullopt;
}

void dragon::x86::X86IGNILLifter::liftVec(const llvm::MCInst &mc, const VecOp &op,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  using V = VecKind;
  using Opc = ignil::Opcode;
  using ignil::Value;
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  struct Vec {
    Value lo, hi;
  };
  auto emit = [&](Opc o, std::vector<Value> srcs, ignil::Width w) -> Value {
    auto t = newTemp(tmp, w);
    out.append({o, t, std::move(srcs), addr});
    return t;
  };
  auto k = [](std::uint64_t v, ignil::Width w) {
    return Value{ignil::Const{w == 64 ? v : v & ((1ull << w) - 1), w}};
  };
  auto vecIndex = [&](unsigned i) -> std::optional<unsigned> {
    if (i >= mc.getNumOperands() || !mc.getOperand(i).isReg())
      return std::nullopt;
    auto it = vecRegIndex_.find(mc.getOperand(i).getReg());
    if (it == vecRegIndex_.end() || regXmmHi_[it->second].width == 0)
      return std::nullopt;
    return it->second;
  };
  auto fail = [&](const char *why) {
    logger_.log("Error liftVec in addr {:#x}: {}", addr, why);
    if (instrInfo_->get(mc.getOpcode()).getNumDefs() != 0) {
      if (auto n = vecIndex(0)) {
        emitUndef(regXmmLo_[*n], addr, out);
        emitUndef(regXmmHi_[*n], addr, out);
      } else if (mc.getOperand(0).isReg()) {
        if (auto r = toReg(mc.getOperand(0).getReg()))
          emitUndef(*r, addr, out);
      }
    }
    out.noteUnlifted(addr);
  };
  auto imm = [&](unsigned i) -> std::uint64_t {
    return static_cast<std::uint64_t>(mc.getOperand(i).getImm()) & 0xff;
  };
  auto readReg = [&](unsigned n) -> Vec {
    return {emit(Opc::COPY, {regXmmLo_[n]}, 64), emit(Opc::COPY, {regXmmHi_[n]}, 64)};
  };
  auto addrOf = [&](unsigned i) -> Value {
    return emitAddress(mc, i, ptrWidth, addr, out, tmp);
  };
  auto plus8 = [&](Value ea) {
    return emit(Opc::ADD, {ea, ignil::Const{8, ptrWidth}}, ptrWidth);
  };
  auto load128 = [&](Value ea) -> Vec {
    return {emit(Opc::LOAD, {ea}, 64), emit(Opc::LOAD, {plus8(ea)}, 64)};
  };
  // Register or 128-bit memory source at operand i.
  auto source = [&](unsigned i) -> std::optional<Vec> {
    if (op.memSrc)
      return load128(addrOf(i));
    auto n = vecIndex(i);
    if (!n)
      return std::nullopt;
    return readReg(*n);
  };
  auto write = [&](unsigned n, const Vec &v) {
    out.append({Opc::COPY, regXmmLo_[n], {v.lo}, addr});
    out.append({Opc::COPY, regXmmHi_[n], {v.hi}, addr});
  };
  // Lanes of e bits, lane 0 = bits e-1:0.
  auto lanes = [&](const Vec &v, ignil::Width e) {
    std::vector<Value> l;
    const unsigned per = 64 / e;
    for (const Value &half: {v.lo, v.hi})
      for (unsigned i = 0; i < per; ++i) {
        if (e == 64) {
          l.push_back(half);
          continue;
        }
        const Value sh = i ? emit(Opc::SHR, {half, k(i * e, 64)}, 64) : half;
        l.push_back(emit(Opc::TRUNC, {sh}, e));
      }
    return l;
  };
  auto pack = [&](const std::vector<Value> &l, ignil::Width e) -> Vec {
    const unsigned per = 64 / e;
    Value halves[2];
    for (unsigned h = 0; h < 2; ++h) {
      if (e == 64) {
        halves[h] = l[h];
        continue;
      }
      Value acc;
      for (unsigned i = 0; i < per; ++i) {
        const Value z = emit(Opc::ZEXT, {l[h * per + i]}, 64);
        const Value sh = i ? emit(Opc::SHL, {z, k(i * e, 64)}, 64) : z;
        acc = i ? emit(Opc::OR, {acc, sh}, 64) : sh;
      }
      halves[h] = acc;
    }
    return {halves[0], halves[1]};
  };
  auto mask = [&](Value bit, ignil::Width w) { // 1-bit -> all ones / zero
    return emit(Opc::NEG, {emit(Opc::ZEXT, {bit}, w)}, w);
  };
  auto notBit = [&](Value bit) { return emit(Opc::XOR, {bit, ignil::Const{1, 1}}, 1); };
  auto choose = [&](Value condition,Value yes,Value no,ignil::Width width) {
    const auto selected=mask(condition,width);
    return emit(Opc::OR,{emit(Opc::AND,{yes,selected},width),
                         emit(Opc::AND,{no,emit(Opc::NOT,{selected},width)},width)},width);
  };
  auto saturate = [&](Value value,ignil::Width width,ignil::Width result,bool signedResult) {
    const auto minimum = signedResult ? ~((1ull << (result-1))-1) : 0;
    const auto maximum = signedResult ? (1ull << (result-1))-1 : (1ull << result)-1;
    value=choose(emit(Opc::SLT,{value,k(minimum,width)},1),k(minimum,width),value,width);
    value=choose(emit(Opc::SGT,{value,k(maximum,width)},1),k(maximum,width),value,width);
    return emit(Opc::TRUNC,{value},result);
  };
  const ignil::Width e = op.laneWidth;

  switch (op.kind) {
    case V::FloatToInt: {
      const auto dst=toReg(mc.getOperand(0).getReg());
      if (!dst) return fail("no integer destination");
      Value value;
      if (op.memSrc) value=emit(Opc::LOAD,{addrOf(1)},e);
      else {
        const auto src=vecIndex(1);
        if (!src) return fail("no floating source");
        value=regXmmLo_[*src];
        if (e == 32) value=emit(Opc::TRUNC,{value},32);
      }
      const auto less=e == 32 ? Opc::FLT32 : Opc::FLT;
      const auto unordered=e == 32 ? Opc::FUNO32 : Opc::FUNO;
      const auto rounded=emit(e == 32 ? Opc::FTRUNC32 : Opc::FTRUNC,{value},e);
      const auto fraction=e == 32 ? 23u : 52u;
      const uint64_t bound=uint64_t((e == 32 ? 127 : 1023)+dst->width-1) << fraction;
      const auto below=emit(less,{rounded,k(bound | (1ull << (e-1)),e)},1);
      const auto above=notBit(emit(less,{rounded,k(bound,e)},1));
      const auto bad=emit(Opc::OR,{emit(unordered,{value,value},1),emit(Opc::OR,{below,above},1)},1);
      const auto safe=choose(bad,k(0,e),value,e);
      const auto integer=emit(e == 32 ? Opc::FPTOSI32 : Opc::FPTOSI,{safe},dst->width);
      out.append({Opc::COPY,*dst,{choose(bad,k(1ull << (dst->width-1),dst->width),integer,dst->width)},addr});
      return;
    }
    case V::IntToFloat: {
      const auto dst=vecIndex(0);
      if (!dst) return fail("no floating destination");
      const Vec previous=readReg(*dst);
      Value value;
      if (op.memSrc) value=emit(Opc::LOAD,{addrOf(2)},op.sourceWidth);
      else {
        const auto src=toReg(mc.getOperand(2).getReg());
        if (!src) return fail("no integer source");
        value=*src;
      }
      value=emit(e == 32 ? Opc::SITOFP32 : Opc::SITOFP,{value},e);
      if (e == 32) value=emit(Opc::OR,{emit(Opc::AND,{previous.lo,k(0xffffffff00000000ull,64)},64),emit(Opc::ZEXT,{value},64)},64);
      write(*dst,{value,previous.hi}); return;
    }
    case V::FloatWiden: case V::FloatNarrow: {
      const auto dst=vecIndex(0);
      if (!dst) return fail("no conversion destination");
      std::optional<Vec> src;
      if (op.kind == V::FloatWiden && op.memSrc)
        src=Vec{emit(Opc::LOAD,{addrOf(1)},64),k(0,64)};
      else src=source(1);
      if (!src) return fail("no conversion source");
      const auto inputs=lanes(*src,op.kind == V::FloatWiden ? 32 : 64);
      std::vector<Value> result;
      for (unsigned i=0;i<2;++i) result.push_back(emit(op.kind == V::FloatWiden ? Opc::FPEXT : Opc::FPTRUNC,{inputs[i]},e));
      if (op.kind == V::FloatNarrow) { result.push_back(k(0,32)); result.push_back(k(0,32)); }
      write(*dst,pack(result,e)); return;
    }
    case V::FloatCompareFlags: {
      const auto first=vecIndex(0);
      if (!first) return fail("no first compare source");
      Value left=regXmmLo_[*first],right;
      if (op.memSrc) right=emit(Opc::LOAD,{addrOf(1)},e);
      else {
        const auto second=vecIndex(1);
        if (!second) return fail("no second compare source");
        right=regXmmLo_[*second];
      }
      if (e == 32) { left=emit(Opc::TRUNC,{left},32); if (!op.memSrc) right=emit(Opc::TRUNC,{right},32); }
      const auto unordered=emit(e == 32 ? Opc::FUNO32 : Opc::FUNO,{left,right},1);
      const auto equal=emit(e == 32 ? Opc::FEQ32 : Opc::FEQ,{left,right},1);
      const auto less=emit(e == 32 ? Opc::FLT32 : Opc::FLT,{left,right},1);
      out.append({Opc::ZEXT,regZF_,{emit(Opc::OR,{unordered,equal},1)},addr});
      out.append({Opc::ZEXT,regCF_,{emit(Opc::OR,{unordered,less},1)},addr});
      out.append({Opc::ZEXT,regPF_,{unordered},addr});
      for (const auto flag : {regAF_,regSF_,regOF_}) out.append({Opc::COPY,flag,{k(0,8)},addr});
      return;
    }
    case V::FloatSqrt:
      if (!op.scalar) {
        const auto dst=vecIndex(0); const auto src=source(1);
        if (!dst || !src) return fail("bad packed sqrt operands");
        std::vector<Value> result;
        for (const auto value : lanes(*src,e)) result.push_back(emit(e == 32 ? Opc::FSQRT32 : Opc::FSQRT,{value},e));
        write(*dst,pack(result,e)); return;
      }
      break;
    case V::Extract: {
      auto index=vecIndex(op.memDst ? 5 : 1);
      if (!index) return fail("no vector source");
      const auto values=lanes(readReg(*index),e);
      Value value=values[imm(mc.getNumOperands()-1) & (128/e-1)];
      if (op.memDst) out.append({Opc::STORE,std::nullopt,{addrOf(0),value},addr});
      else {
        const auto dst=toReg(mc.getOperand(0).getReg());
        if (!dst) return fail("no GPR destination");
        if (dst->width != e) value=emit(Opc::ZEXT,{value},dst->width);
        out.append({Opc::COPY,*dst,{value},addr});
      }
      return;
    }
    case V::Abs:
    case V::ExtendSigned:
    case V::ExtendUnsigned: {
      const auto dst=vecIndex(0);
      if (!dst) return fail("no vector destination");
      const bool extend=op.kind != V::Abs;
      const ignil::Width input=extend ? op.sourceWidth : e;
      std::optional<Vec> src;
      if (extend && op.memSrc) {
        const auto width=static_cast<ignil::Width>(128/e*input);
        Value bits=emit(Opc::LOAD,{addrOf(1)},width);
        if (width < 64) bits=emit(Opc::ZEXT,{bits},64);
        src=Vec{bits,k(0,64)};
      } else src=source(1);
      if (!src) return fail("no vector source");
      const auto values=lanes(*src,input);
      std::vector<Value> result;
      for (unsigned i=0;i<128/e;++i)
        result.push_back(extend ? emit(op.kind == V::ExtendSigned ? Opc::SEXT : Opc::ZEXT,{values[i]},e)
          : choose(emit(Opc::SLT,{values[i],k(0,e)},1),emit(Opc::NEG,{values[i]},e),values[i],e));
      write(*dst,pack(result,e));
      return;
    }
    case V::Test: {
      const auto first=vecIndex(0); const auto second=source(1);
      if (!first || !second) return fail("bad PTEST inputs");
      const Vec a=readReg(*first), b=*second;
      const auto both=emit(Opc::OR,{emit(Opc::AND,{a.lo,b.lo},64),emit(Opc::AND,{a.hi,b.hi},64)},64);
      const auto missing=emit(Opc::OR,{emit(Opc::AND,{emit(Opc::NOT,{a.lo},64),b.lo},64),
                                     emit(Opc::AND,{emit(Opc::NOT,{a.hi},64),b.hi},64)},64);
      out.append({Opc::ZEXT,regZF_,{emit(Opc::EQ,{both,k(0,64)},1)},addr});
      out.append({Opc::ZEXT,regCF_,{emit(Opc::EQ,{missing,k(0,64)},1)},addr});
      for (const auto flag : {regPF_,regAF_,regSF_,regOF_}) out.append({Opc::COPY,flag,{k(0,8)},addr});
      return;
    }
    case V::Store:
    case V::MovHiStore:
    case V::MovLoStore:
    case V::MovssStore: { // op[0..4] = mem, op[5] = xmm
      auto n = vecIndex(5);
      if (!n)
        return fail("no source");
      const Value ea = addrOf(0);
      if (op.kind == V::MovssStore) {
        out.append({Opc::STORE, std::nullopt,
                    {ea, emit(Opc::TRUNC, {regXmmLo_[*n]}, 32)}, addr});
        return;
      }
      const Vec v = readReg(*n);
      if (op.kind != V::MovHiStore)
        out.append({Opc::STORE, std::nullopt, {ea, v.lo}, addr});
      if (op.kind == V::Store)
        out.append({Opc::STORE, std::nullopt, {plus8(ea), v.hi}, addr});
      if (op.kind == V::MovHiStore)
        out.append({Opc::STORE, std::nullopt, {ea, v.hi}, addr});
      return;
    }
    case V::Pmovmskb:
    case V::Pextrw: { // op[0] = GPR, op[1] = xmm (, op[2] = imm)
      auto dst = toReg(mc.getOperand(0).getReg());
      auto n = vecIndex(1);
      if (!dst || !n)
        return fail("bad operands");
      Value r;
      if (op.kind == V::Pextrw) {
        r = emit(Opc::ZEXT, {lanes(readReg(*n), 16)[imm(2) & 7]}, 32);
      } else {
        const auto l = lanes(readReg(*n), 8);
        for (unsigned j = 0; j < 16; ++j) {
          const Value bit = emit(Opc::ZEXT, {emit(Opc::SHR, {l[j], k(7, 8)}, 8)}, 32);
          const Value sh = j ? emit(Opc::SHL, {bit, k(j, 32)}, 32) : bit;
          r = j ? emit(Opc::OR, {r, sh}, 32) : sh;
        }
      }
      if (dst->width > 32)
        r = emit(Opc::ZEXT, {r}, dst->width);
      out.append({Opc::COPY, *dst, {r}, addr});
      return;
    }
    case V::MovssLoad: { // op[0] = xmm, op[1..5] = mem
      auto d = vecIndex(0);
      if (!d)
        return fail("no destination");
      const Value v = emit(Opc::LOAD, {addrOf(1)}, 32);
      write(*d, {emit(Opc::ZEXT, {v}, 64), k(0, 64)});
      return;
    }
    case V::Pshufd:
    case V::Pshuflw:
    case V::Pshufhw: { // op[0] = xmm, op[1] = xmm | op[1..5] = mem, then imm
      auto d = vecIndex(0);
      auto src = source(1);
      if (!d || !src)
        return fail("bad operands");
      const std::uint64_t sel = imm(mc.getNumOperands() - 1);
      std::vector<Value> r;
      if (op.kind == V::Pshufd) {
        const auto l = lanes(*src, 32);
        for (unsigned i = 0; i < 4; ++i)
          r.push_back(l[(sel >> (2 * i)) & 3]);
        write(*d, pack(r, 32));
      } else {
        const auto l = lanes(*src, 16);
        const unsigned base = op.kind == V::Pshuflw ? 0 : 4;
        r = l;
        for (unsigned i = 0; i < 4; ++i)
          r[base + i] = l[base + ((sel >> (2 * i)) & 3)];
        write(*d, pack(r, 16));
      }
      return;
    }
    default:
      break;
  }

  // Everything below has a tied destination: op[0] = op[1] = xmm.
  auto d = vecIndex(0);
  if (!d)
    return fail("no destination");
  const Vec a = readReg(*d);

  switch (op.kind) {
    case V::FloatAdd: case V::FloatSub: case V::FloatMul: case V::FloatDiv:
    case V::FloatMin: case V::FloatMax: case V::FloatSqrt: case V::FloatCompareMask: {
      std::optional<Vec> b;
      const unsigned sourceIndex=op.kind == V::FloatSqrt
                                  ? mc.getNumOperands()-(op.memSrc ? 5 : 1) : 2;
      if (op.scalar && op.memSrc) {
        Value bits=emit(Opc::LOAD,{addrOf(sourceIndex)},e);
        if (e == 32) bits=emit(Opc::ZEXT,{bits},64);
        b=Vec{bits,k(0,64)};
      } else b=source(sourceIndex);
      if (!b) return fail("no floating source");
      const auto left=lanes(a,e),right=lanes(*b,e);
      auto result=left;
      const auto predicate=op.kind == V::FloatCompareMask ? imm(mc.getNumOperands()-1) : 0;
      if (predicate > 7) return fail("unsupported floating predicate");
      for (unsigned i=0;i<(op.scalar ? 1 : 128/e);++i) {
        const auto x=left[i],y=right[i];
        Opc code;
        switch (op.kind) {
          case V::FloatAdd: code=e == 32 ? Opc::FADD32 : Opc::FADD; break;
          case V::FloatSub: code=e == 32 ? Opc::FSUB32 : Opc::FSUB; break;
          case V::FloatMul: code=e == 32 ? Opc::FMUL32 : Opc::FMUL; break;
          case V::FloatDiv: code=e == 32 ? Opc::FDIV32 : Opc::FDIV; break;
          case V::FloatSqrt: result[i]=emit(e == 32 ? Opc::FSQRT32 : Opc::FSQRT,{y},e); continue;
          case V::FloatMin: case V::FloatMax: {
            const auto comparison=e == 32 ? Opc::FLT32 : Opc::FLT;
            // Ordered comparisons are false for NaN and equal signed zeros:
            // all those cases must return the second operand's raw bits.
            const auto takeFirst=emit(comparison,op.kind == V::FloatMin ? std::vector<Value>{x,y} : std::vector<Value>{y,x},1);
            result[i]=choose(takeFirst,x,y,e); continue;
          }
          case V::FloatCompareMask: {
            const auto un=emit(e == 32 ? Opc::FUNO32 : Opc::FUNO,{x,y},1);
            const auto lt=emit(e == 32 ? Opc::FLT32 : Opc::FLT,{x,y},1);
            const auto eq=emit(e == 32 ? Opc::FEQ32 : Opc::FEQ,{x,y},1);
            const auto le=emit(Opc::OR,{lt,eq},1);
            const Value conditions[]={eq,lt,le,un,notBit(eq),notBit(lt),notBit(le),notBit(un)};
            result[i]=mask(conditions[predicate],e); continue;
          }
          default: return fail("unexpected floating operation");
        }
        result[i]=emit(code,{x,y},e);
      }
      write(*d,pack(result,e)); return;
    }
    case V::Insert: {
      Value value;
      if (op.memSrc) value=emit(Opc::LOAD,{addrOf(2)},e);
      else {
        const auto src=toReg(mc.getOperand(2).getReg());
        if (!src) return fail("no insertion source");
        value=src->width == e ? Value{*src} : emit(Opc::TRUNC,{*src},e);
      }
      auto result=lanes(a,e);
      result[imm(mc.getNumOperands()-1) & (128/e-1)]=value;
      write(*d,pack(result,e)); return;
    }
    case V::PackUnsigned: case V::PackSigned:
    case V::AddSatUnsigned: case V::SubSatUnsigned: case V::AddSatSigned: case V::SubSatSigned:
    case V::MulUnsignedDword: case V::MulLow: case V::MulHighSigned: case V::MulHighUnsigned: case V::MaddSigned:
    case V::Average: case V::MinSigned: case V::MinUnsigned: case V::MaxSigned: case V::MaxUnsigned:
    case V::Sad: case V::ShuffleBytes: case V::AlignBytes: case V::Sign: case V::Blend: {
      const auto b=source(2);
      if (!b) return fail("no vector source");
      const auto la=lanes(a,e), lb=lanes(*b,e);
      std::vector<Value> result;
      ignil::Width resultWidth=e;
      if (op.kind == V::PackUnsigned || op.kind == V::PackSigned) {
        resultWidth=e/2;
        for (const auto &values : {la,lb})
          for (auto value : values) result.push_back(saturate(value,e,resultWidth,op.kind == V::PackSigned));
      } else if (op.kind == V::MulUnsignedDword) {
        resultWidth=64;
        for (unsigned i : {0u,2u}) result.push_back(emit(Opc::MUL,{emit(Opc::ZEXT,{la[i]},64),emit(Opc::ZEXT,{lb[i]},64)},64));
      } else if (op.kind == V::MaddSigned) {
        resultWidth=32;
        for (unsigned i=0;i<la.size();i+=2) {
          Value products[2];
          for (unsigned j=0;j<2;++j) products[j]=emit(Opc::MUL,{emit(Opc::SEXT,{la[i+j]},32),emit(Opc::SEXT,{lb[i+j]},32)},32);
          result.push_back(emit(Opc::ADD,{products[0],products[1]},32));
        }
      } else if (op.kind == V::Sad) {
        resultWidth=64;
        for (unsigned h=0;h<2;++h) {
          Value sum=k(0,64);
          for (unsigned i=h*8;i<(h+1)*8;++i) {
            const auto below=emit(Opc::ULT,{la[i],lb[i]},1);
            const auto distance=choose(below,emit(Opc::SUB,{lb[i],la[i]},8),emit(Opc::SUB,{la[i],lb[i]},8),8);
            sum=emit(Opc::ADD,{sum,emit(Opc::ZEXT,{distance},64)},64);
          }
          result.push_back(sum);
        }
      } else if (op.kind == V::AlignBytes) {
        const auto offset=imm(mc.getNumOperands()-1);
        for (unsigned i=0;i<16;++i) {
          const auto index=offset+i;
          result.push_back(index < 16 ? lb[index] : index < 32 ? la[index-16] : k(0,8));
        }
      } else for (unsigned i=0;i<la.size();++i) {
        const auto x=la[i], y=lb[i];
        switch (op.kind) {
          case V::AddSatUnsigned: case V::SubSatUnsigned: case V::AddSatSigned: case V::SubSatSigned: {
            const bool sign=op.kind == V::AddSatSigned || op.kind == V::SubSatSigned;
            const bool add=op.kind == V::AddSatUnsigned || op.kind == V::AddSatSigned;
            const auto extension=sign ? Opc::SEXT : Opc::ZEXT;
            const auto wide=static_cast<ignil::Width>(e*2);
            const auto value=emit(add ? Opc::ADD : Opc::SUB,{emit(extension,{x},wide),emit(extension,{y},wide)},wide);
            result.push_back(saturate(value,wide,e,sign)); break;
          }
          case V::MulLow: result.push_back(emit(Opc::MUL,{x,y},e)); break;
          case V::MulHighSigned: case V::MulHighUnsigned: {
            const auto extension=op.kind == V::MulHighSigned ? Opc::SEXT : Opc::ZEXT;
            const auto wide=static_cast<ignil::Width>(e*2);
            const auto value=emit(Opc::MUL,{emit(extension,{x},wide),emit(extension,{y},wide)},wide);
            result.push_back(emit(Opc::TRUNC,{emit(Opc::SHR,{value,k(e,wide)},wide)},e)); break;
          }
          case V::Average: {
            const auto wide=static_cast<ignil::Width>(e*2);
            const auto sum=emit(Opc::ADD,{emit(Opc::ZEXT,{x},wide),emit(Opc::ZEXT,{y},wide)},wide);
            result.push_back(emit(Opc::TRUNC,{emit(Opc::SHR,{emit(Opc::ADD,{sum,k(1,wide)},wide),k(1,wide)},wide)},e)); break;
          }
          case V::MinSigned: case V::MinUnsigned: case V::MaxSigned: case V::MaxUnsigned: {
            const bool sign=op.kind == V::MinSigned || op.kind == V::MaxSigned;
            const bool minimum=op.kind == V::MinSigned || op.kind == V::MinUnsigned;
            result.push_back(choose(emit(sign ? Opc::SLT : Opc::ULT,{x,y},1),minimum ? x : y,minimum ? y : x,e)); break;
          }
          case V::Sign:
            result.push_back(choose(emit(Opc::EQ,{y,k(0,e)},1),k(0,e),
                             choose(emit(Opc::SLT,{y,k(0,e)},1),emit(Opc::NEG,{x},e),x,e),e)); break;
          case V::Blend: result.push_back((imm(mc.getNumOperands()-1) >> i) & 1 ? y : x); break;
          case V::ShuffleBytes: {
            const auto index=emit(Opc::AND,{y,k(15,8)},8);
            Value selected=k(0,8);
            for (unsigned j=0;j<16;++j) selected=choose(emit(Opc::EQ,{index,k(j,8)},1),la[j],selected,8);
            result.push_back(choose(emit(Opc::EQ,{emit(Opc::AND,{y,k(128,8)},8),k(0,8)},1),selected,k(0,8),8)); break;
          }
          default: return fail("unexpected integer operation");
        }
      }
      write(*d,pack(result,resultWidth)); return;
    }
    case V::Add:
    case V::Sub:
    case V::CmpEq:
    case V::CmpGt:
    case V::UnpackLo:
    case V::UnpackHi:
    case V::Shufps:
    case V::Shufpd: {
      auto b = source(2);
      if (!b)
        return fail("no source");
      if (op.kind == V::Shufpd) {
        const std::uint64_t sel = imm(mc.getNumOperands() - 1);
        write(*d, {(sel & 1) ? a.hi : a.lo, (sel & 2) ? b->hi : b->lo});
        return;
      }
      if (op.kind == V::Shufps) {
        const std::uint64_t sel = imm(mc.getNumOperands() - 1);
        const auto la = lanes(a, 32), lb = lanes(*b, 32);
        write(*d, pack({la[sel & 3], la[(sel >> 2) & 3], lb[(sel >> 4) & 3],
                        lb[(sel >> 6) & 3]},
                       32));
        return;
      }
      const auto la = lanes(a, e), lb = lanes(*b, e);
      const unsigned n = static_cast<unsigned>(la.size());
      std::vector<Value> r;
      if (op.kind == V::UnpackLo || op.kind == V::UnpackHi) {
        const unsigned base = op.kind == V::UnpackLo ? 0 : n / 2;
        for (unsigned i = 0; i < n / 2; ++i) {
          r.push_back(la[base + i]);
          r.push_back(lb[base + i]);
        }
      } else {
        for (unsigned i = 0; i < n; ++i) {
          if (op.kind == V::Add)
            r.push_back(emit(Opc::ADD, {la[i], lb[i]}, e));
          else if (op.kind == V::Sub)
            r.push_back(emit(Opc::SUB, {la[i], lb[i]}, e));
          else
            r.push_back(mask(emit(op.kind == V::CmpEq ? Opc::EQ : Opc::SGT,
                                  {la[i], lb[i]}, 1),
                             e));
        }
      }
      write(*d, pack(r, e));
      return;
    }
    case V::ShiftImm:
    case V::ShiftReg: {
      const Opc shiftOp = op.shift == VecShift::Left    ? Opc::SHL
                          : op.shift == VecShift::Right ? Opc::SHR
                                                        : Opc::SAR;
      const auto l = lanes(a, e);
      std::vector<Value> r;
      if (op.kind == V::ShiftImm) {
        const std::uint64_t c = imm(2);
        for (const Value &x: l) {
          if (op.shift != VecShift::Arith && c >= e)
            r.push_back(k(0, e));
          else
            r.push_back(emit(shiftOp, {x, k(std::min<std::uint64_t>(c, e - 1), e)}, e));
        }
      } else {
        // The count is bits 63:0 of the source; >= e empties a logical
        // lane and fills an arithmetic one with its sign.
        Value count;
        if (op.memSrc) {
          count = emit(Opc::LOAD, {addrOf(2)}, 64);
        } else {
          auto s = vecIndex(2);
          if (!s)
            return fail("no count");
          count = emit(Opc::COPY, {regXmmLo_[*s]}, 64);
        }
        const Value big = mask(emit(Opc::UGE, {count, k(e, 64)}, 1), 64);
        const Value cap = k(op.shift == VecShift::Arith ? e - 1 : 0, 64);
        Value amount = emit(Opc::OR, {emit(Opc::AND, {count, emit(Opc::NOT, {big}, 64)}, 64),
                                      emit(Opc::AND, {cap, big}, 64)},
                            64);
        Value keep = emit(Opc::NOT, {big}, 64);
        if (e < 64) {
          amount = emit(Opc::TRUNC, {amount}, e);
          keep = emit(Opc::TRUNC, {keep}, e);
        }
        for (const Value &x: l) {
          Value v = emit(shiftOp, {x, amount}, e);
          if (op.shift != VecShift::Arith)
            v = emit(Opc::AND, {v, keep}, e);
          r.push_back(v);
        }
      }
      write(*d, pack(r, e));
      return;
    }
    case V::ByteShiftLeft:
    case V::ByteShiftRight: {
      const std::uint64_t c = imm(2);
      if (c > 15) {
        write(*d, {k(0, 64), k(0, 64)});
        return;
      }
      const std::uint64_t sh = 8 * c;
      if (sh == 0) {
        write(*d, a);
        return;
      }
      const bool left = op.kind == V::ByteShiftLeft;
      const Opc fwd = left ? Opc::SHL : Opc::SHR, back = left ? Opc::SHR : Opc::SHL;
      // "near" is the half the bits move away from, "far" the one they enter.
      const Value nearH = left ? a.lo : a.hi, farH = left ? a.hi : a.lo;
      Value newNear, newFar;
      if (sh < 64) {
        newNear = emit(fwd, {nearH, k(sh, 64)}, 64);
        newFar = emit(Opc::OR, {emit(fwd, {farH, k(sh, 64)}, 64),
                                emit(back, {nearH, k(64 - sh, 64)}, 64)},
                      64);
      } else {
        newNear = k(0, 64);
        newFar = sh == 64 ? nearH : emit(fwd, {nearH, k(sh - 64, 64)}, 64);
      }
      write(*d, left ? Vec{newNear, newFar} : Vec{newFar, newNear});
      return;
    }
    case V::MovHiLoad:
    case V::MovLoLoad: {
      const Value v = emit(Opc::LOAD, {addrOf(2)}, 64);
      write(*d, op.kind == V::MovHiLoad ? Vec{a.lo, v} : Vec{v, a.hi});
      return;
    }
    case V::Movhlps:
    case V::Movlhps:
    case V::MovssReg: {
      auto s = vecIndex(2);
      if (!s)
        return fail("no source");
      const Vec b = readReg(*s);
      if (op.kind == V::Movhlps)
        write(*d, {b.hi, a.hi});
      else if (op.kind == V::Movlhps)
        write(*d, {a.lo, b.lo});
      else
        write(*d, {emit(Opc::OR, {emit(Opc::AND, {a.lo, k(0xffffffff00000000ull, 64)}, 64),
                                  emit(Opc::AND, {b.lo, k(0xffffffffull, 64)}, 64)},
                        64),
                   a.hi});
      return;
    }
    case V::Pinsrw: { // op[2] = r32, op[3] = imm
      auto g = toReg(mc.getOperand(2).getReg());
      if (!g)
        return fail("no source");
      auto l = lanes(a, 16);
      l[imm(3) & 7] = emit(Opc::TRUNC, {*g}, 16);
      write(*d, pack(l, 16));
      return;
    }
    case V::Cmpsd: {
      Value b;
      if (op.memSrc) {
        b = emit(Opc::LOAD, {addrOf(2)}, 64);
      } else {
        auto s = vecIndex(2);
        if (!s)
          return fail("no source");
        b = emit(Opc::COPY, {regXmmLo_[*s]}, 64);
      }
      const std::uint64_t pred = imm(mc.getNumOperands() - 1);
      if (pred > 7)
        return fail("AVX predicate");
      const Value un = emit(Opc::FUNO, {a.lo, b}, 1);
      const Value lt = emit(Opc::FLT, {a.lo, b}, 1);
      const Value eq = emit(Opc::FEQ, {a.lo, b}, 1);
      const Value le = emit(Opc::OR, {lt, eq}, 1);
      const Value bits[8] = {eq, lt, le, un, notBit(eq), notBit(lt), notBit(le), notBit(un)};
      write(*d, {mask(bits[pred & 7], 64), a.hi});
      return;
    }
    default:
      return fail("unhandled kind");
  }
}

std::optional<llvm::MCInst> dragon::x86::X86IGNILLifter::rewriteThroughScratch(
        const llvm::MCInst &mc, const MemViaReg &form, std::uint64_t addr,
        ignil::Block &out, std::uint32_t &tmp, ScratchState &state) const {
  constexpr unsigned kMem = 5; // base, scale, index, displacement, segment
  const auto &memDesc = instrInfo_->get(mc.getOpcode());
  const auto &regDesc = instrInfo_->get(form.regOpcode);
  unsigned k = memDesc.getNumOperands();
  for (unsigned i = 0; i < memDesc.getNumOperands(); ++i)
    if (memDesc.operands()[i].OperandType == llvm::MCOI::OPERAND_MEMORY) {
      k = i;
      break;
    }
  if (k + kMem > mc.getNumOperands())
    return std::nullopt;

  // A scratch register that overlaps nothing the instruction names, reads
  // or writes (operands, address registers, implicit RAX/RDX/CL).
  std::vector<ignil::Reg> used;
  for (unsigned i = 0; i < mc.getNumOperands(); ++i)
    if (mc.getOperand(i).isReg())
      if (auto r = toReg(mc.getOperand(i).getReg()))
        used.push_back(*r);
  for (const auto *desc: {&memDesc, &regDesc}) {
    for (const llvm::MCPhysReg r: desc->implicit_uses())
      if (auto ir = toReg(r))
        used.push_back(*ir);
    for (const llvm::MCPhysReg r: desc->implicit_defs())
      if (auto ir = toReg(r))
        used.push_back(*ir);
  }
  auto overlaps = [](const ignil::Reg &a, const ignil::Reg &b) {
    return a.offset < b.offset + b.width / 8 && b.offset < a.offset + a.width / 8;
  };
  const unsigned wIdx = form.width == 8 ? 0 : form.width == 16 ? 1 : form.width == 32 ? 2 : 3;
  const ScratchReg *pick = nullptr;
  for (const ScratchReg &c: scratchRegs_) {
    if (c.byWidth[wIdx] == 0 || !toReg(c.byWidth[wIdx]))
      continue;
    const ignil::Reg full = *toReg(c.full);
    if (std::none_of(used.begin(), used.end(),
                     [&](const ignil::Reg &u) { return overlaps(full, u); })) {
      pick = &c;
      break;
    }
  }
  if (!pick)
    return std::nullopt;
  const unsigned viewLlvm = pick->byWidth[wIdx];

  // The register form's operands: the memory group becomes the scratch view;
  // a read-modify-write register form has one more operand, its tied
  // destination, in front.
  llvm::MCInst rewritten;
  rewritten.setOpcode(form.regOpcode);
  if (form.xadd) {
    // XADD*rm: op[0]=SRC (def), op[1]=SRC (tied), op[2..6]=mem = DEST.
    const llvm::MCOperand src = mc.getOperand(0);
    rewritten.addOperand(llvm::MCOperand::createReg(viewLlvm));
    rewritten.addOperand(src);
    rewritten.addOperand(llvm::MCOperand::createReg(viewLlvm));
    rewritten.addOperand(src);
  } else {
    std::vector<llvm::MCOperand> ops;
    for (unsigned i = 0; i < k; ++i)
      ops.push_back(mc.getOperand(i));
    ops.push_back(llvm::MCOperand::createReg(viewLlvm));
    for (unsigned i = k + kMem; i < mc.getNumOperands(); ++i)
      ops.push_back(mc.getOperand(i));
    if (regDesc.getNumOperands() == ops.size() + 1)
      ops.insert(ops.begin(), llvm::MCOperand::createReg(viewLlvm));
    if (regDesc.getNumOperands() != ops.size())
      return std::nullopt;
    for (const auto &op: ops)
      rewritten.addOperand(op);
  }
  if (rewritten.getNumOperands() != regDesc.getNumOperands())
    return std::nullopt;

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Temp ea = emitAddress(mc, k, ptrWidth, addr, out, tmp);
  if (form.bitString) {
    // bt [m], reg: the signed bit offset selects the word at
    // m + (offset >> log2(w)) * w/8; the register form then takes the
    // offset modulo w, which is the bit inside that word.
    auto offReg = toReg(mc.getOperand(k + kMem).getReg());
    if (!offReg)
      return std::nullopt; // nothing emitted that matters: ea is a temp
    ignil::Value off = *offReg;
    if (offReg->width < ptrWidth) {
      auto t = newTemp(tmp, ptrWidth);
      out.append({ignil::Opcode::SEXT, t, {off}, addr});
      off = t;
    }
    unsigned bitShift = 0, byteShift = 0;
    for (unsigned b = form.width; b > 1; b >>= 1)
      ++bitShift;
    for (unsigned b = form.width / 8; b > 1; b >>= 1)
      ++byteShift;
    auto words = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::SAR, words, {off, ignil::Const{bitShift, ptrWidth}}, addr});
    auto delta = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::SHL, delta,
                {ignil::Value{words}, ignil::Const{byteShift, ptrWidth}}, addr});
    auto moved = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::ADD, moved, {ignil::Value{ea}, ignil::Value{delta}}, addr});
    ea = moved;
  }

  const ignil::Reg full = *toReg(pick->full);
  const ignil::Reg view = *toReg(viewLlvm);
  auto saved = newTemp(tmp, full.width);
  out.append({ignil::Opcode::COPY, saved, {full}, addr});
  auto loaded = newTemp(tmp, form.width);
  out.append({ignil::Opcode::LOAD, loaded, {ignil::Value{ea}}, addr});
  out.append({ignil::Opcode::COPY, view, {ignil::Value{loaded}}, addr});

  state = ScratchState{ea, saved, full, view, memDesc.mayStore()};
  return rewritten;
}


std::optional<dragon::x86::X86IGNILLifter::MemOperandOp>
dragon::x86::X86IGNILLifter::classifyMemOperandOp(std::string_view name) {
  using K = AluKind;
  using F = MemOperandForm;
  auto widthOf = [](std::string_view digits) -> ignil::Width {
    if (digits == "8") return 8;
    if (digits == "16") return 16;
    if (digits == "32") return 32;
    if (digits == "64") return 64;
    return 0;
  };
  // <OP><width><form>: ADD32mr, SUB16rm, XOR8mi, AND64mi8, CMP64mi32, ...
  static const std::pair<std::string_view, AluKind> kAlu[] = {
          {"ADD", K::Add}, {"SUB", K::Sub}, {"ADC", K::Adc}, {"SBB", K::Sbb},
          {"AND", K::And}, {"XOR", K::Xor}, {"CMP", K::Cmp}, {"TEST", K::Test},
          {"OR", K::Or}, {"NEG", K::Neg}, {"NOT", K::Not}};
  for (const auto &[prefix, kind]: kAlu) {
    if (!name.starts_with(prefix))
      continue;
    std::string_view rest = name.substr(prefix.size());
    std::size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
      ++digits;
    const ignil::Width w = widthOf(rest.substr(0, digits));
    if (!w)
      return std::nullopt;
    const std::string_view form = rest.substr(digits);
    if (kind == K::Neg || kind == K::Not)
      return form == "m" ? std::optional{MemOperandOp{kind, F::Mem, w}}
                         : std::nullopt;
    if (form == "mr")
      return MemOperandOp{kind, F::MemReg, w};
    if (form == "rm" && kind != K::Test)
      return MemOperandOp{kind, F::RegMem, w};
    if (form == "mi" || form == "mi8" || form == "mi32")
      return MemOperandOp{kind, F::MemImm, w};
    return std::nullopt;
  }
  if (name == "CMOV16rm") return MemOperandOp{K::Cmov, F::RegMem, 16};
  if (name == "CMOV32rm") return MemOperandOp{K::Cmov, F::RegMem, 32};
  if (name == "CMOV64rm") return MemOperandOp{K::Cmov, F::RegMem, 64};
  if (name == "SETCCm") return MemOperandOp{K::Setcc, F::Mem, 8};
  if (name == "MOV16mi") return MemOperandOp{K::MovStoreImm, F::MemImm, 16};
  if (name == "PUSH64rmm") return MemOperandOp{K::PushMem, F::Mem, 64};
  if (name == "PUSH32rmm") return MemOperandOp{K::PushMem, F::Mem, 32};
  if (name == "POP64rmm") return MemOperandOp{K::PopMem, F::Mem, 64};
  if (name == "POP32rmm") return MemOperandOp{K::PopMem, F::Mem, 32};
  // mov al/ax/eax/rax, [moffs] (MOV<w>ao<asize>) and the store
  // MOV<w>o<asize>a.
  if (name.starts_with("MOV")) {
    std::string_view rest = name.substr(3);
    std::size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
      ++digits;
    const ignil::Width w = widthOf(rest.substr(0, digits));
    const std::string_view tail = rest.substr(digits);
    if (w && tail.size() > 2 && tail.starts_with("ao"))
      return MemOperandOp{K::MovLoadOffset, F::Offset, w};
    if (w && tail.size() > 2 && tail.starts_with("o") && tail.ends_with("a"))
      return MemOperandOp{K::MovStoreOffset, F::Offset, w};
  }
  return std::nullopt;
}

void dragon::x86::X86IGNILLifter::liftMemOperandForm(const llvm::MCInst &mc,
                                                     const MemOperandOp &op,
                                                     std::uint64_t addr,
                                                     ignil::Block &out,
                                                     std::uint32_t &tmp) const {
  using K = AluKind;
  using F = MemOperandForm;
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  const ignil::Width w = op.width;
  auto fail = [&](const char *why) {
    logger_.log("Error liftMemOperandForm in addr {:#x}: {}", addr, why);
    out.noteUnlifted(addr);
  };

  // -- moffs: the address is an absolute offset, the register the
  // accumulator of the operand's width.
  if (op.form == F::Offset) {
    static const ignil::Reg *const kAcc[] = {&dragon::x86::AL, &dragon::x86::AX,
                                             &dragon::x86::EAX,
                                             &dragon::x86::RAX};
    const ignil::Reg &acc = *kAcc[w == 8 ? 0 : w == 16 ? 1 : w == 32 ? 2 : 3];
    const auto offset = static_cast<std::uint64_t>(mc.getOperand(0).getImm());
    auto a = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::COPY, a, {ignil::Const{offset, ptrWidth}},
                addr});
    if (op.kind == K::MovLoadOffset) {
      auto v = newTemp(tmp, w);
      out.append({ignil::Opcode::LOAD, v, {a}, addr});
      out.append({ignil::Opcode::COPY, acc, {v}, addr});
    } else {
      out.append({ignil::Opcode::STORE, std::nullopt, {a, acc}, addr});
    }
    return;
  }

  // -- PUSH/POP [mem]
  if (op.kind == K::PushMem || op.kind == K::PopMem) {
    const ignil::Reg &sp = ptrWidth == 64 ? dragon::x86::RSP : dragon::x86::ESP;
    const ignil::Const slot{std::uint64_t{w} / 8, ptrWidth};
    if (op.kind == K::PushMem) {
      auto a = emitAddress(mc, 0, ptrWidth, addr, out, tmp); // before RSP moves
      auto v = newTemp(tmp, w);
      out.append({ignil::Opcode::LOAD, v, {a}, addr});
      auto nsp = newTemp(tmp, ptrWidth);
      out.append({ignil::Opcode::SUB, nsp, {sp, slot}, addr});
      out.append({ignil::Opcode::STORE, std::nullopt, {nsp, v}, addr});
      out.append({ignil::Opcode::COPY, sp, {nsp}, addr});
    } else {
      auto v = newTemp(tmp, w);
      out.append({ignil::Opcode::LOAD, v, {sp}, addr});
      auto nsp = newTemp(tmp, ptrWidth);
      out.append({ignil::Opcode::ADD, nsp, {sp, slot}, addr});
      out.append({ignil::Opcode::COPY, sp, {nsp}, addr});
      // An RSP-based destination is addressed with the incremented RSP.
      auto a = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
      out.append({ignil::Opcode::STORE, std::nullopt, {a, v}, addr});
    }
    return;
  }

  // -- operands. Memory starts at 0 for mem-first forms; for reg-first ones
  // after the destination (and its tied source), or after the single
  // register of CMP.
  const bool memFirst = op.form != F::RegMem;
  const unsigned memStart =
          memFirst ? 0 : (op.kind == K::Cmp ? 1u : 2u);
  auto ea = emitAddress(mc, memStart, ptrWidth, addr, out, tmp);
  auto memVal = newTemp(tmp, w);
  if (op.kind != K::MovStoreImm && op.kind != K::Setcc)
    out.append({ignil::Opcode::LOAD, memVal, {ea}, addr});

  if (op.kind == K::MovStoreImm) {
    const auto imm = static_cast<std::uint64_t>(mc.getOperand(5).getImm());
    out.append({ignil::Opcode::STORE, std::nullopt, {ea, ignil::Const{imm, w}},
                addr});
    return;
  }
  if (op.kind == K::Setcc) {
    const auto cc = static_cast<unsigned>(mc.getOperand(5).getImm());
    auto cond = emitCondition(cc, addr, out, tmp);
    auto v = newTemp(tmp, 8);
    out.append({ignil::Opcode::ZEXT, v, {cond}, addr});
    out.append({ignil::Opcode::STORE, std::nullopt, {ea, v}, addr});
    return;
  }

  std::optional<ignil::Reg> reg; // the register operand, if any
  if (op.form == F::MemReg) {
    reg = toReg(mc.getOperand(5).getReg());
    if (!reg)
      return fail("no register operand");
  } else if (op.form == F::RegMem) {
    reg = toReg(mc.getOperand(0).getReg());
    if (!reg)
      return fail("no register operand");
  }

  if (op.kind == K::Cmov) {
    // dst = cond ? [mem] : dst. The load happens either way (as on
    // hardware, a faulting address faults even when the move is not taken).
    const auto cc = static_cast<unsigned>(mc.getOperand(7).getImm());
    auto cond = emitCondition(cc, addr, out, tmp);
    auto kept = newTemp(tmp, w);
    out.append({ignil::Opcode::COPY, kept, {*reg}, addr});
    auto condW = newTemp(tmp, w);
    out.append({ignil::Opcode::ZEXT, condW, {ignil::Value{cond}}, addr});
    auto mask = newTemp(tmp, w);
    out.append({ignil::Opcode::NEG, mask, {ignil::Value{condW}}, addr});
    auto keepMask = newTemp(tmp, w);
    out.append({ignil::Opcode::NOT, keepMask, {ignil::Value{mask}}, addr});
    auto a = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, a, {memVal, mask}, addr});
    auto b = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, b, {kept, keepMask}, addr});
    auto r = newTemp(tmp, w);
    out.append({ignil::Opcode::OR, r, {a, b}, addr});
    out.append({ignil::Opcode::COPY, *reg, {r}, addr}); // 32-bit zero-extends
    return;
  }

  // lhs is the destination operand, rhs the source.
  ignil::Temp lhs = newTemp(tmp, w);
  ignil::Value rhs = ignil::Const{0, w};
  switch (op.form) {
    case F::MemReg:
      out.append({ignil::Opcode::COPY, lhs, {memVal}, addr});
      rhs = *reg;
      break;
    case F::RegMem:
      out.append({ignil::Opcode::COPY, lhs, {*reg}, addr});
      rhs = memVal;
      break;
    case F::MemImm:
      out.append({ignil::Opcode::COPY, lhs, {memVal}, addr});
      rhs = ignil::Const{static_cast<std::uint64_t>(mc.getOperand(5).getImm()),
                         w};
      break;
    default: // Mem: NEG/NOT
      out.append({ignil::Opcode::COPY, lhs, {memVal}, addr});
      break;
  }
  // A copy of rhs, so a register both read and written stays the input.
  auto rhsT = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, rhsT, {rhs}, addr});

  auto result = newTemp(tmp, w);
  auto writeBack = [&](ignil::Temp v) {
    if (op.form == F::RegMem)
      out.append({ignil::Opcode::COPY, *reg, {v}, addr});
    else
      out.append({ignil::Opcode::STORE, std::nullopt, {ea, v}, addr});
  };
  auto logicFlags = [&](ignil::Temp r) {
    emitArithFlags(addr, out, tmp,
                   {
                           {X86Flag::CF, cfAlwaysZero(addr)},
                           {X86Flag::PF, pfParity(r, addr)},
                           {X86Flag::AF, afAlwaysZero(addr)},
                           {X86Flag::ZF, zfEqZero(r, addr)},
                           {X86Flag::SF, sfSignBit(r, addr)},
                           {X86Flag::OF, ofAlwaysZero(addr)},
                   });
  };

  switch (op.kind) {
    case K::Add:
      out.append({ignil::Opcode::ADD, result, {lhs, rhsT}, addr});
      writeBack(result);
      emitArithFlags(addr, out, tmp,
                     {
                             {X86Flag::CF, cfAddOverflow(result, rhsT, addr)},
                             {X86Flag::PF, pfParity(result, addr)},
                             {X86Flag::AF, afNibbleCarry(lhs, rhsT, result, addr)},
                             {X86Flag::ZF, zfEqZero(result, addr)},
                             {X86Flag::SF, sfSignBit(result, addr)},
                             {X86Flag::OF, ofAddOverflow(lhs, rhsT, result, addr)},
                     });
      return;
    case K::Sub:
    case K::Cmp:
      out.append({ignil::Opcode::SUB, result, {lhs, rhsT}, addr});
      if (op.kind == K::Sub)
        writeBack(result);
      emitArithFlags(addr, out, tmp,
                     {
                             {X86Flag::CF, cfSubBorrow(lhs, rhsT, addr)},
                             {X86Flag::PF, pfParity(result, addr)},
                             {X86Flag::AF, afNibbleCarry(lhs, rhsT, result, addr)},
                             {X86Flag::ZF, zfEqZero(result, addr)},
                             {X86Flag::SF, sfSignBit(result, addr)},
                             {X86Flag::OF, ofSubOverflow(lhs, rhsT, result, addr)},
                     });
      return;
    case K::And:
    case K::Test:
      out.append({ignil::Opcode::AND, result, {lhs, rhsT}, addr});
      if (op.kind == K::And)
        writeBack(result);
      logicFlags(result);
      return;
    case K::Or:
      out.append({ignil::Opcode::OR, result, {lhs, rhsT}, addr});
      writeBack(result);
      logicFlags(result);
      return;
    case K::Xor:
      out.append({ignil::Opcode::XOR, result, {lhs, rhsT}, addr});
      writeBack(result);
      logicFlags(result);
      return;
    case K::Not:
      out.append({ignil::Opcode::NOT, result, {lhs}, addr});
      writeBack(result); // NOT affects no flags
      return;
    case K::Neg: {
      out.append({ignil::Opcode::NEG, result, {lhs}, addr});
      writeBack(result);
      std::map<X86Flag, FlagSetter> flags{
              {X86Flag::CF,
               [this, lhs, addr](ignil::Block &b, std::uint32_t &t) {
                 auto cf = newTemp(t, 1);
                 b.append({ignil::Opcode::NEQ, cf,
                           {ignil::Value{lhs}, ignil::Const{0, lhs.width}},
                           addr});
                 b.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cf}},
                           addr});
               }},
              {X86Flag::PF, pfParity(result, addr)},
              {X86Flag::AF, afNibbleCarry(ignil::Const{0, lhs.width},
                                          ignil::Value{lhs}, result, addr)},
              {X86Flag::ZF, zfEqZero(result, addr)},
              {X86Flag::SF, sfSignBit(result, addr)},
              {X86Flag::OF,
               [this, lhs, addr](ignil::Block &b, std::uint32_t &t) {
                 const std::uint64_t minNeg = std::uint64_t{1}
                                              << (lhs.width - 1);
                 auto of = newTemp(t, 1);
                 b.append({ignil::Opcode::EQ, of,
                           {ignil::Value{lhs}, ignil::Const{minNeg, lhs.width}},
                           addr});
                 b.append({ignil::Opcode::COPY, regOF_, {ignil::Value{of}},
                           addr});
               }},
      };
      emitArithFlags(addr, out, tmp, flags);
      return;
    }
    case K::Adc:
    case K::Sbb: {
      const bool isSbb = op.kind == K::Sbb;
      auto carryIn = newTemp(tmp, w);
      out.append({w == 8 ? ignil::Opcode::COPY : ignil::Opcode::ZEXT, carryIn,
                  {regCF_}, addr});
      const ignil::Opcode step = isSbb ? ignil::Opcode::SUB : ignil::Opcode::ADD;
      auto partial = newTemp(tmp, w);
      out.append({step, partial, {lhs, rhsT}, addr});
      out.append({step, result, {partial, carryIn}, addr});
      writeBack(result);
      FlagSetter carryOut = [this, lhs, rhsT, carryIn, partial, result, isSbb,
                             addr](ignil::Block &block, std::uint32_t &t) {
        auto first = newTemp(t, 1);
        auto second = newTemp(t, 1);
        if (isSbb) {
          block.append({ignil::Opcode::ULT, first, {lhs, rhsT}, addr});
          block.append({ignil::Opcode::ULT, second, {partial, carryIn}, addr});
        } else {
          block.append({ignil::Opcode::ULT, first, {partial, lhs}, addr});
          block.append({ignil::Opcode::ULT, second, {result, partial}, addr});
        }
        auto carry = newTemp(t, 1);
        block.append({ignil::Opcode::OR, carry, {first, second}, addr});
        block.append({ignil::Opcode::COPY, regCF_, {carry}, addr});
      };
      emitArithFlags(addr, out, tmp,
                     {
                             {X86Flag::CF, carryOut},
                             {X86Flag::PF, pfParity(result, addr)},
                             {X86Flag::AF, afNibbleCarry(lhs, rhsT, result, addr)},
                             {X86Flag::ZF, zfEqZero(result, addr)},
                             {X86Flag::SF, sfSignBit(result, addr)},
                             {X86Flag::OF, isSbb ? ofSubOverflow(lhs, rhsT, result, addr)
                                                 : ofAddOverflow(lhs, rhsT, result, addr)},
                     });
      return;
    }
    default:
      return fail("unexpected kind");
  }
}

void dragon::x86::X86IGNILLifter::liftSse(const llvm::MCInst &mc,
                                          const SseOp &op, std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  using S = SseKind;
  using Opc = ignil::Opcode;
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto fail = [&](const char *why) {
    logger_.log("Error liftSse in addr {:#x}: {}", addr, why);
    out.noteUnlifted(addr);
  };
  auto emit = [&](Opc o, std::vector<ignil::Value> srcs, ignil::Width w) {
    auto t = newTemp(tmp, w);
    out.append({o, t, std::move(srcs), addr});
    return t;
  };

  // A memory destination: op[0..4] = mem, op[5] = xmm.
  if (op.memDst) {
    auto src = toReg(mc.getOperand(5).getReg());
    if (!src)
      return fail("no source");
    auto ea = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
    ignil::Value v = *src;
    if (op.memWidth < src->width)
      v = emit(Opc::TRUNC, {v}, op.memWidth);
    out.append({Opc::STORE, std::nullopt, {ea, v}, addr});
    return;
  }

  // Register destination. Two-operand SSE arithmetic ties dst to the first
  // source (op[1]); moves and COMISD do not.
  const bool tied = op.kind == S::And || op.kind == S::Andn ||
                    op.kind == S::Or || op.kind == S::Xor ||
                    op.kind == S::Addsd || op.kind == S::Subsd ||
                    op.kind == S::Divsd || op.kind == S::Cvtsi2sd ||
                    op.kind == S::Cvtss2sd || op.kind == S::Cvtsd2ss;
  const unsigned srcIdx = tied ? 2 : 1;
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst)
    return fail("no destination");

  // The source operand: memory (low memWidth bits) or a register. The
  // full-width kinds (logic, 128-bit moves) also take bits 127:64: the next
  // 8 bytes of memory, or the source's XMMnH.
  const bool fullWidth = op.kind == S::And || op.kind == S::Andn ||
                         op.kind == S::Or || op.kind == S::Xor ||
                         op.kind == S::Move;
  const std::optional<ignil::Reg> dstHi = xmmHigh(*dst);
  ignil::Value src;
  std::optional<ignil::Value> srcHi;
  if (op.memSrc) {
    auto ea = emitAddress(mc, srcIdx, ptrWidth, addr, out, tmp);
    const ignil::Width w =
            op.kind == S::Cvtsi2sd || op.kind == S::MovdTo || op.kind == S::Cvtss2sd ? op.memWidth : 64;
    src = emit(Opc::LOAD, {ea}, w);
    if (fullWidth)
      srcHi = emit(Opc::LOAD,
                   {emit(Opc::ADD, {ea, ignil::Const{8, ptrWidth}}, ptrWidth)}, 64);
  } else {
    auto r = toReg(mc.getOperand(srcIdx).getReg());
    if (!r)
      return fail("no source");
    src = *r;
    if (fullWidth)
      if (auto h = xmmHigh(*r))
        srcHi = emit(Opc::COPY, {*h}, 64);
  }
  // Bits 127:64 of the destination for the full-width kinds: computed like
  // the low half, or unknown if the source half could not be named.
  auto writeHigh = [&](Opc o, bool invertDst) {
    if (!dstHi)
      return;
    if (!srcHi) {
      emitUndef(*dstHi, addr, out);
      return;
    }
    if (o == Opc::COPY) {
      out.append({Opc::COPY, *dstHi, {*srcHi}, addr});
      return;
    }
    ignil::Value d = emit(Opc::COPY, {*dstHi}, 64);
    if (invertDst)
      d = emit(Opc::NOT, {d}, 64);
    out.append({Opc::COPY, *dstHi, {emit(o, {d, *srcHi}, 64)}, addr});
  };

  switch (op.kind) {
    case S::Comisd: {
      // dst is the first compared operand here, not a destination.
      auto a = emit(Opc::COPY, {*dst}, 64);
      auto b = emit(Opc::COPY, {src}, 64);
      auto un = emit(Opc::FUNO, {a, b}, 1);
      auto lt = emit(Opc::FLT, {a, b}, 1);
      auto eq = emit(Opc::FEQ, {a, b}, 1);
      // Unordered 111, greater 000, less 001, equal 100 (ZF, PF, CF); OF,
      // SF and AF are cleared.
      out.append({Opc::COPY, regZF_, {emit(Opc::OR, {un, eq}, 1)}, addr});
      out.append({Opc::COPY, regPF_, {un}, addr});
      out.append({Opc::COPY, regCF_, {emit(Opc::OR, {un, lt}, 1)}, addr});
      out.append({Opc::COPY, regOF_, {ignil::Const{0, 1}}, addr});
      out.append({Opc::COPY, regSF_, {ignil::Const{0, 1}}, addr});
      out.append({Opc::COPY, regAF_, {ignil::Const{0, 1}}, addr});
      return;
    }
    case S::And:
    case S::Or:
    case S::Xor: {
      const Opc o = op.kind == S::And ? Opc::AND
                    : op.kind == S::Or ? Opc::OR
                                       : Opc::XOR;
      auto r = emit(o, {*dst, src}, dst->width);
      out.append({Opc::COPY, *dst, {r}, addr});
      writeHigh(o, false);
      return;
    }
    case S::Andn: {
      auto r = emit(Opc::AND, {emit(Opc::NOT, {*dst}, dst->width), src},
                    dst->width);
      out.append({Opc::COPY, *dst, {r}, addr});
      writeHigh(Opc::AND, true);
      return;
    }
    case S::Move:
    case S::Movq: {
      // Low 64 bits; between a GPR and an XMM register the widths match.
      ignil::Value v = src;
      const ignil::Width sw = ignil::widthOf(v);
      if (sw < dst->width)
        v = emit(Opc::ZEXT, {v}, dst->width);
      else if (sw > dst->width)
        v = emit(Opc::TRUNC, {v}, dst->width);
      out.append({Opc::COPY, *dst, {v}, addr});
      // movaps & co. copy bits 127:64; movq xmm, r64/m64/xmm zeroes them.
      if (op.kind == S::Move)
        writeHigh(Opc::COPY, false);
      else if (dstHi)
        out.append({Opc::COPY, *dstHi, {ignil::Const{0, 64}}, addr});
      return;
    }
    case S::MovdTo:
    case S::MovdFrom: {
      ignil::Value v = src;
      const ignil::Width sw = ignil::widthOf(v);
      if (op.kind == S::MovdFrom && sw > 32)
        v = emit(Opc::TRUNC, {v}, 32);
      if (ignil::widthOf(v) < dst->width)
        v = emit(Opc::ZEXT, {v}, dst->width);
      out.append({Opc::COPY, *dst, {v}, addr});
      if (dstHi) // movd xmm, r32/m32 zeroes bits 127:32
        out.append({Opc::COPY, *dstHi, {ignil::Const{0, 64}}, addr});
      return;
    }
    case S::Addsd:
    case S::Subsd:
    case S::Divsd: {
      const Opc o = op.kind == S::Addsd   ? Opc::FADD
                    : op.kind == S::Subsd ? Opc::FSUB
                                          : Opc::FDIV;
      out.append({o, *dst, {*dst, src}, addr});
      return;
    }
    case S::Cvtsi2sd:
      out.append({Opc::SITOFP, *dst, {src}, addr});
      return;
    case S::Cvtss2sd:
      if (ignil::widthOf(src) != 32) src = emit(Opc::TRUNC,{src},32);
      out.append({Opc::FPEXT,*dst,{src},addr});
      return;
    case S::Cvtsd2ss: {
      auto result = emit(Opc::FPTRUNC,{src},32);
      auto upper = emit(Opc::AND,{*dst,ignil::Const{0xffffffff00000000ull,64}},64);
      auto low = emit(Opc::ZEXT,{result},64);
      out.append({Opc::COPY,*dst,{emit(Opc::OR,{upper,low},64)},addr});
      return;
    }
  }
}

void dragon::x86::X86IGNILLifter::liftMov(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftMov in addr {:#x} no dst", addr);
    return;
  }

  const auto &srcOp = mc.getOperand(1);
  if (srcOp.isImm()) {
    auto imm = static_cast<std::uint64_t>(srcOp.getImm());
    out.append(
            {ignil::Opcode::COPY, *dst, {ignil::Const{imm, dst->width}}, addr});
  } else {
    auto src = toReg(srcOp.getReg());
    if (!src) {
      emitUndef(*dst, addr, out);
      return;
    }
    out.append({ignil::Opcode::COPY, *dst, {*src}, addr});
  }
}
void dragon::x86::X86IGNILLifter::liftMovLoad(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  // op[0]=dst, op[1..5]=mem(base,scale,index,disp,seg)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftMovLoad in addr {:#x} no dst", addr);
    return;
  }

  ignil::Width ptrWidth = (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 1, ptrWidth, addr, out, tmp);
  auto loaded = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, loaded, {addrTemp}, addr});
  out.append({ignil::Opcode::COPY, *dst, {loaded}, addr});
}
void dragon::x86::X86IGNILLifter::liftMovStore(const llvm::MCInst &mc,
                                               std::uint64_t addr,
                                               ignil::Block &out,
                                               std::uint32_t &tmp) const {
  // op[0..4]=mem(base,scale,index,disp,seg), op[5]=src (reg, e.g. MOV64mr /
  // MOV32mr) or imm (e.g. MOV64mi32 / MOV32mi - `mov dword ptr [x], 0`).
  ignil::Width ptrWidth = (architecture_ == Architecture::X86_64) ? 64 : 32;
  const auto &srcOp = mc.getOperand(5);

  ignil::Value src;
  if (srcOp.isImm()) {
    // The memory operand carries no register to read a width from; the
    // opcode variant (8, 32 or 64) is the only source of truth for how wide
    // the store is.
    const unsigned opc = mc.getOpcode();
    const ignil::Width opWidth = (opc == opMov64mi_)  ? 64
                                 : (opc == opMov8mi_) ? 8
                                                      : 32;
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), opWidth};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftMovStore in addr {:#x} no src", addr);
      return;
    }
    src = *srcReg;
  }

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, src}, addr});
}
void dragon::x86::X86IGNILLifter::liftAdd(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftAdd in addr {:#x} no dst", addr);
    return;
  }

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), dst->width};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftAdd in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
    // A copy: with `add eax, eax` / `sub eax, eax` the source is the
    // destination, and the flags below must see its value before the write.
    auto srcCopy = newTemp(tmp, dst->width);
    out.append({ignil::Opcode::COPY, srcCopy, {*srcReg}, addr});
    src = srcCopy;
  }

  // Save original dst before overwrite so OF can compare pre- and post-signs.
  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::ADD, result, {lhs, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAddOverflow(result, src, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, src, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAddOverflow(lhs, src, result, addr)},
                 });
}
void dragon::x86::X86IGNILLifter::liftAddMem(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // RMW: op[0..4]=mem(base,scale,index,disp,seg), op[5]=reg-or-imm addend.
  // The destination is memory itself: load the current value, add, store
  // the result back, then set flags exactly like the register form.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  const unsigned opc = mc.getOpcode();

  ignil::Value addend;
  ignil::Width opWidth;
  const auto &srcOp = mc.getOperand(5);
  if (srcOp.isImm()) {
    opWidth = (opc == opAdd64mi_ || opc == opAdd64mi8_) ? 64 : 32;
    addend = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()),
                          opWidth};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftAddMem in addr {:#x} no src", addr);
      return;
    }
    // The register is as wide as the memory operand (AL for ADD8mr).
    opWidth = srcReg->width;
    addend = *srcReg;
  }

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto lhs = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::LOAD, lhs, {addrTemp}, addr});

  auto result = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::ADD, result, {lhs, addend}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAddOverflow(result, addend, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, addend, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAddOverflow(lhs, addend, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftSubMem(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // RMW: op[0..4]=mem(base,scale,index,disp,seg), op[5]=reg-or-imm
  // subtrahend. Mirrors liftAddMem but with SUB semantics/flags.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  const unsigned opc = mc.getOpcode();
  const ignil::Width opWidth =
          (opc == opSub64mr_ || opc == opSub64mi_ || opc == opSub64mi8_) ? 64
                                                                          : 32;

  ignil::Value rhs;
  const auto &srcOp = mc.getOperand(5);
  if (srcOp.isImm()) {
    rhs = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), opWidth};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftSubMem in addr {:#x} no src", addr);
      return;
    }
    rhs = *srcReg;
  }

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto lhs = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::LOAD, lhs, {addrTemp}, addr});

  auto result = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::SUB, result, {lhs, rhs}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(lhs, rhs, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftAddRegMem(const llvm::MCInst &mc,
                                                std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2..6]=mem(rhs). Load rhs from
  // memory, then the same ADD + flags as the register form (liftAdd).
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftAddRegMem in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 2, ptrWidth, addr, out, tmp);
  auto src = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, src, {addrTemp}, addr});

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::ADD, result, {lhs, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAddOverflow(result, src, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, src, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAddOverflow(lhs, src, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftSubRegMem(const llvm::MCInst &mc,
                                                std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp) const {
  // Mirrors liftAddRegMem but with SUB semantics/flags.
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftSubRegMem in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 2, ptrWidth, addr, out, tmp);
  auto rhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, rhs, {addrTemp}, addr});

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SUB, result, {lhs, rhs}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(lhs, rhs, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftLogicRegMem(const llvm::MCInst &mc,
                                                  std::uint64_t addr,
                                                  ignil::Block &out,
                                                  std::uint32_t &tmp,
                                                  ignil::Opcode op) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2..6]=mem(rhs).
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftLogicRegMem in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 2, ptrWidth, addr, out, tmp);
  auto src = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, src, {addrTemp}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({op, result, {*dst, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftLogicMem(const llvm::MCInst &mc,
                                               std::uint64_t addr,
                                               ignil::Block &out,
                                               std::uint32_t &tmp,
                                               ignil::Opcode op) const {
  // RMW: op[0..4]=mem, op[5]=reg-or-imm rhs.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;

  ignil::Value rhs;
  ignil::Width opWidth = 32; // only 32-bit immediate forms are registered
  const auto &srcOp = mc.getOperand(5);
  if (srcOp.isImm()) {
    rhs = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), opWidth};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftLogicMem in addr {:#x} no src", addr);
      return;
    }
    // The register is as wide as the memory operand.
    opWidth = srcReg->width;
    rhs = *srcReg;
  }

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto lhs = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::LOAD, lhs, {addrTemp}, addr});

  auto result = newTemp(tmp, opWidth);
  out.append({op, result, {lhs, rhs}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftSub(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftSub in addr {:#x} no dst", addr);
    return;
  }

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), dst->width};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftSub in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
    // A copy: with `add eax, eax` / `sub eax, eax` the source is the
    // destination, and the flags below must see its value before the write.
    auto srcCopy = newTemp(tmp, dst->width);
    out.append({ignil::Opcode::COPY, srcCopy, {*srcReg}, addr});
    src = srcCopy;
  }

  // Save original dst before overwrite so CF/OF see the pre-subtraction value.
  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SUB, result, {lhs, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(lhs, src, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, src, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(lhs, src, result, addr)},
                 });
}
void dragon::x86::X86IGNILLifter::liftXor(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftXor in addr {:#x} no dst", addr);
    return;
  }

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), dst->width};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftXor in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
    src = *srcReg;
  }
  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::XOR, result, {*dst, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}


void dragon::x86::X86IGNILLifter::liftAnd(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftAnd in addr {:#x} no dst", addr);
    return;
  }

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), dst->width};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftAnd in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
    src = *srcReg;
  }
  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::AND, result, {*dst, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}
void dragon::x86::X86IGNILLifter::liftOr(const llvm::MCInst &mc,
                                         std::uint64_t addr, ignil::Block &out,
                                         std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftOr in addr {:#x} no dst", addr);
    return;
  }

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), dst->width};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftOr in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
    src = *srcReg;
  }
  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::OR, result, {*dst, src}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}
void dragon::x86::X86IGNILLifter::liftPush(const llvm::MCInst &mc,
                                           std::uint64_t addr,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  auto src = toReg(mc.getOperand(0).getReg());
  if (!src) {
    logger_.log("Error liftPush in addr {:#x} no src", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;
  const ignil::Width srcBytes = src->width / 8;

  // The value is read before SP moves: `push rsp` stores the old RSP.
  auto value = newTemp(tmp, src->width);
  out.append({ignil::Opcode::COPY, value, {*src}, addr});

  // SP -= size
  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::SUB,
              newSP,
              {sp, ignil::Const{srcBytes, ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});

  // memory[RSP] = value
  out.append({ignil::Opcode::STORE, std::nullopt, {sp, value}, addr});
}
void dragon::x86::X86IGNILLifter::liftPushMem(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  // op[0..4]=mem, read before SP moves (`push [esp+4]`).
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto value = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::LOAD, value, {addrTemp}, addr});

  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::SUB,
              newSP,
              {sp, ignil::Const{static_cast<std::uint64_t>(ptrWidth / 8),
                                ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {sp, value}, addr});
}
void dragon::x86::X86IGNILLifter::liftPop(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftPop in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;
  const ignil::Width dstBytes = dst->width / 8;

  // loaded = memory[RSP]
  auto loaded = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, loaded, {sp}, addr});

  // RSP += size
  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::ADD,
              newSP,
              {sp, ignil::Const{dstBytes, ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});

  // dst is written last: `pop rsp` leaves RSP = the loaded value, not
  // value + size.
  out.append({ignil::Opcode::COPY, *dst, {loaded}, addr});
}

void dragon::x86::X86IGNILLifter::liftPopMem(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // op[0..4]=mem, addressed after SP moves (`pop [esp+4]`).
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;

  auto value = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::LOAD, value, {sp}, addr});

  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::ADD,
              newSP,
              {sp, ignil::Const{static_cast<std::uint64_t>(ptrWidth / 8),
                                ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, value}, addr});
}

// POPFQ/PUSHFQ move RFLAGS to/from the stack. Only the flags IGNIL models are
// handled (CF/PF/AF/ZF/SF/DF/OF, at their RFLAGS bit positions below); the
// system bits (TF/IF/IOPL/NT/...) are not modeled - POPFQ drops them and
// PUSHFQ emits them as 0. Bit 1 of RFLAGS always reads as 1 (reserved), which
// PUSHFQ sets explicitly.
void dragon::x86::X86IGNILLifter::liftPopf(const llvm::MCInst & /*mc*/,
                                           std::uint64_t addr,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // POPFQ: t = memory[RSP]; scatter t's bits into the modeled flag registers;
  // RSP += 8. Unmodeled system bits in t are simply not read.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;

  auto loaded = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::LOAD, loaded, {sp}, addr});

  const std::array<std::pair<ignil::Reg, unsigned>, 7> flags = {{
          {regCF_, 0}, {regPF_, 2}, {regAF_, 4}, {regZF_, 6}, {regSF_, 7},
          {regDF_, 10}, {regOF_, 11},
  }};
  for (const auto &[flagReg, bit]: flags) {
    auto shifted = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::SHR,
                shifted,
                {loaded, ignil::Const{bit, ptrWidth}},
                addr});
    auto masked = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::AND,
                masked,
                {shifted, ignil::Const{1, ptrWidth}},
                addr});
    auto narrowed = newTemp(tmp, flagReg.width);
    out.append({ignil::Opcode::TRUNC, narrowed, {masked}, addr});
    out.append({ignil::Opcode::COPY, flagReg, {narrowed}, addr});
  }

  // RSP += pointer size (8 in 64-bit).
  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::ADD,
              newSP,
              {sp, ignil::Const{static_cast<std::uint64_t>(ptrWidth / 8), ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});
}

void dragon::x86::X86IGNILLifter::liftPushf(const llvm::MCInst & /*mc*/,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  // PUSHFQ: gather the modeled flags into an RFLAGS word (reserved bit 1 = 1,
  // unmodeled system bits = 0); RSP -= 8; memory[RSP] = word.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  ignil::Reg sp = (architecture_ == Architecture::X86_64) ? regRSP_ : regESP_;

  const std::array<std::pair<ignil::Reg, unsigned>, 7> flags = {{
          {regCF_, 0}, {regPF_, 2}, {regAF_, 4}, {regZF_, 6}, {regSF_, 7},
          {regDF_, 10}, {regOF_, 11},
  }};
  ignil::Value acc = ignil::Const{2, ptrWidth}; // reserved bit 1
  for (const auto &[flagReg, bit]: flags) {
    auto widened = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::ZEXT, widened, {flagReg}, addr});
    auto shifted = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::SHL,
                shifted,
                {widened, ignil::Const{bit, ptrWidth}},
                addr});
    auto combined = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::OR, combined, {acc, shifted}, addr});
    acc = combined;
  }

  // RSP -= pointer size, then store the word.
  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::SUB,
              newSP,
              {sp, ignil::Const{static_cast<std::uint64_t>(ptrWidth / 8), ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {sp, acc}, addr});
}

void dragon::x86::X86IGNILLifter::liftPusha(const llvm::MCInst & /*mc*/,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  // Every slot is addressed from the original ESP, which is also what is saved.
  const std::array<ignil::Reg, 8> regs = {regEAX_, regECX_, regEDX_, regEBX_,
                                          regESP_, regEBP_, regESI_, regEDI_};
  for (std::size_t i = 0; i < regs.size(); ++i) {
    auto slot = newTemp(tmp, 32);
    out.append({ignil::Opcode::SUB,
                slot,
                {regESP_, ignil::Const{4 * (i + 1), 32}},
                addr});
    out.append({ignil::Opcode::STORE, std::nullopt, {slot, regs[i]}, addr});
  }

  auto newSP = newTemp(tmp, 32);
  out.append(
          {ignil::Opcode::SUB, newSP, {regESP_, ignil::Const{32, 32}}, addr});
  out.append({ignil::Opcode::COPY, regESP_, {newSP}, addr});
}

void dragon::x86::X86IGNILLifter::liftPopa(const llvm::MCInst & /*mc*/,
                                           std::uint64_t addr,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // EAX is at ESP+28, down to EDI at ESP+0.
  const std::array<ignil::Reg, 8> regs = {regEAX_, regECX_, regEDX_, regEBX_,
                                          regESP_, regEBP_, regESI_, regEDI_};
  for (std::size_t i = 0; i < regs.size(); ++i) {
    if (i == 4) // the saved ESP is discarded
      continue;
    auto slot = newTemp(tmp, 32);
    out.append({ignil::Opcode::ADD,
                slot,
                {regESP_, ignil::Const{28 - 4 * i, 32}},
                addr});
    auto loaded = newTemp(tmp, 32);
    out.append({ignil::Opcode::LOAD, loaded, {slot}, addr});
    out.append({ignil::Opcode::COPY, regs[i], {loaded}, addr});
  }

  auto newSP = newTemp(tmp, 32);
  out.append(
          {ignil::Opcode::ADD, newSP, {regESP_, ignil::Const{32, 32}}, addr});
  out.append({ignil::Opcode::COPY, regESP_, {newSP}, addr});
}

void dragon::x86::X86IGNILLifter::liftPushSeg(const llvm::MCInst & /*mc*/,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  // Segment registers are not modeled: the pushed selector is UNDEF.
  auto selector = newTemp(tmp, 32);
  out.append({ignil::Opcode::UNDEF, selector, {}, addr});
  auto newSP = newTemp(tmp, 32);
  out.append({ignil::Opcode::SUB, newSP, {regESP_, ignil::Const{4, 32}}, addr});
  out.append({ignil::Opcode::COPY, regESP_, {newSP}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {regESP_, selector}, addr});
}

void dragon::x86::X86IGNILLifter::liftPopSeg(const llvm::MCInst & /*mc*/,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // Not modeled: the selector, and POP SS inhibiting interrupts and traps
  // for one instruction.
  auto newSP = newTemp(tmp, 32);
  out.append({ignil::Opcode::ADD, newSP, {regESP_, ignil::Const{4, 32}}, addr});
  out.append({ignil::Opcode::COPY, regESP_, {newSP}, addr});
}

void dragon::x86::X86IGNILLifter::liftLea(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftLea in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 1, ptrWidth, addr, out, tmp);
  out.append({ignil::Opcode::COPY, *dst, {addrTemp}, addr});
}
void dragon::x86::X86IGNILLifter::liftCmp(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // CMP has no output: op[0]=lhs, op[1]=rhs(reg or imm)
  auto lhs = toReg(mc.getOperand(0).getReg());
  if (!lhs) {
    logger_.log("Error liftCmp in addr {:#x} no lhs", addr);
    return;
  }

  ignil::Value rhs;
  if (const auto &rhsOp = mc.getOperand(1); rhsOp.isImm()) {
    rhs = ignil::Const{static_cast<std::uint64_t>(rhsOp.getImm()), lhs->width};
  } else {
    auto rhsReg = toReg(rhsOp.getReg());
    if (!rhsReg) {
      logger_.log("Error liftCmp in addr {:#x} no rhs", addr);
      return;
    }
    rhs = *rhsReg;
  }

  // CMP is SUB that discards the result but keeps all flags.
  // lhs is not overwritten so its Reg can be used directly in CF/OF factories.
  auto result = newTemp(tmp, lhs->width);
  out.append({ignil::Opcode::SUB, result, {*lhs, rhs}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(*lhs, rhs, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(*lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(*lhs, rhs, result, addr)},
                 });
}
void dragon::x86::X86IGNILLifter::liftCmpRegMem(const llvm::MCInst &mc,
                                                std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp) const {
  // CMP reg,mem: op[0]=reg(lhs), op[1..5]=mem(rhs). Load rhs, SUB, discard
  // the result but keep the flags - same as liftCmp with rhs from memory.
  auto lhs = toReg(mc.getOperand(0).getReg());
  if (!lhs) {
    logger_.log("Error liftCmpRegMem in addr {:#x} no lhs", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 1, ptrWidth, addr, out, tmp);
  auto rhs = newTemp(tmp, lhs->width);
  out.append({ignil::Opcode::LOAD, rhs, {addrTemp}, addr});

  auto result = newTemp(tmp, lhs->width);
  out.append({ignil::Opcode::SUB, result, {*lhs, rhs}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(*lhs, rhs, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(*lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(*lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftCmpMem(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // CMP mem,reg / CMP mem,imm: op[0..4]=mem(lhs), op[5]=reg-or-imm(rhs).
  // Load lhs, SUB, discard the result but keep the flags. No writeback -
  // unlike ADD/SUB, CMP never modifies its destination.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  const unsigned opc = mc.getOpcode();
  const ignil::Width opWidth =
          (opc == opCmp64mr_ || opc == opCmp64mi_ || opc == opCmp64mi8_) ? 64
          : (opc == opCmp8mi_)                                           ? 8
                                                                         : 32;

  ignil::Value rhs;
  const auto &srcOp = mc.getOperand(5);
  if (srcOp.isImm()) {
    rhs = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), opWidth};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftCmpMem in addr {:#x} no rhs", addr);
      return;
    }
    rhs = *srcReg;
  }

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto lhs = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::LOAD, lhs, {addrTemp}, addr});

  auto result = newTemp(tmp, opWidth);
  out.append({ignil::Opcode::SUB, result, {lhs, rhs}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfSubBorrow(lhs, rhs, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofSubOverflow(lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftTest(const llvm::MCInst &mc,
                                           std::uint64_t addr,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // op[0]=lhs(reg), op[1]=rhs(reg or imm)
  auto lhs = toReg(mc.getOperand(0).getReg());
  if (!lhs) {
    logger_.log("Error liftTest in addr {:#x} no lhs", addr);
    return;
  }

  ignil::Value rhs;
  const auto &rhsOp = mc.getOperand(1);
  if (rhsOp.isImm()) {
    rhs = ignil::Const{static_cast<std::uint64_t>(rhsOp.getImm()), lhs->width};
  } else {
    auto rhsReg = toReg(rhsOp.getReg());
    if (!rhsReg) {
      logger_.log("Error liftTest in addr {:#x} no rhs", addr);
      return;
    }
    rhs = *rhsReg;
  }

  // TEST is AND that discards the result but keeps flags; CF and OF always 0.
  auto result = newTemp(tmp, lhs->width);
  out.append({ignil::Opcode::AND, result, {*lhs, rhs}, addr});
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAlwaysZero(addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afAlwaysZero(addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAlwaysZero(addr)},
                 });
}
std::uint64_t dragon::x86::X86IGNILLifter::pcRelTarget(
        std::uint64_t addr, std::uint8_t insnSize, std::int64_t offset) const {
  const auto target = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(addr) + insnSize + offset);
  // EIP wraps at 4 GB.
  return (architecture_ == Architecture::X86_64) ? target
                                                 : (target & 0xffffffffULL);
}

void dragon::x86::X86IGNILLifter::liftJcc(const llvm::MCInst &mc,
                                          std::uint64_t addr,
                                          std::uint8_t insnSize,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // LLVM 15+ unifies all Jcc into JCC_1/JCC_4 with layout:
  //   op[0] = signed PC-relative offset (imm)
  //   op[1] = condition code (imm, LLVM x86 CondCode enum 0-15)
  const std::uint64_t taken =
          pcRelTarget(addr, insnSize, mc.getOperand(0).getImm());
  const std::uint64_t fallthrough = pcRelTarget(addr, insnSize, 0);

  unsigned condCode = (mc.getNumOperands() >= 2 && mc.getOperand(1).isImm())
                              ? static_cast<unsigned>(mc.getOperand(1).getImm())
                              : 0;

  auto cond = emitCondition(condCode, addr, out, tmp);
  out.append({ignil::Opcode::CJMP,
              std::nullopt,
              {cond, ignil::Const{taken, 64}, ignil::Const{fallthrough, 64}},
              addr});
}

bool dragon::x86::X86IGNILLifter::liftString(const Instruction &insn,
    ignil::Block &out,std::uint32_t &tmp) const {
  const auto *mc=insn.mcInst();
  const auto name=instrInfo_->getName(mc->getOpcode());
  enum class Kind { Move,Store,Load,Compare,Scan };
  std::optional<Kind> kind;
  ignil::Width width=0;
  for (const auto &[prefix,operation] : {std::pair{"MOVS",Kind::Move},std::pair{"STOS",Kind::Store},
       std::pair{"LODS",Kind::Load},std::pair{"CMPS",Kind::Compare},std::pair{"SCAS",Kind::Scan}})
    for (const auto &[suffix,bits] : {std::pair{'B',8},std::pair{'W',16},std::pair{'L',32},std::pair{'Q',64}})
      if (name == std::string(prefix)+suffix) { kind=operation; width=bits; }
  if (!kind) return false;
  using O=ignil::Opcode;
  using ignil::Value;
  const auto address=insn.address();
  const bool is64=architecture_ == Architecture::X86_64;
  const auto pointerReg=toReg(mc->getOperand(0).getReg());
  const ignil::Width aw=pointerReg ? pointerReg->width :
      hasAddressSizePrefix(insn) ? (is64 ? 32 : 16) : (is64 ? 64 : 32);
  const ignil::Reg source{kOffRSI,aw,"string.source"},destination{kOffRDI,aw,"string.destination"},
                   counter{kOffRCX,aw,"string.count"};
  const auto accumulator=width == 8 ? regAL_ : width == 16 ? regAX_ : width == 32 ? regEAX_ : regRAX_;
  unsigned repetition=0;
  bool unknownSegment=false;
  for (auto byte : insn.bytes()) {
    if (byte == 0xf2 || byte == 0xf3) repetition=byte;
    else if (byte == 0x64 || byte == 0x65) unknownSegment=true;
    else if (byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e) unknownSegment=false;
    else if (byte == 0x66 || byte == 0x67 || byte == 0xf0 || (is64 && byte >= 0x40 && byte <= 0x4f)) {}
    else break;
  }
  const bool readSource=*kind == Kind::Move || *kind == Kind::Load || *kind == Kind::Compare;
  const bool useDestination=*kind != Kind::Load;
  const bool compare=*kind == Kind::Compare || *kind == Kind::Scan;
  if (unknownSegment && readSource) out.noteUnlifted(address); // FS/GS base is outside this register model.
  auto emit=[&](O code,std::vector<Value> values,ignil::Width bits) {
    auto result=newTemp(tmp,bits); out.append({code,result,std::move(values),address}); return result;
  };
  auto constant=[](uint64_t value,ignil::Width bits) -> Value {
    return ignil::Const{bits >= 64 ? value : value & ((1ull << bits)-1),bits};
  };
  std::uint32_t header=0,done=0;
  if (repetition) {
    header=tmp++; done=tmp++;
    out.append({O::LABEL,std::nullopt,{},address,header});
    const auto empty=emit(O::EQ,{counter,constant(0,aw)},1);
    out.append({O::LOCALCJMP,std::nullopt,{empty},address,done});
  }
  // All source reads happen before writes, including when source/destination
  // overlap. Each REP iteration repeats these loads with the updated pointers.
  const auto direction=emit(O::NEQ,{regDF_,constant(0,8)},1);
  const auto mask=emit(O::NEG,{emit(O::ZEXT,{direction},aw)},aw);
  const auto positive=constant(width/8,aw),negative=constant(-uint64_t(width/8),aw);
  const auto stride=emit(O::OR,{emit(O::AND,{mask,negative},aw),
                              emit(O::AND,{emit(O::NOT,{mask},aw),positive},aw)},aw);
  Value left=accumulator;
  if (readSource) {
    const Value pointer=unknownSegment ? Value{emit(O::UNDEF,{},aw)} : Value{source};
    left=emit(O::LOAD,{pointer},width);
  }
  if (*kind == Kind::Move || *kind == Kind::Store)
    out.append({O::STORE,std::nullopt,{destination,left},address});
  else if (*kind == Kind::Load) out.append({O::COPY,accumulator,{left},address});
  else {
    const auto right=emit(O::LOAD,{destination},width);
    const auto result=emit(O::SUB,{left,right},width);
    emitArithFlags(address,out,tmp,{{X86Flag::CF,cfSubBorrow(left,right,address)},
      {X86Flag::PF,pfParity(result,address)},{X86Flag::AF,afNibbleCarry(left,right,result,address)},
      {X86Flag::ZF,zfEqZero(result,address)},{X86Flag::SF,sfSignBit(result,address)},
      {X86Flag::OF,ofSubOverflow(left,right,result,address)}});
  }
  if (readSource) out.append({O::COPY,source,{emit(O::ADD,{source,stride},aw)},address});
  if (useDestination) out.append({O::COPY,destination,{emit(O::ADD,{destination,stride},aw)},address});
  if (repetition) {
    out.append({O::COPY,counter,{emit(O::SUB,{counter,constant(1,aw)},aw)},address});
    if (compare) {
      const auto stop=emit(repetition == 0xf3 ? O::EQ : O::NEQ,{regZF_,constant(0,8)},1);
      out.append({O::LOCALCJMP,std::nullopt,{stop},address,done});
    }
    out.append({O::LOCALJMP,std::nullopt,{},address,header});
    out.append({O::LABEL,std::nullopt,{},address,done});
  }
  return true;
}

bool dragon::x86::X86IGNILLifter::hasAddressSizePrefix(
        const Instruction &insn) {
  for (const std::uint8_t b: insn.bytes()) {
    switch (b) {
      case 0x67:
        return true;
      case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x64: case 0x65:
      case 0x66: case 0xf0: case 0xf2: case 0xf3:
        continue; // another legacy prefix
      default:
        return false;
    }
  }
  return false;
}

void dragon::x86::X86IGNILLifter::liftLoop(const llvm::MCInst &mc,
                                           std::uint64_t addr,
                                           std::uint8_t insnSize,
                                           bool addressSizePrefix,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // Handles LOOP, LOOPE, LOOPNE, JCXZ, JECXZ and JRCXZ.
  // op[0] = signed PC-relative offset. No flags are touched.
  const unsigned opc = mc.getOpcode();
  const std::uint64_t taken =
          pcRelTarget(addr, insnSize, mc.getOperand(0).getImm());
  const std::uint64_t fallthrough = pcRelTarget(addr, insnSize, 0);

  if (opc == opJcxz_ || opc == opJecxz_ || opc == opJrcxz_) {
    // Jump if the counter is zero; nothing is written. The opcode itself
    // names the register (the 0x67 prefix is what selects JECXZ in 64-bit
    // mode, and LLVM already folded it into the opcode).
    const ignil::Reg counter =
            opc == opJrcxz_ ? regRCX_ : (opc == opJecxz_ ? regECX_ : regCX_);
    auto cond = newTemp(tmp, 1);
    out.append({ignil::Opcode::EQ,
                cond,
                {counter, ignil::Const{0, counter.width}},
                addr});
    out.append({ignil::Opcode::CJMP,
                std::nullopt,
                {cond, ignil::Const{taken, 64}, ignil::Const{fallthrough, 64}},
                addr});
    return;
  }

  // LOOP/LOOPcc: the counter is as wide as the address size (0x67 halves it).
  const bool is64 = architecture_ == Architecture::X86_64;
  const ignil::Reg counter = is64 ? (addressSizePrefix ? regECX_ : regRCX_)
                                  : (addressSizePrefix ? regCX_ : regECX_);

  auto next = newTemp(tmp, counter.width);
  out.append({ignil::Opcode::SUB,
              next,
              {counter, ignil::Const{1, counter.width}},
              addr});
  out.append({ignil::Opcode::COPY, counter, {next}, addr});
  auto cond = newTemp(tmp, 1);
  out.append({ignil::Opcode::NEQ,
              cond,
              {next, ignil::Const{0, counter.width}},
              addr});
  if (opc == opLoope_ || opc == opLoopne_) {
    // ... and ZF == 1 (LOOPE) / ZF == 0 (LOOPNE).
    auto zf = newTemp(tmp, 1);
    out.append({opc == opLoope_ ? ignil::Opcode::NEQ : ignil::Opcode::EQ,
                zf,
                {regZF_, ignil::Const{0, regZF_.width}},
                addr});
    auto both = newTemp(tmp, 1);
    out.append({ignil::Opcode::AND, both, {cond, zf}, addr});
    cond = both;
  }
  out.append({ignil::Opcode::CJMP,
              std::nullopt,
              {cond, ignil::Const{taken, 64}, ignil::Const{fallthrough, 64}},
              addr});
}

void dragon::x86::X86IGNILLifter::liftJmp(const llvm::MCInst &mc,
                                          const std::uint64_t addr,
                                          std::uint8_t insnSize,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  if (const auto &op = mc.getOperand(0); op.isImm()) {
    const std::uint64_t target = pcRelTarget(addr, insnSize, op.getImm());
    out.append({ignil::Opcode::JMP,
                std::nullopt,
                {ignil::Const{target, 64}},
                addr});
  } else if (op.isReg()) {
    auto targetReg = toReg(op.getReg());
    if (!targetReg) {
      logger_.log("Error liftJmp in addr {:#x} no target register", addr);
      return;
    }
    out.append({ignil::Opcode::JMP, std::nullopt, {*targetReg}, addr});
  }
}
void dragon::x86::X86IGNILLifter::liftJmpMem(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             std::uint8_t insnSize,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  ignil::Width ptrWidth = (architecture_ == Architecture::X86_64) ? 64 : 32;
  // generate an address from the instruction
  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  // generate a target that will be used to store the address
  auto target = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::LOAD, target, {addrTemp}, addr});
  out.append({ignil::Opcode::JMP, std::nullopt, {target}, addr});
}
void dragon::x86::X86IGNILLifter::liftCallMem(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              std::uint8_t insnSize,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  ignil::Width ptrWidth = (architecture_ == Architecture::X86_64) ? 64 : 32;
  // generate an address from the instruction
  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  // generate a target that will be used to store the address
  auto target = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::LOAD, target, {addrTemp}, addr});
  out.append({ignil::Opcode::CALL, std::nullopt, {target}, addr});
}


void dragon::x86::X86IGNILLifter::liftCall(const llvm::MCInst &mc,
                                           std::uint64_t addr,
                                           std::uint8_t insnSize,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  if (const auto &op = mc.getOperand(0); op.isImm()) {
    // Direct call: pc-relative offset from end of instruction
    const std::uint64_t target = pcRelTarget(addr, insnSize, op.getImm());
    out.append({ignil::Opcode::CALL,
                std::nullopt,
                {ignil::Const{target, 64}},
                addr});
  } else if (op.isReg()) {
    // Indirect call through register
    auto targetReg = toReg(op.getReg());
    if (!targetReg) {
      logger_.log("Error liftCall in addr {:#x} no target register", addr);
      return;
    }
    out.append({ignil::Opcode::CALL, std::nullopt, {*targetReg}, addr});
  }
}

void dragon::x86::X86IGNILLifter::liftRet(const llvm::MCInst &mc,
                                          std::uint64_t addr, ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // ret N (RETI64/RETI32) pops N extra bytes after the return address.
  // IGNIL RET has no operands; N travels in aux, which the LLVM lowering
  // adds to the stack pointer together with the return address.
  ignil::Op ret{ignil::Opcode::RET, std::nullopt, {}, addr};
  if (mc.getNumOperands() > 0 && mc.getOperand(0).isImm())
    ret.aux = static_cast<std::uint32_t>(mc.getOperand(0).getImm() & 0xffff);
  out.append(std::move(ret));
}

void dragon::x86::X86IGNILLifter::liftLeave(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  // leave ≡  mov rsp, rbp  ;  pop rbp
  // Decomposed into IGNIL primitives (P-CODE / VEX style):
  //   rsp  = rbp
  //   t0   = memory[rsp]
  //   rbp  = t0
  //   rsp  = rsp + ptrBytes
  const bool is64 = (architecture_ == Architecture::X86_64);
  const ignil::Width ptrWidth = is64 ? 64 : 32;
  const std::uint64_t ptrBytes = is64 ? 8 : 4;
  ignil::Reg sp = is64 ? regRSP_ : regESP_;
  ignil::Reg bp = is64 ? regRBP_ : regEBP_;

  // mov rsp, rbp
  out.append({ignil::Opcode::COPY, sp, {bp}, addr});

  // pop rbp
  auto loaded = newTemp(tmp, bp.width);
  out.append({ignil::Opcode::LOAD, loaded, {sp}, addr});
  out.append({ignil::Opcode::COPY, bp, {loaded}, addr});
  auto newSP = newTemp(tmp, ptrWidth);
  out.append({ignil::Opcode::ADD,
              newSP,
              {sp, ignil::Const{ptrBytes, ptrWidth}},
              addr});
  out.append({ignil::Opcode::COPY, sp, {newSP}, addr});
}

void dragon::x86::X86IGNILLifter::liftMovExtend(const llvm::MCInst &mc,
                                               std::uint64_t addr,
                                               MovExtendForm form,
                                               bool signExtend,
                                               ignil::Block &out,
                                               std::uint32_t &tmp) const {
  // op[0]=dst(wider); op[1]=src(narrower) for the rr forms, op[1..5]=mem for
  // the rm ones. Neither MOVSX nor MOVZX affects flags.
  const ignil::Opcode extend =
          signExtend ? ignil::Opcode::SEXT : ignil::Opcode::ZEXT;
  const char *what = signExtend ? "liftMovsx" : "liftMovzx";

  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error {} in addr {:#x} no dst", what, addr);
    return;
  }

  // Memory source: load form.srcWidth bits from the effective address, then
  // widen. Note this cannot be decided from the operands - op[1] is a
  // register in both forms (here it is the memory operand's base), so the
  // opcode's own spelling is what says which it is.
  if (form.memorySource) {
    const ignil::Width ptrWidth =
            (architecture_ == Architecture::X86_64) ? 64 : 32;
    auto addrTemp = emitAddress(mc, 1, ptrWidth, addr, out, tmp);
    auto loaded = newTemp(tmp, form.srcWidth);
    out.append({ignil::Opcode::LOAD, loaded, {addrTemp}, addr});
    out.append({extend, *dst, {loaded}, addr});
    return;
  }

  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error {} in addr {:#x} no src", what, addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({extend, *dst, {*src}, addr});
}

void dragon::x86::X86IGNILLifter::liftSarImm(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src(tied=dst), op[2]=imm shift count.
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftSarImm in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftSarImm in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  const auto imm = static_cast<std::uint64_t>(mc.getOperand(2).getImm());

  // Real x86: shift by 0 leaves every flag - and the value - exactly as it
  // was, not even recomputed.
  if (imm == 0) {
    out.append({ignil::Opcode::COPY, *dst, {*src}, addr});
    return;
  }

  // Save the original value before dst gets overwritten below (dst and src
  // are the same tied register) - the CF FlagSetter below runs later, after
  // that overwrite, so it must read this saved Temp, not the Reg directly
  // (same reasoning liftAdd's `lhs` save applies to OF).
  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*src}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SAR,
              result,
              {ignil::Value{lhs}, ignil::Const{imm, dst->width}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  auto cfLastBitOut = [this, lhs, imm,
                       addr](ignil::Block &out, std::uint32_t &tmp) {
    const ignil::Width w = lhs.width;
    auto shifted = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR,
                shifted,
                {ignil::Value{lhs}, ignil::Const{imm - 1, w}},
                addr});
    auto masked = newTemp(tmp, w);
    out.append(
            {ignil::Opcode::AND, masked, {shifted, ignil::Const{1, w}}, addr});
    auto bit = newTemp(tmp, 1);
    out.append(
            {ignil::Opcode::NEQ, bit, {masked, ignil::Const{0, w}}, addr});
    out.append({ignil::Opcode::COPY, regCF_, {bit}, addr});
  };

  // OF is only architecturally defined for a 1-bit shift, where an
  // arithmetic right shift can never overflow (the sign bit is replicated,
  // never changed) - always 0. Left UNDEF (omitted from the map) for any
  // other count, matching this lifter's "UNDEF for genuinely
  // spec-undefined" convention elsewhere rather than guessing.
  std::map<X86Flag, FlagSetter> flags{
          {X86Flag::CF, cfLastBitOut},
          {X86Flag::PF, pfParity(result, addr)},
          {X86Flag::AF, afAlwaysZero(addr)},
          {X86Flag::ZF, zfEqZero(result, addr)},
          {X86Flag::SF, sfSignBit(result, addr)},
  };
  if (imm == 1)
    flags.emplace(X86Flag::OF, [this, addr](ignil::Block &out, std::uint32_t &) {
      out.append({ignil::Opcode::COPY, regOF_, {ignil::Const{0, 1}}, addr});
    });
  emitArithFlags(addr, out, tmp, flags);
}

void dragon::x86::X86IGNILLifter::liftImulImm(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  // op[0]=dst, op[1]=src, op[2]=imm (already sign-extended to dst's width
  // by LLVM's MCInst decode - same convention liftAdd already relies on).
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftImulImm in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftImulImm in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  const auto imm = static_cast<std::uint64_t>(mc.getOperand(2).getImm());

  // CF = OF = the signed product does not fit in dst (SF/ZF/AF/PF stay
  // UNDEF - architecturally undefined). See liftImulRR.
  const std::uint64_t mask =
          dst->width >= 64 ? ~0ULL : ((1ULL << dst->width) - 1);
  const ignil::Value factor = ignil::Const{imm & mask, dst->width};
  const auto [lowHalf, highHalf] =
          emitWideProduct(*src, factor, dst->width, true, addr, out, tmp);
  out.append({ignil::Opcode::COPY, *dst, {lowHalf}, addr});
  emitProductFlags(lowHalf, highHalf, true, dst->width, addr, out, tmp);
}

void dragon::x86::X86IGNILLifter::liftImulRR(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg) - unlike
  // liftImulImm, both operands are registers here, so op[1] really is the
  // same register as op[0] (the tied 2-address form), and the actual second
  // factor is op[2].
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftImulRR in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftImulRR in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }

  // CF = OF = the signed product does not fit in dst. This used to leave
  // every flag UNDEF on the grounds that IGNIL cannot form a double-width
  // product - but emitWideProduct does exactly that for one-operand IMUL.
  const auto [lowHalf, highHalf] =
          emitWideProduct(*dst, *src, dst->width, true, addr, out, tmp);
  out.append({ignil::Opcode::COPY, *dst, {lowHalf}, addr});
  emitProductFlags(lowHalf, highHalf, true, dst->width, addr, out, tmp);
}

void dragon::x86::X86IGNILLifter::liftLeaTrunc32(const llvm::MCInst &mc,
                                                 std::uint64_t addr,
                                                 ignil::Block &out,
                                                 std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftLeaTrunc32 in addr {:#x} no dst", addr);
    return;
  }
  // Address registers are always 64-bit for this form (that's the whole
  // point of LEA64_32r vs plain LEA32r), regardless of the 32-bit dst.
  auto addrTemp = emitAddress(mc, 1, 64, addr, out, tmp);
  auto truncated = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::TRUNC, truncated, {addrTemp}, addr});
  out.append({ignil::Opcode::COPY, *dst, {truncated}, addr});
}

void dragon::x86::X86IGNILLifter::liftInc(const llvm::MCInst &mc,
                                          std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src(tied=dst).
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftInc in addr {:#x} no dst", addr);
    return;
  }

  // Save the original value before dst gets overwritten (same reasoning
  // as liftAdd's `lhs` and liftSarImm's fix earlier).
  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::ADD,
              result,
              {ignil::Value{lhs}, ignil::Const{1, dst->width}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  // CF is deliberately NOT touched at all - no Op for it, not even UNDEF.
  // That is the entire point of INC over ADD-by-1: it must not disturb a
  // carry chain a previous instruction set up. PF/AF stay UNDEF, matching
  // this lifter's existing convention elsewhere.
  pfParity(result, addr)(out, tmp);
  afNibbleCarry(ignil::Value{lhs}, ignil::Const{1, dst->width}, result, addr)(out, tmp);
  zfEqZero(result, addr)(out, tmp);
  sfSignBit(result, addr)(out, tmp);
  // OF: set iff the original value was the maximum positive value for its
  // width (non-negative before, negative after - a positive overflow).
  {
    auto lhsNeg = newTemp(tmp, 1);
    out.append(
            {ignil::Opcode::SLT, lhsNeg, {ignil::Value{lhs}, ignil::Const{0, dst->width}}, addr});
    auto lhsNonNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::NOT, lhsNonNeg, {ignil::Value{lhsNeg}}, addr});
    auto resNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, resNeg, {result, ignil::Const{0, dst->width}}, addr});
    auto of = newTemp(tmp, 1);
    out.append({ignil::Opcode::AND, of, {ignil::Value{lhsNonNeg}, ignil::Value{resNeg}}, addr});
    out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{of}}, addr});
  }
}

void dragon::x86::X86IGNILLifter::liftDec(const llvm::MCInst &mc,
                                          std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftDec in addr {:#x} no dst", addr);
    return;
  }

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SUB,
              result,
              {ignil::Value{lhs}, ignil::Const{1, dst->width}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  // Same CF-untouched reasoning as liftInc.
  pfParity(result, addr)(out, tmp);
  afNibbleCarry(ignil::Value{lhs}, ignil::Const{1, dst->width}, result, addr)(out, tmp);
  zfEqZero(result, addr)(out, tmp);
  sfSignBit(result, addr)(out, tmp);
  // OF: set iff the original value was the minimum negative value for its
  // width (negative before, non-negative after - a negative overflow).
  {
    auto lhsNeg = newTemp(tmp, 1);
    out.append(
            {ignil::Opcode::SLT, lhsNeg, {ignil::Value{lhs}, ignil::Const{0, dst->width}}, addr});
    auto resNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, resNeg, {result, ignil::Const{0, dst->width}}, addr});
    auto resNonNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::NOT, resNonNeg, {ignil::Value{resNeg}}, addr});
    auto of = newTemp(tmp, 1);
    out.append({ignil::Opcode::AND, of, {ignil::Value{lhsNeg}, ignil::Value{resNonNeg}}, addr});
    out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{of}}, addr});
  }
}

void dragon::x86::X86IGNILLifter::liftNeg(const llvm::MCInst &mc,
                                          std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftNeg in addr {:#x} no dst", addr);
    return;
  }

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::NEG, result, {ignil::Value{lhs}}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  std::map<X86Flag, FlagSetter> flags{
          // CF = (src != 0): negating zero doesn't borrow, anything else
          // does.
          {X86Flag::CF,
           [this, lhs, addr](ignil::Block &out, std::uint32_t &tmp) {
             auto cf = newTemp(tmp, 1);
             out.append({ignil::Opcode::NEQ,
                         cf,
                         {ignil::Value{lhs}, ignil::Const{0, lhs.width}},
                         addr});
             out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cf}}, addr});
           }},
          {X86Flag::PF, pfParity(result, addr)},
          // AF for NEG: borrow into bit 4 of 0 - src.
          {X86Flag::AF,
           afNibbleCarry(ignil::Const{0, lhs.width}, ignil::Value{lhs}, result,
                         addr)},
          {X86Flag::ZF, zfEqZero(result, addr)},
          {X86Flag::SF, sfSignBit(result, addr)},
          // OF = (src was the minimum negative value for its width) - the
          // one value whose negation can't be represented.
          {X86Flag::OF,
           [this, lhs, addr](ignil::Block &out, std::uint32_t &tmp) {
             const std::uint64_t minNeg =
                     lhs.width >= 64 ? (std::uint64_t{1} << 63)
                                     : (std::uint64_t{1} << (lhs.width - 1));
             auto of = newTemp(tmp, 1);
             out.append({ignil::Opcode::EQ,
                         of,
                         {ignil::Value{lhs}, ignil::Const{minNeg, lhs.width}},
                         addr});
             out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{of}}, addr});
           }},
  };
  emitArithFlags(addr, out, tmp, flags);
}


void dragon::x86::X86IGNILLifter::liftShlImm(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src(tied=dst), op[2]=imm shift count.
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftShlImm in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftShlImm in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  const auto imm = static_cast<std::uint64_t>(mc.getOperand(2).getImm());

  if (imm == 0) {
    out.append({ignil::Opcode::COPY, *dst, {*src}, addr});
    return;
  }

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*src}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SHL,
              result,
              {ignil::Value{lhs}, ignil::Const{imm, dst->width}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  // CF = the last bit shifted out the top (bit width-imm of the original
  // value) - the opposite end from SAR/SHR.
  auto cfLastBitOut = [this, lhs, imm,
                       addr](ignil::Block &out, std::uint32_t &tmp) {
    const ignil::Width w = lhs.width;
    auto shifted = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR,
                shifted,
                {ignil::Value{lhs}, ignil::Const{w - imm, w}},
                addr});
    auto masked = newTemp(tmp, w);
    out.append(
            {ignil::Opcode::AND, masked, {shifted, ignil::Const{1, w}}, addr});
    auto bit = newTemp(tmp, 1);
    out.append(
            {ignil::Opcode::NEQ, bit, {masked, ignil::Const{0, w}}, addr});
    out.append({ignil::Opcode::COPY, regCF_, {bit}, addr});
  };

  std::map<X86Flag, FlagSetter> flags{
          {X86Flag::CF, cfLastBitOut},
          {X86Flag::PF, pfParity(result, addr)},
          {X86Flag::AF, afAlwaysZero(addr)},
          {X86Flag::ZF, zfEqZero(result, addr)},
          {X86Flag::SF, sfSignBit(result, addr)},
  };
  // OF only architecturally defined for a 1-bit shift: whether the sign
  // bit changed (SHL, unlike SAR/SHR, can overflow in either direction).
  if (imm == 1)
    flags.emplace(X86Flag::OF, [this, lhs, result,
                                addr](ignil::Block &out, std::uint32_t &tmp) {
      auto lhsNeg = newTemp(tmp, 1);
      out.append({ignil::Opcode::SLT,
                  lhsNeg,
                  {ignil::Value{lhs}, ignil::Const{0, lhs.width}},
                  addr});
      auto resNeg = newTemp(tmp, 1);
      out.append({ignil::Opcode::SLT,
                  resNeg,
                  {result, ignil::Const{0, ignil::widthOf(result)}},
                  addr});
      auto of = newTemp(tmp, 1);
      out.append({ignil::Opcode::NEQ, of, {lhsNeg, resNeg}, addr});
      out.append({ignil::Opcode::COPY, regOF_, {of}, addr});
    });
  emitArithFlags(addr, out, tmp, flags);
}

void dragon::x86::X86IGNILLifter::liftShrImm(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftShrImm in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftShrImm in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  const auto imm = static_cast<std::uint64_t>(mc.getOperand(2).getImm());

  if (imm == 0) {
    out.append({ignil::Opcode::COPY, *dst, {*src}, addr});
    return;
  }

  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*src}, addr});

  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::SHR,
              result,
              {ignil::Value{lhs}, ignil::Const{imm, dst->width}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  // CF = bit imm-1 of the original value - same formula shape as SAR.
  auto cfLastBitOut = [this, lhs, imm,
                       addr](ignil::Block &out, std::uint32_t &tmp) {
    const ignil::Width w = lhs.width;
    auto shifted = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR,
                shifted,
                {ignil::Value{lhs}, ignil::Const{imm - 1, w}},
                addr});
    auto masked = newTemp(tmp, w);
    out.append(
            {ignil::Opcode::AND, masked, {shifted, ignil::Const{1, w}}, addr});
    auto bit = newTemp(tmp, 1);
    out.append(
            {ignil::Opcode::NEQ, bit, {masked, ignil::Const{0, w}}, addr});
    out.append({ignil::Opcode::COPY, regCF_, {bit}, addr});
  };

  std::map<X86Flag, FlagSetter> flags{
          {X86Flag::CF, cfLastBitOut},
          {X86Flag::PF, pfParity(result, addr)},
          {X86Flag::AF, afAlwaysZero(addr)},
          {X86Flag::ZF, zfEqZero(result, addr)},
          {X86Flag::SF, sfSignBit(result, addr)},
  };
  // OF only architecturally defined for a 1-bit shift: a logical shift
  // right always clears the new sign bit, so "did the sign appear to
  // change" reduces to whatever the original sign bit was.
  if (imm == 1)
    flags.emplace(X86Flag::OF, [this, lhs,
                                addr](ignil::Block &out, std::uint32_t &tmp) {
      auto of = newTemp(tmp, 1);
      out.append({ignil::Opcode::SLT,
                  of,
                  {ignil::Value{lhs}, ignil::Const{0, lhs.width}},
                  addr});
      out.append({ignil::Opcode::COPY, regOF_, {of}, addr});
    });
  emitArithFlags(addr, out, tmp, flags);
}

void dragon::x86::X86IGNILLifter::liftSarR1(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src(tied=dst) - no imm operand at all, the shift
  // count of 1 is implicit in the opcode. Synthesize that operand and
  // delegate to liftSarImm so the flag formulas live in exactly one place.
  llvm::MCInst synthetic = mc;
  synthetic.addOperand(llvm::MCOperand::createImm(1));
  liftSarImm(synthetic, addr, out, tmp);
}

void dragon::x86::X86IGNILLifter::liftShlR1(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  llvm::MCInst synthetic = mc;
  synthetic.addOperand(llvm::MCOperand::createImm(1));
  liftShlImm(synthetic, addr, out, tmp);
}

void dragon::x86::X86IGNILLifter::liftShrR1(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  llvm::MCInst synthetic = mc;
  synthetic.addOperand(llvm::MCOperand::createImm(1));
  liftShrImm(synthetic, addr, out, tmp);
}

void dragon::x86::X86IGNILLifter::liftCvtsi2sd(const llvm::MCInst &mc,
                                               std::uint64_t addr,
                                               ignil::Block &out,
                                               std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftCvtsi2sd in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftCvtsi2sd in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::SITOFP, *dst, {*src}, addr});
}

void dragon::x86::X86IGNILLifter::liftCvttsd2si(const llvm::MCInst &mc,
                                                std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftCvttsd2si in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftCvttsd2si in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::FPTOSI, *dst, {*src}, addr});
}

void dragon::x86::X86IGNILLifter::liftAddsd(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftAddsd in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftAddsd in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::FADD, *dst, {*dst, *src}, addr});
}

void dragon::x86::X86IGNILLifter::liftSubsd(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftSubsd in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftSubsd in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::FSUB, *dst, {*dst, *src}, addr});
}

void dragon::x86::X86IGNILLifter::liftMulsd(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftMulsd in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftMulsd in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::FMUL, *dst, {*dst, *src}, addr});
}

void dragon::x86::X86IGNILLifter::liftMulsdMem(const llvm::MCInst &mc,
                                               std::uint64_t addr,
                                               ignil::Block &out,
                                               std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=tied(=dst, ignored), op[2..6]=mem(rhs) - mirrors
  // liftAddRegMem's shape.
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftMulsdMem in addr {:#x} no dst", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 2, ptrWidth, addr, out, tmp);
  auto rhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::LOAD, rhs, {addrTemp}, addr});
  out.append({ignil::Opcode::FMUL, *dst, {*dst, rhs}, addr});
}

void dragon::x86::X86IGNILLifter::liftMovsdRR(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftMovsdRR in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(2).getReg());
  if (!src) {
    logger_.log("Error liftMovsdRR in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  out.append({ignil::Opcode::COPY, *dst, {*src}, addr});
}

std::optional<dragon::ignil::Reg>
dragon::x86::X86IGNILLifter::toReg(unsigned llvmReg) const {
  auto it = regMap_.find(llvmReg);
  if (it == regMap_.end())
    return std::nullopt;
  return it->second;
}

dragon::ignil::Temp
dragon::x86::X86IGNILLifter::newTemp(std::uint32_t &tempId,
                                     ignil::Width width) const {
  return ignil::Temp{tempId++, width};
}

dragon::ignil::Temp dragon::x86::X86IGNILLifter::emitAddress(
        const llvm::MCInst &mc, unsigned opStart, ignil::Width ptrWidth,
        std::uint64_t addr, ignil::Block &out, std::uint32_t &tmp) const {
  // x86 memory operand layout: base, scale, index, disp, seg
  auto base = toReg(mc.getOperand(opStart + 0).getReg());
  const auto scale = static_cast<unsigned>(mc.getOperand(opStart + 1).getImm());
  auto index = toReg(mc.getOperand(opStart + 2).getReg());
  const std::int64_t disp = mc.getOperand(opStart + 3).getImm();

  // Accumulate into temps. Start with base or zero.
  auto result = newTemp(tmp, ptrWidth);
  if (base)
    out.append({ignil::Opcode::COPY, result, {*base}, addr});
  else
    out.append(
            {ignil::Opcode::COPY, result, {ignil::Const{0, ptrWidth}}, addr});

  // Add index*scale if an index register is present.
  if (index) {
    auto term = newTemp(tmp, ptrWidth);
    if (scale > 1) {
      // scale is always a power of two (1/2/4/8); represent as SHL.
      const ignil::Width shift = (scale == 2) ? 1u : (scale == 4) ? 2u : 3u;
      out.append({ignil::Opcode::SHL,
                  term,
                  {*index, ignil::Const{shift, 8}},
                  addr});
    } else {
      out.append({ignil::Opcode::COPY, term, {*index}, addr});
    }
    auto added = newTemp(tmp, ptrWidth);
    out.append({ignil::Opcode::ADD, added, {result, term}, addr});
    result = added;
  }

  // Add displacement if non-zero.
  if (disp != 0) {
    auto withDisp = newTemp(tmp, ptrWidth);
    out.append(
            {ignil::Opcode::ADD,
             withDisp,
             {result, ignil::Const{static_cast<std::uint64_t>(disp), ptrWidth}},
             addr});
    result = withDisp;
  }

  return result;
}

dragon::ignil::Temp dragon::x86::X86IGNILLifter::emitCondition(
        unsigned condCode, std::uint64_t addr, ignil::Block &out,
        std::uint32_t &tmp) const {
  // LLVM x86 CondCode enum (X86BaseInfo.h):
  //   0=O,1=NO,2=B,3=AE,4=E,5=NE,6=BE,7=A,8=S,9=NS,10=P,11=NP,12=L,13=GE,
  //   14=LE,15=G
  auto flagSet = [&](ignil::Reg flag) -> ignil::Temp {
    auto t = newTemp(tmp, 8);
    out.append({ignil::Opcode::COPY, t, {flag}, addr});
    auto cond = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, cond, {t, ignil::Const{0, 8}}, addr});
    return cond;
  };
  auto flagClear = [&](ignil::Reg flag) -> ignil::Temp {
    auto t = newTemp(tmp, 8);
    out.append({ignil::Opcode::COPY, t, {flag}, addr});
    auto cond = newTemp(tmp, 1);
    out.append({ignil::Opcode::EQ, cond, {t, ignil::Const{0, 8}}, addr});
    return cond;
  };

  switch (condCode) {
    case 0:
      return flagSet(regOF_); // O  / JO  : OF=1
    case 1:
      return flagClear(regOF_); // NO / JNO : OF=0
    case 10:
      return flagSet(regPF_); // P  / JP  : PF=1
    case 11:
      return flagClear(regPF_); // NP / JNP : PF=0
    case 2:
      return flagSet(regCF_); // B  / JB  : CF=1
    case 3:
      return flagClear(regCF_); // AE / JAE : CF=0
    case 4:
      return flagSet(regZF_); // E  / JE  : ZF=1
    case 5:
      return flagClear(regZF_); // NE / JNE : ZF=0
    case 8:
      return flagSet(regSF_); // S  / JS  : SF=1
    case 9:
      return flagClear(regSF_); // NS / JNS : SF=0
    case 6: { // BE / JBE : CF=1 OR ZF=1
      auto cf = flagSet(regCF_), zf = flagSet(regZF_);
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::OR, cond, {cf, zf}, addr});
      return cond;
    }
    case 7: { // A  / JA  : CF=0 AND ZF=0
      auto cfClr = flagClear(regCF_), zfClr = flagClear(regZF_);
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::AND, cond, {cfClr, zfClr}, addr});
      return cond;
    }
    case 12: { // L  / JL  : SF != OF
      auto sf = newTemp(tmp, 8), of = newTemp(tmp, 8);
      out.append({ignil::Opcode::COPY, sf, {regSF_}, addr});
      out.append({ignil::Opcode::COPY, of, {regOF_}, addr});
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::NEQ, cond, {sf, of}, addr});
      return cond;
    }
    case 13: { // GE / JGE : SF == OF
      auto sf = newTemp(tmp, 8), of = newTemp(tmp, 8);
      out.append({ignil::Opcode::COPY, sf, {regSF_}, addr});
      out.append({ignil::Opcode::COPY, of, {regOF_}, addr});
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::EQ, cond, {sf, of}, addr});
      return cond;
    }
    case 14: { // LE / JLE : ZF=1 OR (SF != OF)
      auto zf = flagSet(regZF_);
      auto sf = newTemp(tmp, 8), of = newTemp(tmp, 8);
      out.append({ignil::Opcode::COPY, sf, {regSF_}, addr});
      out.append({ignil::Opcode::COPY, of, {regOF_}, addr});
      auto sfNeOf = newTemp(tmp, 1);
      out.append({ignil::Opcode::NEQ, sfNeOf, {sf, of}, addr});
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::OR, cond, {zf, sfNeOf}, addr});
      return cond;
    }
    case 15: { // G  / JG  : ZF=0 AND SF == OF
      auto zfClr = flagClear(regZF_);
      auto sf = newTemp(tmp, 8), of = newTemp(tmp, 8);
      out.append({ignil::Opcode::COPY, sf, {regSF_}, addr});
      out.append({ignil::Opcode::COPY, of, {regOF_}, addr});
      auto sfEqOf = newTemp(tmp, 1);
      out.append({ignil::Opcode::EQ, sfEqOf, {sf, of}, addr});
      auto cond = newTemp(tmp, 1);
      out.append({ignil::Opcode::AND, cond, {zfClr, sfEqOf}, addr});
      return cond;
    }
    default: {
      auto undef = newTemp(tmp, 1);
      out.append({ignil::Opcode::UNDEF, undef, {}, addr});
      return undef;
    }
  }
}

void dragon::x86::X86IGNILLifter::emitUndef(ignil::Reg dst, std::uint64_t addr,
                                            ignil::Block &out) const {
  ignil::Op op;
  op.opcode = ignil::Opcode::UNDEF;
  op.dst = dst;
  op.address = addr;
  out.append(std::move(op));
}

void dragon::x86::X86IGNILLifter::emitArithFlags(
        std::uint64_t addr, ignil::Block &out, std::uint32_t &tmp,
        const std::map<X86Flag, FlagSetter> &flagMap) const {
  const std::array<std::pair<X86Flag, ignil::Reg>, 7> allFlags = {{
          {X86Flag::CF, regCF_},
          {X86Flag::PF, regPF_},
          {X86Flag::AF, regAF_},
          {X86Flag::ZF, regZF_},
          {X86Flag::SF, regSF_},
          {X86Flag::OF, regOF_},
          {X86Flag::DF, regDF_},
  }};

  for (const auto &[flag, reg]: allFlags) {
    auto it = flagMap.find(flag);
    // No arithmetic/logic instruction touches DF (only STD/CLD/POPF do), so
    // an absent DF setter means "unchanged", not "undefined".
    if (flag == X86Flag::DF && it == flagMap.end())
      continue;
    if (it != flagMap.end())
      it->second(out, tmp);
    else
      emitUndef(reg, addr, out);
  }
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::zfEqZero(ignil::Value value,
                                      std::uint64_t addr) const {
  return [this, value, addr](ignil::Block &out, std::uint32_t &tmp) {
    auto t = newTemp(tmp, 1);
    out.append({ignil::Opcode::EQ,
                t,
                {value, ignil::Const{0, ignil::widthOf(value)}},
                addr});
    out.append({ignil::Opcode::COPY, regZF_, {t}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::sfSignBit(ignil::Value value,
                                       std::uint64_t addr) const {
  return [this, value, addr](ignil::Block &out, std::uint32_t &tmp) {
    auto t = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT,
                t,
                {value, ignil::Const{0, ignil::widthOf(value)}},
                addr});
    out.append({ignil::Opcode::COPY, regSF_, {t}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::cfAlwaysZero(std::uint64_t addr) const {
  return [this, addr](ignil::Block &out, std::uint32_t &) {
    out.append({ignil::Opcode::COPY, regCF_, {ignil::Const{0, 1}}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::ofAlwaysZero(std::uint64_t addr) const {
  return [this, addr](ignil::Block &out, std::uint32_t &) {
    out.append({ignil::Opcode::COPY, regOF_, {ignil::Const{0, 1}}, addr});
  };
}

dragon::x86::FlagSetter dragon::x86::X86IGNILLifter::cfAddOverflow(
        ignil::Value result, ignil::Value addend, std::uint64_t addr) const {
  // Unsigned overflow: result wrapped around  ⟺  result < addend (unsigned)
  return [this, result, addend, addr](ignil::Block &out, std::uint32_t &tmp) {
    auto t = newTemp(tmp, 1);
    out.append({ignil::Opcode::ULT, t, {result, addend}, addr});
    out.append({ignil::Opcode::COPY, regCF_, {t}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::cfSubBorrow(ignil::Value lhs, ignil::Value rhs,
                                         std::uint64_t addr) const {
  // Unsigned borrow: subtraction required borrow  ⟺  lhs < rhs (unsigned)
  return [this, lhs, rhs, addr](ignil::Block &out, std::uint32_t &tmp) {
    auto t = newTemp(tmp, 1);
    out.append({ignil::Opcode::ULT, t, {lhs, rhs}, addr});
    out.append({ignil::Opcode::COPY, regCF_, {t}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::ofAddOverflow(ignil::Value lhs, ignil::Value rhs,
                                           ignil::Value result,
                                           std::uint64_t addr) const {
  // Signed overflow when both inputs have the same sign but the result has the
  // opposite sign: OF = (sign(lhs) == sign(rhs)) AND (sign(result) !=
  // sign(lhs))
  return [this, lhs, rhs, result, addr](ignil::Block &out, std::uint32_t &tmp) {
    ignil::Const zero{0, ignil::widthOf(lhs)};
    auto sa = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sa, {lhs, zero}, addr});
    auto sb = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sb, {rhs, zero}, addr});
    auto sr = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sr, {result, zero}, addr});
    auto sameSign = newTemp(tmp, 1);
    out.append({ignil::Opcode::EQ, sameSign, {sa, sb}, addr});
    auto diffSign = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, diffSign, {sr, sa}, addr});
    auto of = newTemp(tmp, 1);
    out.append({ignil::Opcode::AND, of, {sameSign, diffSign}, addr});
    out.append({ignil::Opcode::COPY, regOF_, {of}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::ofSubOverflow(ignil::Value lhs, ignil::Value rhs,
                                           ignil::Value result,
                                           std::uint64_t addr) const {
  // Signed overflow when inputs have different signs and result sign differs
  // from lhs: OF = (sign(lhs) != sign(rhs)) AND (sign(result) != sign(lhs))
  return [this, lhs, rhs, result, addr](ignil::Block &out, std::uint32_t &tmp) {
    ignil::Const zero{0, ignil::widthOf(lhs)};
    auto sa = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sa, {lhs, zero}, addr});
    auto sb = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sb, {rhs, zero}, addr});
    auto sr = newTemp(tmp, 1);
    out.append({ignil::Opcode::SLT, sr, {result, zero}, addr});
    auto diffAB = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, diffAB, {sa, sb}, addr});
    auto diffRA = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, diffRA, {sr, sa}, addr});
    auto of = newTemp(tmp, 1);
    out.append({ignil::Opcode::AND, of, {diffAB, diffRA}, addr});
    out.append({ignil::Opcode::COPY, regOF_, {of}, addr});
  };
}

// -----------------------------------------------------------------------
// LifterGaps.md #7 - forms VMProtect handlers lean on: NOT, shifts by CL,
// 8/16-bit shifts (via the existing imm/r1 lifters), NEG8/16 (existing
// liftNeg), accumulator sign-extends, STC/CLC/CMC.
// -----------------------------------------------------------------------

void dragon::x86::X86IGNILLifter::liftNot(const llvm::MCInst &mc,
                                          std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftNot in addr {:#x} no dst", addr);
    return;
  }
  auto src = toReg(mc.getOperand(1).getReg());
  if (!src) {
    logger_.log("Error liftNot in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  auto lhs = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::COPY, lhs, {*src}, addr});
  auto result = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::NOT, result, {ignil::Value{lhs}}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});
}

void dragon::x86::X86IGNILLifter::liftShiftCL(
        const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
        std::uint32_t &tmp, ignil::Opcode shift,
        std::optional<ignil::Width> memWidth) const {
  std::optional<ignil::Reg> dst, src;
  std::optional<ignil::Temp> memAddr;
  if (memWidth) {
    const ignil::Width ptrWidth =
            (architecture_ == Architecture::X86_64) ? 64 : 32;
    memAddr = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  } else {
    dst = toReg(mc.getOperand(0).getReg());
    if (!dst) {
      logger_.log("Error liftShiftCL in addr {:#x} no dst", addr);
      return;
    }
    src = toReg(mc.getOperand(1).getReg());
    if (!src) {
      logger_.log("Error liftShiftCL in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      return;
    }
  }
  const ignil::Width w = memWidth ? *memWidth : dst->width;
  const std::uint64_t W = w;
  const std::uint64_t countMask = (w == 64) ? 63 : 31;

  auto lhs = newTemp(tmp, w);
  if (memAddr)
    out.append({ignil::Opcode::LOAD, lhs, {*memAddr}, addr});
  else
    out.append({ignil::Opcode::COPY, lhs, {*src}, addr});
  auto cnt8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND,
              cnt8,
              {regCL_, ignil::Const{countMask, 8}},
              addr});
  auto cnt = newTemp(tmp, w);
  if (w == 8)
    out.append({ignil::Opcode::COPY, cnt, {ignil::Value{cnt8}}, addr});
  else
    out.append({ignil::Opcode::ZEXT, cnt, {ignil::Value{cnt8}}, addr});
  auto result = newTemp(tmp, w);
  out.append({shift, result, {ignil::Value{lhs}, ignil::Value{cnt}}, addr});
  if (memAddr)
    out.append({ignil::Opcode::STORE, std::nullopt, {*memAddr, result}, addr});
  else
    out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  emitGatedShiftFlags(addr, out, tmp, lhs, cnt, result,
                      shift == ignil::Opcode::SHL);
}

void dragon::x86::X86IGNILLifter::liftAccExtend(std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp,
                                                AccExtend kind) const {
  auto signFill = [&](ignil::Reg from, ignil::Reg to, std::uint64_t bits) {
    auto t = newTemp(tmp, from.width);
    out.append({ignil::Opcode::COPY, t, {from}, addr});
    auto r = newTemp(tmp, from.width);
    out.append({ignil::Opcode::SAR,
                r,
                {ignil::Value{t}, ignil::Const{bits, from.width}},
                addr});
    out.append({ignil::Opcode::COPY, to, {ignil::Value{r}}, addr});
  };
  switch (kind) {
    case AccExtend::CBW:
      out.append({ignil::Opcode::SEXT, regAX_, {regAL_}, addr});
      return;
    case AccExtend::CWDE:
      out.append({ignil::Opcode::SEXT, regEAX_, {regAX_}, addr});
      return;
    case AccExtend::CDQE:
      out.append({ignil::Opcode::SEXT, regRAX_, {regEAX_}, addr});
      return;
    case AccExtend::CWD:
      signFill(regAX_, regDX_, 15);
      return;
    case AccExtend::CDQ:
      signFill(regEAX_, regEDX_, 31);
      return;
    case AccExtend::CQO:
      signFill(regRAX_, regRDX_, 63);
      return;
  }
}

void dragon::x86::X86IGNILLifter::liftOpaque(std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp,
                                             ignil::OpaqueKind kind) const {
  // Snapshot the inputs first: CPUID reads EAX/ECX and also defines them, so
  // every result must be a function of the values on entry rather than of
  // whatever an earlier result op has just stored.
  std::vector<ignil::Value> srcs;
  auto snapshot = [&](const ignil::Reg &r) {
    auto t = newTemp(tmp, r.width);
    out.append({ignil::Opcode::COPY, t, {r}, addr});
    srcs.push_back(ignil::Value{t});
  };

  std::vector<ignil::Reg> results;
  switch (kind) {
    case ignil::OpaqueKind::CPUID:
      snapshot(regEAX_); // leaf
      snapshot(regECX_); // subleaf
      results = {regEAX_, regEBX_, regECX_, regEDX_};
      break;
    case ignil::OpaqueKind::RDTSC:
      results = {regEAX_, regEDX_}; // EDX:EAX
      break;
    case ignil::OpaqueKind::RDTSCP:
      results = {regEAX_, regEDX_, regECX_}; // EDX:EAX, ECX = processor id
      break;
  }

  for (unsigned i = 0; i < results.size(); ++i)
    out.append({ignil::Opcode::OPAQUE, results[i], srcs, addr,
                ignil::makeOpaqueAux(kind, i)});
}

void dragon::x86::X86IGNILLifter::liftSetcc(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  // op[0]=dst(r8), op[1]=condition code
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftSetcc in addr {:#x} no dst", addr);
    out.noteUnlifted(addr);
    return;
  }
  const auto condCode = static_cast<unsigned>(mc.getOperand(1).getImm());
  auto cond = emitCondition(condCode, addr, out, tmp);
  auto value = newTemp(tmp, dst->width);
  out.append({ignil::Opcode::ZEXT, value, {cond}, addr});
  out.append({ignil::Opcode::COPY, *dst, {value}, addr});
}

void dragon::x86::X86IGNILLifter::liftAdcSbb(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp,
                                             bool isSbb) const {
  // op[0]=dst(out), op[1]=src1(tied=dst), op[2]=src2(reg or imm)
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftAdcSbb in addr {:#x} no dst", addr);
    return;
  }
  const ignil::Width w = dst->width;

  ignil::Value src;
  const auto &srcOp = mc.getOperand(2);
  if (srcOp.isImm()) {
    src = ignil::Const{static_cast<std::uint64_t>(srcOp.getImm()), w};
  } else {
    auto srcReg = toReg(srcOp.getReg());
    if (!srcReg) {
      logger_.log("Error liftAdcSbb in addr {:#x} no src", addr);
      emitUndef(*dst, addr, out);
      emitArithFlags(addr, out, tmp);
      return;
    }
    src = *srcReg;
  }

  // Everything is read before anything is written: `adc rax, rax` and the
  // carry flag itself are both inputs and outputs.
  auto lhs = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});
  auto rhs = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, rhs, {src}, addr});
  auto carryIn = newTemp(tmp, w);
  out.append({w == 8 ? ignil::Opcode::COPY : ignil::Opcode::ZEXT,
              carryIn,
              {regCF_},
              addr});

  const ignil::Opcode step = isSbb ? ignil::Opcode::SUB : ignil::Opcode::ADD;
  auto partial = newTemp(tmp, w);
  out.append({step, partial, {lhs, rhs}, addr});
  auto result = newTemp(tmp, w);
  out.append({step, result, {partial, carryIn}, addr});
  out.append({ignil::Opcode::COPY, *dst, {result}, addr});

  // The carry (borrow) out of either of the two steps. At most one of them can
  // carry, so OR is exact.
  FlagSetter carryOut = [this, lhs, rhs, carryIn, partial, result, isSbb,
                         addr](ignil::Block &block, std::uint32_t &t) {
    auto first = newTemp(t, 1);
    auto second = newTemp(t, 1);
    if (isSbb) {
      block.append({ignil::Opcode::ULT, first, {lhs, rhs}, addr});
      block.append({ignil::Opcode::ULT, second, {partial, carryIn}, addr});
    } else {
      block.append({ignil::Opcode::ULT, first, {partial, lhs}, addr});
      block.append({ignil::Opcode::ULT, second, {result, partial}, addr});
    }
    auto carry = newTemp(t, 1);
    block.append({ignil::Opcode::OR, carry, {first, second}, addr});
    block.append({ignil::Opcode::COPY, regCF_, {carry}, addr});
  };

  // OF and AF are the plain ADD / SUB rules applied to the final result: both
  // are functions of the operand and result sign (or nibble) bits only, which
  // is where a carry-in ends up.
  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, carryOut},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, isSbb ? ofSubOverflow(lhs, rhs, result, addr)
                                             : ofAddOverflow(lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftMulDiv(const llvm::MCInst &mc,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp,
                                             MulDivForm form) const {
  using Kind = MulDivForm::Kind;
  using ignil::Opcode;
  const ignil::Width w = form.width;
  const bool isSigned = form.kind == Kind::Imul || form.kind == Kind::Idiv;
  const bool isDivide = form.kind == Kind::Div || form.kind == Kind::Idiv;

  // The accumulator pair for this width: result low half / quotient, and
  // result high half / remainder. The 8-bit forms use AX = AH:AL.
  const ignil::Reg low = w == 8 ? regAL_ : w == 16 ? regAX_ : w == 32 ? regEAX_ : regRAX_;
  const ignil::Reg high = w == 8 ? regAH_ : w == 16 ? regDX_ : w == 32 ? regEDX_ : regRDX_;

  auto operand = toReg(mc.getOperand(0).getReg());
  if (!operand || low.width == 0 || high.width == 0) {
    logger_.log("Error liftMulDiv in addr {:#x} no operand", addr);
    emitUndef(low, addr, out);
    emitUndef(high, addr, out);
    emitArithFlags(addr, out, tmp);
    out.noteUnlifted(addr);
    return;
  }

  auto binary = [&](Opcode op, ignil::Width width, ignil::Value a, ignil::Value b) {
    auto t = newTemp(tmp, width);
    out.append({op, t, {a, b}, addr});
    return t;
  };
  auto unary = [&](Opcode op, ignil::Width width, ignil::Value a) {
    auto t = newTemp(tmp, width);
    out.append({op, t, {a}, addr});
    return t;
  };
  const Opcode extend = isSigned ? Opcode::SEXT : Opcode::ZEXT;

  // Read every input first: the operand may be the accumulator itself.
  auto src = unary(Opcode::COPY, w, *operand);
  auto acc = unary(Opcode::COPY, w, low);

  if (isDivide) {
    // The dividend is twice the operand width: AH:AL, DX:AX, EDX:EAX, or the
    // 128-bit RDX:RAX. 128-bit temps never meet a 64-bit host fold (the
    // simplifier leaves them alone); Z3 and LLVM handle them natively.
    const ignil::Width wide = static_cast<ignil::Width>(2 * w);
    ignil::Temp dividend = newTemp(tmp, wide);
    if (w == 8) {
      out.append({Opcode::COPY, dividend, {regAX_}, addr});
    } else {
      auto highWide = unary(Opcode::ZEXT, wide, high);
      auto shifted = binary(Opcode::SHL, wide, highWide, ignil::Const{w, 8});
      auto lowWide = unary(Opcode::ZEXT, wide, acc);
      out.append({Opcode::OR, dividend, {shifted, lowWide}, addr});
    }
    auto divisor = unary(extend, wide, src);
    auto quotient = binary(isSigned ? Opcode::SDIV : Opcode::UDIV, wide, dividend, divisor);
    auto remainder = binary(isSigned ? Opcode::SMOD : Opcode::UMOD, wide, dividend, divisor);
    auto quotientLow = unary(Opcode::TRUNC, w, quotient);
    auto remainderLow = unary(Opcode::TRUNC, w, remainder);
    out.append({Opcode::COPY, low, {quotientLow}, addr});
    out.append({Opcode::COPY, high, {remainderLow}, addr});
    emitArithFlags(addr, out, tmp); // all six are undefined after a divide
    return;
  }

  const auto [lowHalf, highHalf] =
          emitWideProduct(acc, src, w, isSigned, addr, out, tmp);
  out.append({Opcode::COPY, low, {lowHalf}, addr});
  out.append({Opcode::COPY, high, {highHalf}, addr});

  emitProductFlags(lowHalf, highHalf, isSigned, w, addr, out, tmp);
}


std::pair<dragon::ignil::Temp, dragon::ignil::Temp>
dragon::x86::X86IGNILLifter::emitWideProduct(ignil::Value acc, ignil::Value src,
                                             ignil::Width w, bool isSigned,
                                             std::uint64_t addr,
                                             ignil::Block &out,
                                             std::uint32_t &tmp) const {
  // The full 2w-bit product of two w-bit values, as (low half, high half).
  // Shared by one-operand MUL/IMUL and the two/three-operand IMUL forms.
  using ignil::Opcode;
  auto binary = [&](Opcode op, ignil::Width width, ignil::Value a, ignil::Value b) {
    auto t = newTemp(tmp, width);
    out.append({op, t, {a, b}, addr});
    return t;
  };
  auto unary = [&](Opcode op, ignil::Width width, ignil::Value a) {
    auto t = newTemp(tmp, width);
    out.append({op, t, {a}, addr});
    return t;
  };
  const Opcode extend = isSigned ? Opcode::SEXT : Opcode::ZEXT;

  ignil::Temp lowHalf = newTemp(tmp, w);
  ignil::Temp highHalf = newTemp(tmp, w);
  if (w < 64) {
    const ignil::Width wide = static_cast<ignil::Width>(2 * w);
    auto product = binary(Opcode::MUL, wide, unary(extend, wide, acc), unary(extend, wide, src));
    out.append({Opcode::TRUNC, lowHalf, {product}, addr});
    auto shifted = binary(Opcode::SHR, wide, product, ignil::Const{w, 8});
    out.append({Opcode::TRUNC, highHalf, {shifted}, addr});
  } else {
    // 64 x 64 -> 128 from 32-bit partial products.
    const ignil::Const mask{0xFFFFFFFFull, 64};
    const ignil::Const thirtyTwo{32, 8};
    auto aLow = binary(Opcode::AND, 64, acc, mask);
    auto aHigh = binary(Opcode::SHR, 64, acc, thirtyTwo);
    auto bLow = binary(Opcode::AND, 64, src, mask);
    auto bHigh = binary(Opcode::SHR, 64, src, thirtyTwo);
    auto ll = binary(Opcode::MUL, 64, aLow, bLow);
    auto lh = binary(Opcode::MUL, 64, aLow, bHigh);
    auto hl = binary(Opcode::MUL, 64, aHigh, bLow);
    auto hh = binary(Opcode::MUL, 64, aHigh, bHigh);
    auto middle = binary(Opcode::ADD, 64,
                         binary(Opcode::ADD, 64, binary(Opcode::SHR, 64, ll, thirtyTwo),
                                binary(Opcode::AND, 64, lh, mask)),
                         binary(Opcode::AND, 64, hl, mask));
    auto unsignedHigh = binary(
            Opcode::ADD, 64,
            binary(Opcode::ADD, 64, hh, binary(Opcode::SHR, 64, lh, thirtyTwo)),
            binary(Opcode::ADD, 64, binary(Opcode::SHR, 64, hl, thirtyTwo),
                   binary(Opcode::SHR, 64, middle, thirtyTwo)));
    out.append({Opcode::MUL, lowHalf, {acc, src}, addr});
    if (isSigned) {
      // signed high = unsigned high - (a < 0 ? b : 0) - (b < 0 ? a : 0)
      const ignil::Const sixtyThree{63, 8};
      auto aNegative = binary(Opcode::SAR, 64, acc, sixtyThree);
      auto bNegative = binary(Opcode::SAR, 64, src, sixtyThree);
      auto corrected = binary(Opcode::SUB, 64,
                              binary(Opcode::SUB, 64, unsignedHigh,
                                     binary(Opcode::AND, 64, aNegative, src)),
                              binary(Opcode::AND, 64, bNegative, acc));
      out.append({Opcode::COPY, highHalf, {corrected}, addr});
    } else {
      out.append({Opcode::COPY, highHalf, {unsignedHigh}, addr});
    }
  }
  return {lowHalf, highHalf};
}

void dragon::x86::X86IGNILLifter::emitProductFlags(ignil::Temp lowHalf,
                                                   ignil::Temp highHalf,
                                                   bool isSigned, ignil::Width w,
                                                   std::uint64_t addr,
                                                   ignil::Block &out,
                                                   std::uint32_t &tmp) const {
  // CF = OF = "the high half is significant": non-zero for MUL, not the sign
  // extension of the low half for IMUL. SF, ZF, AF and PF are undefined.
  FlagSetter significant = [this, lowHalf, highHalf, isSigned, w,
                            addr](ignil::Block &block, std::uint32_t &t) {
    ignil::Value expected = ignil::Const{0, w};
    if (isSigned) {
      auto sign = newTemp(t, w);
      block.append({ignil::Opcode::SAR, sign,
                    {lowHalf, ignil::Const{static_cast<std::uint64_t>(w - 1), 8}}, addr});
      expected = sign;
    }
    auto flag = newTemp(t, 1);
    block.append({ignil::Opcode::NEQ, flag, {highHalf, expected}, addr});
    block.append({ignil::Opcode::COPY, regCF_, {flag}, addr});
    block.append({ignil::Opcode::COPY, regOF_, {flag}, addr});
  };
  emitArithFlags(addr, out, tmp,
                 {{X86Flag::CF, significant},
                  {X86Flag::OF, [](ignil::Block &, std::uint32_t &) {}}});}

void dragon::x86::X86IGNILLifter::liftBt(const llvm::MCInst &mc,
                                         std::uint64_t addr, ignil::Block &out,
                                         std::uint32_t &tmp) const {
  // op[0]=value(reg), op[1]=bit index(reg or imm8)
  auto value = toReg(mc.getOperand(0).getReg());
  if (!value) {
    logger_.log("Error liftBt in addr {:#x} no value", addr);
    emitUndef(regCF_, addr, out);
    return;
  }
  const ignil::Width w = value->width;

  // The index wraps at the operand width for the register forms.
  auto index = newTemp(tmp, w);
  const auto &indexOp = mc.getOperand(1);
  if (indexOp.isImm()) {
    out.append({ignil::Opcode::COPY,
                index,
                {ignil::Const{static_cast<std::uint64_t>(indexOp.getImm()) %
                                      w,
                              w}},
                addr});
  } else {
    auto indexReg = toReg(indexOp.getReg());
    if (!indexReg) {
      logger_.log("Error liftBt in addr {:#x} no index", addr);
      emitUndef(regCF_, addr, out);
      return;
    }
    out.append({ignil::Opcode::AND,
                index,
                {*indexReg, ignil::Const{static_cast<std::uint64_t>(w) - 1, w}},
                addr});
  }

  auto shifted = newTemp(tmp, w);
  out.append({ignil::Opcode::SHR, shifted, {*value, index}, addr});
  auto bit = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, bit, {shifted, ignil::Const{1, w}}, addr});
  auto carry = newTemp(tmp, 8);
  out.append({ignil::Opcode::TRUNC, carry, {bit}, addr});
  out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{carry}}, addr});
}

void dragon::x86::X86IGNILLifter::liftRotate(
        const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
        std::uint32_t &tmp, bool left, ShiftCount count,
        std::optional<ignil::Width> memWidth) const {
  // op[0]=dst(out), op[1]=src(tied=dst), [op[2]=imm8 for the ri form].
  std::optional<ignil::Reg> dst;
  std::optional<ignil::Temp> memAddr;
  if (memWidth) {
    const ignil::Width ptrWidth =
            (architecture_ == Architecture::X86_64) ? 64 : 32;
    memAddr = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  } else {
    dst = toReg(mc.getOperand(0).getReg());
    if (!dst) {
      logger_.log("Error liftRotate in addr {:#x} no dst", addr);
      return;
    }
  }
  const ignil::Width w = memWidth ? *memWidth : dst->width;
  const std::uint64_t W = w;
  const std::uint64_t mask = W - 1;

  auto xv = newTemp(tmp, w);
  if (memAddr)
    out.append({ignil::Opcode::LOAD, xv, {*memAddr}, addr});
  else
    out.append({ignil::Opcode::COPY, xv, {*dst}, addr});

  // The effective rotation amount, masked to W-1. x86 masks the raw count to
  // 5 bits (6 for 64-bit) then takes it mod the width; for every width that
  // is exactly `count & (W-1)`.
  // The flags are gated on the 5/6-bit count alone: an 8/16-bit rotate by a
  // multiple of its width still writes CF.
  const std::uint64_t countMask = (w == 64) ? 63 : 31;
  ignil::Value rawCount = ignil::Const{1, 8};
  auto cnt = newTemp(tmp, w);
  switch (count) {
    case ShiftCount::Imm: {
      const auto imm = static_cast<std::uint64_t>(
              mc.getOperand(memWidth ? 5 : 2).getImm());
      rawCount = ignil::Const{imm & countMask, 8};
      out.append(
              {ignil::Opcode::COPY, cnt, {ignil::Const{imm & mask, w}}, addr});
      break;
    }
    case ShiftCount::One:
      out.append({ignil::Opcode::COPY, cnt, {ignil::Const{1 & mask, w}}, addr});
      break;
    case ShiftCount::CL: {
      auto raw = newTemp(tmp, 8);
      out.append({ignil::Opcode::AND,
                  raw,
                  {regCL_, ignil::Const{countMask, 8}},
                  addr});
      rawCount = raw;
      auto cl = newTemp(tmp, 8);
      out.append({ignil::Opcode::AND, cl, {regCL_, ignil::Const{mask, 8}}, addr});
      if (w == 8)
        out.append({ignil::Opcode::COPY, cnt, {ignil::Value{cl}}, addr});
      else
        out.append({ignil::Opcode::ZEXT, cnt, {ignil::Value{cl}}, addr});
      break;
    }
  }

  // comp = (W - cnt) & (W-1): the complementary shift, masked so a zero count
  // shifts by 0 (not W, which is poison) and the rotate reduces to x | x = x.
  auto comp = newTemp(tmp, w);
  out.append({ignil::Opcode::SUB, comp, {ignil::Const{W, w}, ignil::Value{cnt}},
              addr});
  auto compM = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, compM,
              {ignil::Value{comp}, ignil::Const{mask, w}}, addr});
  auto part1 = newTemp(tmp, w);
  out.append({left ? ignil::Opcode::SHL : ignil::Opcode::SHR, part1,
              {ignil::Value{xv}, ignil::Value{cnt}}, addr});
  auto part2 = newTemp(tmp, w);
  out.append({left ? ignil::Opcode::SHR : ignil::Opcode::SHL, part2,
              {ignil::Value{xv}, ignil::Value{compM}}, addr});
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, result, {ignil::Value{part1}, ignil::Value{part2}},
              addr});
  if (memAddr)
    out.append({ignil::Opcode::STORE,
                std::nullopt,
                {*memAddr, ignil::Value{result}},
                addr});
  else
    out.append({ignil::Opcode::COPY, *dst, {ignil::Value{result}}, addr});

  // CF is the bit rotated past the far end: for ROL it is the new bit 0, for
  // ROR the new bit W-1. A zero count leaves CF (and OF) unchanged, so CF is
  // merged with its old value under an 8-bit mask, as the gated shifts do.
  auto isZero = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ, isZero, {rawCount, ignil::Const{0, 8}}, addr});
  auto z8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::ZEXT, z8, {ignil::Value{isZero}}, addr});
  auto nz8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR, nz8, {ignil::Value{z8}, ignil::Const{1, 8}},
              addr});
  auto shifted = newTemp(tmp, w);
  if (left)
    out.append({ignil::Opcode::COPY, shifted, {ignil::Value{result}}, addr});
  else
    out.append({ignil::Opcode::SHR, shifted,
                {ignil::Value{result}, ignil::Const{mask, w}}, addr});
  auto bit = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, bit, {ignil::Value{shifted}, ignil::Const{1, w}},
              addr});
  auto cfNew8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::TRUNC, cfNew8, {ignil::Value{bit}}, addr});
  auto keep = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, keep, {regCF_, ignil::Value{z8}}, addr});
  auto take = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, take, {ignil::Value{cfNew8}, ignil::Value{nz8}},
              addr});
  auto cf = newTemp(tmp, 8);
  out.append({ignil::Opcode::OR, cf, {ignil::Value{keep}, ignil::Value{take}},
              addr});
  out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cf}}, addr});
  // OF is defined only for a 1-bit rotate (= MSB(result) XOR the bit rotated
  // into CF) and architecturally undefined otherwise. We emit that concrete
  // value, gated like CF, rather than UNDEF: VMProtect folds the whole RFLAGS
  // word (via pushfq) into its VM state, so an UNDEF OF poisons the recovered
  // control flow (same reasoning as the PF/AF modeling). The value is exact
  // for a 1-bit rotate and a deterministic, legal choice for larger counts.
  auto resMSB = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, resMSB, {ignil::Value{result}, ignil::Const{0, w}}, addr});
  // Left rotate: OF = MSB(result) XOR CF(after). Right rotate: OF = the two
  // most-significant bits of the result XOR'd together.
  auto otherBit = newTemp(tmp, 1);
  if (left) {
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{bit}, ignil::Const{0, w}}, addr});
  } else {
    auto sh2 = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR, sh2, {ignil::Value{result}, ignil::Const{W - 2, w}}, addr});
    auto b2 = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, b2, {ignil::Value{sh2}, ignil::Const{1, w}}, addr});
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{b2}, ignil::Const{0, w}}, addr});
  }
  auto ofBit = newTemp(tmp, 1);
  out.append({ignil::Opcode::XOR, ofBit, {ignil::Value{resMSB}, ignil::Value{otherBit}}, addr});
  auto ofNew8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::ZEXT, ofNew8, {ignil::Value{ofBit}}, addr});
  auto keepOf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, keepOf, {regOF_, ignil::Value{z8}}, addr});
  auto takeOf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, takeOf, {ignil::Value{ofNew8}, ignil::Value{nz8}}, addr});
  auto ofMerged = newTemp(tmp, 8);
  out.append({ignil::Opcode::OR, ofMerged, {ignil::Value{keepOf}, ignil::Value{takeOf}}, addr});
  out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{ofMerged}}, addr});
  // SF/ZF/PF/AF are not affected by a rotate: left untouched on purpose.
}

void dragon::x86::X86IGNILLifter::liftRotateCarry(const llvm::MCInst &mc,
                                                  std::uint64_t addr,
                                                  ignil::Block &out,
                                                  std::uint32_t &tmp, bool left,
                                                  ShiftCount count) const {
  // op[0]=dst(out), op[1]=src(tied=dst), [op[2]=imm8].
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftRotateCarry in addr {:#x} no dst", addr);
    return;
  }
  const ignil::Width w = dst->width;
  const std::uint64_t W = w;

  // The count is known at lift time, so reduce it here: mask to 5 bits (6 for
  // 64-bit), then mod (W+1) for the 8/16-bit forms (32/64-bit never exceed W).
  std::uint64_t raw =
          (count == ShiftCount::One)
                  ? 1
                  : static_cast<std::uint64_t>(mc.getOperand(2).getImm());
  std::uint64_t c = raw & ((W == 64) ? 0x3Full : 0x1Full);
  if (W == 8)
    c %= 9;
  else if (W == 16)
    c %= 17;
  if (c == 0) {
    // A zero count changes neither the operand nor any flag. Emit an explicit
    // identity copy rather than nothing, so the instruction is visibly lifted
    // (no ops at all reads as a silently dropped instruction downstream).
    out.append({ignil::Opcode::COPY, *dst, {*dst}, addr});
    return;
  }

  auto xv = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, xv, {*dst}, addr});
  auto cf8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, cf8, {regCF_, ignil::Const{1, 8}}, addr});
  auto cfW = newTemp(tmp, w);
  if (w == 8)
    out.append({ignil::Opcode::COPY, cfW, {ignil::Value{cf8}}, addr});
  else
    out.append({ignil::Opcode::ZEXT, cfW, {ignil::Value{cf8}}, addr});

  auto shiftTerm = [&](ignil::Opcode op, ignil::Value base,
                       std::uint64_t amt) -> ignil::Temp {
    auto t = newTemp(tmp, w);
    out.append({op, t, {base, ignil::Const{amt, w}}, addr});
    return t;
  };
  std::vector<ignil::Temp> terms;
  if (left) {
    // result = (x << c) | (CF << (c-1)) | (x >> (W+1-c))
    if (c < W)
      terms.push_back(shiftTerm(ignil::Opcode::SHL, ignil::Value{xv}, c));
    terms.push_back(shiftTerm(ignil::Opcode::SHL, ignil::Value{cfW}, c - 1));
    if (c > 1)
      terms.push_back(shiftTerm(ignil::Opcode::SHR, ignil::Value{xv}, W + 1 - c));
  } else {
    // result = (x >> c) | (CF << (W-c)) | (x << (W+1-c))
    if (c < W)
      terms.push_back(shiftTerm(ignil::Opcode::SHR, ignil::Value{xv}, c));
    terms.push_back(shiftTerm(ignil::Opcode::SHL, ignil::Value{cfW}, W - c));
    if (c > 1)
      terms.push_back(shiftTerm(ignil::Opcode::SHL, ignil::Value{xv}, W + 1 - c));
  }
  ignil::Temp acc = terms.front();
  for (std::size_t i = 1; i < terms.size(); ++i) {
    auto merged = newTemp(tmp, w);
    out.append({ignil::Opcode::OR, merged, {ignil::Value{acc}, ignil::Value{terms[i]}},
                addr});
    acc = merged;
  }
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{acc}}, addr});

  // New CF = the bit that rotated into the carry: bit (W-c) of x for RCL, bit
  // (c-1) for RCR. Both are in range for 1 <= c <= W.
  const std::uint64_t cfPos = left ? (W - c) : (c - 1);
  auto sh = newTemp(tmp, w);
  out.append({ignil::Opcode::SHR, sh, {ignil::Value{xv}, ignil::Const{cfPos, w}},
              addr});
  auto bit = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, bit, {ignil::Value{sh}, ignil::Const{1, w}}, addr});
  auto cfNew = newTemp(tmp, 8);
  out.append({ignil::Opcode::TRUNC, cfNew, {ignil::Value{bit}}, addr});
  out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cfNew}}, addr});
  // OF (concrete rather than UNDEF, see liftRotate): MSB(result) XOR the new
  // CF. The count here is known to be >= 1, so no zero-count gate is needed.
  auto resMSB = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, resMSB, {ignil::Value{acc}, ignil::Const{0, w}}, addr});
  auto otherBit = newTemp(tmp, 1);
  if (left) {
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{bit}, ignil::Const{0, w}}, addr});
  } else {
    auto sh2 = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR, sh2, {ignil::Value{acc}, ignil::Const{W - 2, w}}, addr});
    auto b2 = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, b2, {ignil::Value{sh2}, ignil::Const{1, w}}, addr});
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{b2}, ignil::Const{0, w}}, addr});
  }
  auto ofBit = newTemp(tmp, 1);
  out.append({ignil::Opcode::XOR, ofBit, {ignil::Value{resMSB}, ignil::Value{otherBit}}, addr});
  out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{ofBit}}, addr});
}

void dragon::x86::X86IGNILLifter::liftRotateCarryCL(const llvm::MCInst &mc,
                                                    std::uint64_t addr,
                                                    ignil::Block &out,
                                                    std::uint32_t &tmp,
                                                    bool left) const {
  // op[0]=dst(out), op[1]=src(tied=dst); the count is CL.
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftRotateCarryCL in addr {:#x} no dst", addr);
    return;
  }
  const ignil::Width w = dst->width;
  const std::uint64_t W = w;
  const std::uint64_t mask = W - 1;

  // Reduce the count exactly as x86 does: mask to 5 bits (6 for 64-bit), then
  // take it mod (W+1) for the 8/16-bit forms (32/64 never exceed W).
  auto cl5 = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, cl5,
              {regCL_, ignil::Const{(W == 64) ? 0x3Full : 0x1Full, 8}}, addr});
  auto c8 = newTemp(tmp, 8);
  if (W == 8)
    out.append({ignil::Opcode::UMOD, c8, {ignil::Value{cl5}, ignil::Const{9, 8}}, addr});
  else if (W == 16)
    out.append({ignil::Opcode::UMOD, c8, {ignil::Value{cl5}, ignil::Const{17, 8}}, addr});
  else
    out.append({ignil::Opcode::COPY, c8, {ignil::Value{cl5}}, addr});
  auto cW = newTemp(tmp, w);
  if (w == 8)
    out.append({ignil::Opcode::COPY, cW, {ignil::Value{c8}}, addr});
  else
    out.append({ignil::Opcode::ZEXT, cW, {ignil::Value{c8}}, addr});

  auto xv = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, xv, {*dst}, addr});
  auto cf8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, cf8, {regCF_, ignil::Const{1, 8}}, addr});
  auto cfW = newTemp(tmp, w);
  if (w == 8)
    out.append({ignil::Opcode::COPY, cfW, {ignil::Value{cf8}}, addr});
  else
    out.append({ignil::Opcode::ZEXT, cfW, {ignil::Value{cf8}}, addr});

  // A variable shift whose amount may be as large as W: mask the amount to
  // W-1 for the shift itself (never poison) and zero the result when the true
  // amount was >= W.
  auto shiftGated = [&](ignil::Opcode op, ignil::Value base,
                        ignil::Temp amt) -> ignil::Temp {
    auto amtM = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, amtM, {ignil::Value{amt}, ignil::Const{mask, w}},
                addr});
    auto raw = newTemp(tmp, w);
    out.append({op, raw, {base, ignil::Value{amtM}}, addr});
    auto lt = newTemp(tmp, 1);
    out.append({ignil::Opcode::ULT, lt, {ignil::Value{amt}, ignil::Const{W, w}}, addr});
    auto ltW = newTemp(tmp, w);
    out.append({ignil::Opcode::ZEXT, ltW, {ignil::Value{lt}}, addr});
    auto gate = newTemp(tmp, w);
    out.append({ignil::Opcode::NEG, gate, {ignil::Value{ltW}}, addr});
    auto res = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, res, {ignil::Value{raw}, ignil::Value{gate}}, addr});
    return res;
  };
  auto maskedShiftConstBase = [&](ignil::Value base, ignil::Temp amt,
                                  ignil::Opcode op) -> ignil::Temp {
    // Plain shift by an amount already known to be in [0, W-1] for c >= 1; the
    // amount is masked anyway so a c == 0 wrap can never poison (its result is
    // discarded by the count != 0 gate below).
    auto amtM = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, amtM, {ignil::Value{amt}, ignil::Const{mask, w}},
                addr});
    auto r = newTemp(tmp, w);
    out.append({op, r, {base, ignil::Value{amtM}}, addr});
    return r;
  };

  auto cm1 = newTemp(tmp, w); // c - 1
  out.append({ignil::Opcode::SUB, cm1, {ignil::Value{cW}, ignil::Const{1, w}}, addr});
  auto wp1mc = newTemp(tmp, w); // W + 1 - c
  out.append({ignil::Opcode::SUB, wp1mc, {ignil::Const{W + 1, w}, ignil::Value{cW}}, addr});
  auto wmc = newTemp(tmp, w); // W - c
  out.append({ignil::Opcode::SUB, wmc, {ignil::Const{W, w}, ignil::Value{cW}}, addr});

  ignil::Temp termA{0, w}, termB{0, w}, termC{0, w}, cfbit{0, w};
  if (left) {
    termA = shiftGated(ignil::Opcode::SHL, ignil::Value{xv}, cW); // x << c
    termB = maskedShiftConstBase(ignil::Value{cfW}, cm1, ignil::Opcode::SHL); // CF << (c-1)
    termC = shiftGated(ignil::Opcode::SHR, ignil::Value{xv}, wp1mc); // x >> (W+1-c)
    cfbit = maskedShiftConstBase(ignil::Value{xv}, wmc, ignil::Opcode::SHR); // bit W-c
  } else {
    termA = shiftGated(ignil::Opcode::SHR, ignil::Value{xv}, cW); // x >> c
    termB = maskedShiftConstBase(ignil::Value{cfW}, wmc, ignil::Opcode::SHL); // CF << (W-c)
    termC = shiftGated(ignil::Opcode::SHL, ignil::Value{xv}, wp1mc); // x << (W+1-c)
    cfbit = maskedShiftConstBase(ignil::Value{xv}, cm1, ignil::Opcode::SHR); // bit c-1
  }
  auto ab = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, ab, {ignil::Value{termA}, ignil::Value{termB}}, addr});
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, result, {ignil::Value{ab}, ignil::Value{termC}}, addr});

  // A zero count changes nothing: final = (c != 0) ? result : x.
  auto cnz = newTemp(tmp, 1);
  out.append({ignil::Opcode::NEQ, cnz, {ignil::Value{cW}, ignil::Const{0, w}}, addr});
  auto cnzW = newTemp(tmp, w);
  out.append({ignil::Opcode::ZEXT, cnzW, {ignil::Value{cnz}}, addr});
  auto gmask = newTemp(tmp, w);
  out.append({ignil::Opcode::NEG, gmask, {ignil::Value{cnzW}}, addr});
  auto gkeep = newTemp(tmp, w);
  out.append({ignil::Opcode::NOT, gkeep, {ignil::Value{gmask}}, addr});
  auto take = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, take, {ignil::Value{result}, ignil::Value{gmask}}, addr});
  auto keep = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, keep, {ignil::Value{xv}, ignil::Value{gkeep}}, addr});
  auto final = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, final, {ignil::Value{take}, ignil::Value{keep}}, addr});
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{final}}, addr});

  // CF = the rotated-in bit (gated by count != 0, like the operand).
  auto cfbit8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::TRUNC, cfbit8, {ignil::Value{cfbit}}, addr});
  auto z8 = newTemp(tmp, 8);
  auto isZero = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ, isZero, {ignil::Value{cW}, ignil::Const{0, w}}, addr});
  out.append({ignil::Opcode::ZEXT, z8, {ignil::Value{isZero}}, addr});
  auto nz8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR, nz8, {ignil::Value{z8}, ignil::Const{1, 8}}, addr});
  auto keepCf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, keepCf, {regCF_, ignil::Value{z8}}, addr});
  auto takeCf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, takeCf, {ignil::Value{cfbit8}, ignil::Value{nz8}}, addr});
  auto cf = newTemp(tmp, 8);
  out.append({ignil::Opcode::OR, cf, {ignil::Value{keepCf}, ignil::Value{takeCf}}, addr});
  out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cf}}, addr});
  // OF (concrete rather than UNDEF, see liftRotate): MSB(result) XOR new CF,
  // gated by count != 0 just like CF.
  auto resMSB = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, resMSB, {ignil::Value{result}, ignil::Const{0, w}}, addr});
  auto otherBit = newTemp(tmp, 1);
  if (left) {
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{cfbit}, ignil::Const{0, w}}, addr});
  } else {
    auto sh2 = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR, sh2, {ignil::Value{result}, ignil::Const{W - 2, w}}, addr});
    auto b2 = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, b2, {ignil::Value{sh2}, ignil::Const{1, w}}, addr});
    out.append({ignil::Opcode::NEQ, otherBit, {ignil::Value{b2}, ignil::Const{0, w}}, addr});
  }
  auto ofBit = newTemp(tmp, 1);
  out.append({ignil::Opcode::XOR, ofBit, {ignil::Value{resMSB}, ignil::Value{otherBit}}, addr});
  auto ofNew8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::ZEXT, ofNew8, {ignil::Value{ofBit}}, addr});
  auto keepOf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, keepOf, {regOF_, ignil::Value{z8}}, addr});
  auto takeOf = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, takeOf, {ignil::Value{ofNew8}, ignil::Value{nz8}}, addr});
  auto ofMerged = newTemp(tmp, 8);
  out.append({ignil::Opcode::OR, ofMerged, {ignil::Value{keepOf}, ignil::Value{takeOf}}, addr});
  out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{ofMerged}}, addr});
}

void dragon::x86::X86IGNILLifter::liftXchgAcc(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  auto reg = toReg(mc.getOperand(0).getReg());
  if (!reg) {
    logger_.log("Error liftXchgAcc in addr {:#x} no reg", addr);
    return;
  }
  const ignil::Reg acc =
          (reg->width == 16) ? regAX_ : (reg->width == 32) ? regEAX_ : regRAX_;
  auto tr = newTemp(tmp, reg->width);
  out.append({ignil::Opcode::COPY, tr, {*reg}, addr});
  auto ta = newTemp(tmp, acc.width);
  out.append({ignil::Opcode::COPY, ta, {acc}, addr});
  out.append({ignil::Opcode::COPY, *reg, {ignil::Value{ta}}, addr});
  out.append({ignil::Opcode::COPY, acc, {ignil::Value{tr}}, addr});
}

void dragon::x86::X86IGNILLifter::liftBitScan(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp,
                                              bool reverse) const {
  // op[0]=dst(out), last operand = source register.
  auto dst = toReg(mc.getOperand(0).getReg());
  auto src = toReg(mc.getOperand(mc.getNumOperands() - 1).getReg());
  if (!dst || !src) {
    logger_.log("Error liftBitScan in addr {:#x} bad operands", addr);
    if (dst)
      emitUndef(*dst, addr, out);
    emitArithFlags(addr, out, tmp);
    return;
  }
  const ignil::Width w = dst->width;
  const unsigned W = w;

  auto srcV = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, srcV, {*src}, addr});

  // dst = index of the lowest (BSF) / highest (BSR) set bit, as an unrolled
  // "last write wins" scan. BSF scans from the high bit down so the lowest set
  // bit is written last; BSR scans from the low bit up.
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, result, {ignil::Const{0, w}}, addr});
  for (unsigned step = 0; step < W; ++step) {
    const unsigned i = reverse ? step : (W - 1 - step);
    auto shifted = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR, shifted, {ignil::Value{srcV}, ignil::Const{i, w}},
                addr});
    auto justBit = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, justBit,
                {ignil::Value{shifted}, ignil::Const{1, w}}, addr});
    auto isOne = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, isOne,
                {ignil::Value{justBit}, ignil::Const{0, w}}, addr});
    auto condW = newTemp(tmp, w);
    out.append({ignil::Opcode::ZEXT, condW, {ignil::Value{isOne}}, addr});
    auto mask = newTemp(tmp, w);
    out.append({ignil::Opcode::NEG, mask, {ignil::Value{condW}}, addr});
    auto keepMask = newTemp(tmp, w);
    out.append({ignil::Opcode::NOT, keepMask, {ignil::Value{mask}}, addr});
    auto take = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, take,
                {ignil::Const{i, w}, ignil::Value{mask}}, addr});
    auto keep = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, keep,
                {ignil::Value{result}, ignil::Value{keepMask}}, addr});
    auto next = newTemp(tmp, w);
    out.append({ignil::Opcode::OR, next, {ignil::Value{take}, ignil::Value{keep}},
                addr});
    result = next;
  }
  // A source of 0 leaves dst undefined (the SDM; AMD and Unicorn keep the old
  // value, which is not what the 64-bit zero-extension rule would give). It
  // used to come out as 0 - a value no processor produces - so `bsr ebx, eax;
  // xor ebx, 31; add ebx, 32` with eax = 0 was a confident 63. Now:
  // dst = (src == 0) ? UNDEF : index.
  auto unknown = newTemp(tmp, w);
  out.append({ignil::Opcode::UNDEF, unknown, {}, addr});
  auto zero = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ, zero, {ignil::Value{srcV}, ignil::Const{0, w}},
              addr});
  auto zeroW = newTemp(tmp, w);
  out.append({ignil::Opcode::ZEXT, zeroW, {ignil::Value{zero}}, addr});
  auto zeroMask = newTemp(tmp, w);
  out.append({ignil::Opcode::NEG, zeroMask, {ignil::Value{zeroW}}, addr});
  auto indexMask = newTemp(tmp, w);
  out.append({ignil::Opcode::NOT, indexMask, {ignil::Value{zeroMask}}, addr});
  auto fromUnknown = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, fromUnknown,
              {ignil::Value{unknown}, ignil::Value{zeroMask}}, addr});
  auto fromIndex = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, fromIndex,
              {ignil::Value{result}, ignil::Value{indexMask}}, addr});
  auto value = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, value,
              {ignil::Value{fromUnknown}, ignil::Value{fromIndex}}, addr});
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{value}}, addr});

  // ZF reflects a zero source; every other flag is architecturally undefined.
  emitArithFlags(addr, out, tmp, {{X86Flag::ZF, zfEqZero(ignil::Value{srcV}, addr)}});
}

void dragon::x86::X86IGNILLifter::liftBitModify(
        const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
        std::uint32_t &tmp, BitOp op,
        std::optional<ignil::Width> memWidth) const {
  // op[0]=dst(out), op[1]=src(tied=dst), op[2]=bit index (reg or imm8).
  std::optional<ignil::Reg> dst;
  std::optional<ignil::Temp> memAddr;
  if (memWidth) {
    const ignil::Width ptrWidth =
            (architecture_ == Architecture::X86_64) ? 64 : 32;
    memAddr = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  } else {
    dst = toReg(mc.getOperand(0).getReg());
    if (!dst) {
      logger_.log("Error liftBitModify in addr {:#x} no dst", addr);
      emitUndef(regCF_, addr, out);
      return;
    }
  }
  const ignil::Width w = memWidth ? *memWidth : dst->width;

  auto value = newTemp(tmp, w);
  if (memAddr)
    out.append({ignil::Opcode::LOAD, value, {*memAddr}, addr});
  else
    out.append({ignil::Opcode::COPY, value, {*dst}, addr});

  // Index wraps at the operand width.
  auto index = newTemp(tmp, w);
  const auto &indexOp = mc.getOperand(memWidth ? 5 : 2);
  if (indexOp.isImm()) {
    out.append({ignil::Opcode::COPY, index,
                {ignil::Const{static_cast<std::uint64_t>(indexOp.getImm()) % w, w}},
                addr});
  } else {
    auto indexReg = toReg(indexOp.getReg());
    if (!indexReg) {
      logger_.log("Error liftBitModify in addr {:#x} no index", addr);
      emitUndef(regCF_, addr, out);
      return;
    }
    out.append({ignil::Opcode::AND, index,
                {*indexReg, ignil::Const{static_cast<std::uint64_t>(w) - 1, w}},
                addr});
  }

  // CF = the selected bit's current value.
  auto shifted = newTemp(tmp, w);
  out.append({ignil::Opcode::SHR, shifted, {ignil::Value{value}, ignil::Value{index}},
              addr});
  auto oldBit = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, oldBit, {ignil::Value{shifted}, ignil::Const{1, w}},
              addr});
  auto cf = newTemp(tmp, 8);
  out.append({ignil::Opcode::TRUNC, cf, {ignil::Value{oldBit}}, addr});
  out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{cf}}, addr});

  // bitmask = 1 << index, then set / clear / complement that bit.
  auto bitmask = newTemp(tmp, w);
  out.append({ignil::Opcode::SHL, bitmask, {ignil::Const{1, w}, ignil::Value{index}},
              addr});
  auto result = newTemp(tmp, w);
  switch (op) {
    case BitOp::Set:
      out.append({ignil::Opcode::OR, result,
                  {ignil::Value{value}, ignil::Value{bitmask}}, addr});
      break;
    case BitOp::Reset: {
      auto inv = newTemp(tmp, w);
      out.append({ignil::Opcode::NOT, inv, {ignil::Value{bitmask}}, addr});
      out.append({ignil::Opcode::AND, result,
                  {ignil::Value{value}, ignil::Value{inv}}, addr});
      break;
    }
    case BitOp::Complement:
      out.append({ignil::Opcode::XOR, result,
                  {ignil::Value{value}, ignil::Value{bitmask}}, addr});
      break;
  }
  if (memAddr)
    out.append({ignil::Opcode::STORE,
                std::nullopt,
                {*memAddr, ignil::Value{result}},
                addr});
  else
    out.append({ignil::Opcode::COPY, *dst, {ignil::Value{result}}, addr});
  // Only CF is defined; the other flags are left as they were.
}

void dragon::x86::X86IGNILLifter::liftCmov(const llvm::MCInst &mc,
                                           std::uint64_t addr, ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // op[0]=dst(out), op[1]=src1(tied=dst, kept value), op[2]=src2(moved-in),
  // op[3]=condition code.
  auto dst = toReg(mc.getOperand(0).getReg());
  auto kept = toReg(mc.getOperand(1).getReg());
  auto taken = toReg(mc.getOperand(2).getReg());
  if (!dst || !kept || !taken) {
    logger_.log("Error liftCmov in addr {:#x} bad operands", addr);
    if (dst)
      emitUndef(*dst, addr, out);
    return;
  }
  const ignil::Width w = dst->width;
  const auto condCode = static_cast<unsigned>(mc.getOperand(3).getImm());
  auto cond = emitCondition(condCode, addr, out, tmp);

  // dst = cond ? taken : kept, as (taken & m) | (kept & ~m), m = all-ones
  // when the condition holds. IGNIL has no select (same trick as
  // liftShiftDouble).
  auto keptV = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, keptV, {*kept}, addr});
  auto takenV = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, takenV, {*taken}, addr});
  auto condW = newTemp(tmp, w);
  out.append({ignil::Opcode::ZEXT, condW, {ignil::Value{cond}}, addr});
  auto mask = newTemp(tmp, w);
  out.append({ignil::Opcode::NEG, mask, {ignil::Value{condW}}, addr});
  auto keepMask = newTemp(tmp, w);
  out.append({ignil::Opcode::NOT, keepMask, {ignil::Value{mask}}, addr});
  auto takePart = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, takePart, {ignil::Value{takenV}, ignil::Value{mask}},
              addr});
  auto keepPart = newTemp(tmp, w);
  out.append({ignil::Opcode::AND, keepPart,
              {ignil::Value{keptV}, ignil::Value{keepMask}}, addr});
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::OR, result, {ignil::Value{takePart}, ignil::Value{keepPart}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{result}}, addr});
  // CMOVcc affects no flags.
}

void dragon::x86::X86IGNILLifter::liftXadd(const llvm::MCInst &mc,
                                           std::uint64_t addr, ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // op[0]=DEST, op[1]=SRC (both registers, both read before either write).
  auto dst = toReg(mc.getOperand(0).getReg());
  auto src = toReg(mc.getOperand(1).getReg());
  if (!dst || !src) {
    logger_.log("Error liftXadd in addr {:#x} bad operands", addr);
    if (dst)
      emitUndef(*dst, addr, out);
    if (src)
      emitUndef(*src, addr, out);
    return;
  }
  const ignil::Width w = dst->width;

  auto lhs = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, lhs, {*dst}, addr});
  auto rhs = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, rhs, {*src}, addr});
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::ADD, result, {ignil::Value{lhs}, ignil::Value{rhs}},
              addr});
  // SRC = old DEST; DEST = DEST + SRC.
  out.append({ignil::Opcode::COPY, *src, {ignil::Value{lhs}}, addr});
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{result}}, addr});

  emitArithFlags(addr, out, tmp,
                 {
                         {X86Flag::CF, cfAddOverflow(result, ignil::Value{rhs}, addr)},
                         {X86Flag::PF, pfParity(result, addr)},
                         {X86Flag::AF, afNibbleCarry(lhs, rhs, result, addr)},
                         {X86Flag::ZF, zfEqZero(result, addr)},
                         {X86Flag::SF, sfSignBit(result, addr)},
                         {X86Flag::OF, ofAddOverflow(lhs, rhs, result, addr)},
                 });
}

void dragon::x86::X86IGNILLifter::liftIncDecMem(const llvm::MCInst &mc,
                                                std::uint64_t addr,
                                                ignil::Block &out,
                                                std::uint32_t &tmp,
                                                bool dec) const {
  // op[0..4]=mem(base,scale,index,disp,seg). Load, +/- 1, store back.
  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  const unsigned opc = mc.getOpcode();
  ignil::Width w = 32;
  if (opc == opInc8m_ || opc == opDec8m_)
    w = 8;
  else if (opc == opInc16m_ || opc == opDec16m_)
    w = 16;
  else if (opc == opInc64m_ || opc == opDec64m_)
    w = 64;

  auto addrTemp = emitAddress(mc, 0, ptrWidth, addr, out, tmp);
  auto lhs = newTemp(tmp, w);
  out.append({ignil::Opcode::LOAD, lhs, {addrTemp}, addr});
  auto result = newTemp(tmp, w);
  out.append({dec ? ignil::Opcode::SUB : ignil::Opcode::ADD, result,
              {ignil::Value{lhs}, ignil::Const{1, w}}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, ignil::Value{result}},
              addr});

  // Exactly the register INC/DEC flag convention: CF deliberately untouched.
  pfParity(result, addr)(out, tmp);
  afNibbleCarry(ignil::Value{lhs}, ignil::Const{1, w}, result, addr)(out, tmp);
  zfEqZero(result, addr)(out, tmp);
  sfSignBit(result, addr)(out, tmp);
  auto lhsNeg = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, lhsNeg, {ignil::Value{lhs}, ignil::Const{0, w}},
              addr});
  auto resNeg = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, resNeg, {ignil::Value{result}, ignil::Const{0, w}},
              addr});
  auto of = newTemp(tmp, 1);
  if (dec) {
    // OF set iff value was the minimum negative (negative -> non-negative).
    auto resNonNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::NOT, resNonNeg, {ignil::Value{resNeg}}, addr});
    out.append({ignil::Opcode::AND, of, {ignil::Value{lhsNeg}, ignil::Value{resNonNeg}},
                addr});
  } else {
    // OF set iff value was the maximum positive (non-negative -> negative).
    auto lhsNonNeg = newTemp(tmp, 1);
    out.append({ignil::Opcode::NOT, lhsNonNeg, {ignil::Value{lhsNeg}}, addr});
    out.append({ignil::Opcode::AND, of, {ignil::Value{lhsNonNeg}, ignil::Value{resNeg}},
                addr});
  }
  out.append({ignil::Opcode::COPY, regOF_, {ignil::Value{of}}, addr});
}

void dragon::x86::X86IGNILLifter::liftLahf(std::uint64_t addr, ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // AH = SF(7):ZF(6):0(5):AF(4):0(3):PF(2):1(1):CF(0).
  auto acc = newTemp(tmp, 8);
  out.append({ignil::Opcode::COPY, acc, {ignil::Const{0x02, 8}}, addr}); // bit 1 reads as 1
  const std::array<std::pair<ignil::Reg, unsigned>, 5> bits = {{
          {regCF_, 0}, {regPF_, 2}, {regAF_, 4}, {regZF_, 6}, {regSF_, 7},
  }};
  for (const auto &[flag, pos]: bits) {
    auto f8 = newTemp(tmp, 8);
    out.append({ignil::Opcode::AND, f8, {flag, ignil::Const{1, 8}}, addr});
    auto placed = newTemp(tmp, 8);
    if (pos == 0)
      out.append({ignil::Opcode::COPY, placed, {ignil::Value{f8}}, addr});
    else
      out.append({ignil::Opcode::SHL, placed, {ignil::Value{f8}, ignil::Const{pos, 8}},
                  addr});
    auto merged = newTemp(tmp, 8);
    out.append({ignil::Opcode::OR, merged, {ignil::Value{acc}, ignil::Value{placed}},
                addr});
    acc = merged;
  }
  out.append({ignil::Opcode::COPY, regAH_, {ignil::Value{acc}}, addr});
}

void dragon::x86::X86IGNILLifter::liftSahf(std::uint64_t addr, ignil::Block &out,
                                           std::uint32_t &tmp) const {
  // Set SF/ZF/AF/PF/CF from the matching bits of AH.
  auto ah = newTemp(tmp, 8);
  out.append({ignil::Opcode::COPY, ah, {regAH_}, addr});
  const std::array<std::pair<ignil::Reg, unsigned>, 5> bits = {{
          {regCF_, 0}, {regPF_, 2}, {regAF_, 4}, {regZF_, 6}, {regSF_, 7},
  }};
  for (const auto &[flag, pos]: bits) {
    auto sh = newTemp(tmp, 8);
    if (pos == 0)
      out.append({ignil::Opcode::COPY, sh, {ignil::Value{ah}}, addr});
    else
      out.append({ignil::Opcode::SHR, sh, {ignil::Value{ah}, ignil::Const{pos, 8}},
                  addr});
    auto bit = newTemp(tmp, 8);
    out.append({ignil::Opcode::AND, bit, {ignil::Value{sh}, ignil::Const{1, 8}}, addr});
    out.append({ignil::Opcode::COPY, flag, {ignil::Value{bit}}, addr});
  }
}

void dragon::x86::X86IGNILLifter::liftCarryOp(std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp,
                                              CarryOp kind) const {
  switch (kind) {
    case CarryOp::STC:
      out.append({ignil::Opcode::COPY, regCF_, {ignil::Const{1, 1}}, addr});
      return;
    case CarryOp::CLC:
      out.append({ignil::Opcode::COPY, regCF_, {ignil::Const{0, 1}}, addr});
      return;
    case CarryOp::CMC: {
      auto t = newTemp(tmp, 8);
      out.append({ignil::Opcode::XOR, t, {regCF_, ignil::Const{1, 8}}, addr});
      out.append({ignil::Opcode::COPY, regCF_, {ignil::Value{t}}, addr});
      return;
    }
    case CarryOp::CLD:
      out.append({ignil::Opcode::COPY, regDF_, {ignil::Const{0, 1}}, addr});
      return;
  }
}

void dragon::x86::X86IGNILLifter::emitGatedShiftFlags(std::uint64_t addr,
                                                      ignil::Block &out,
                                                      std::uint32_t &tmp,
                                                      ignil::Temp lhs,
                                                      ignil::Temp cnt,
                                                      ignil::Temp result,
                                                      bool leftShift) const {
  const ignil::Width w = lhs.width;
  const std::uint64_t W = w;
  // A zero count leaves every flag untouched. IGNIL has no select, so each
  // new flag value is merged with the old one under 8-bit masks:
  //   flag = (old & isZero8) | (new & notZero8)
  auto isZero = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ,
              isZero,
              {ignil::Value{cnt}, ignil::Const{0, w}},
              addr});
  auto z8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::ZEXT, z8, {ignil::Value{isZero}}, addr});
  auto nz8 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR,
              nz8,
              {ignil::Value{z8}, ignil::Const{1, 8}},
              addr});
  auto gate = [&](ignil::Reg flag, ignil::Temp newBit) {
    auto new8 = newTemp(tmp, 8);
    out.append({ignil::Opcode::ZEXT, new8, {ignil::Value{newBit}}, addr});
    auto keep = newTemp(tmp, 8);
    out.append({ignil::Opcode::AND, keep, {flag, ignil::Value{z8}}, addr});
    auto take = newTemp(tmp, 8);
    out.append({ignil::Opcode::AND,
                take,
                {ignil::Value{new8}, ignil::Value{nz8}},
                addr});
    auto merged = newTemp(tmp, 8);
    out.append({ignil::Opcode::OR,
                merged,
                {ignil::Value{keep}, ignil::Value{take}},
                addr});
    out.append({ignil::Opcode::COPY, flag, {ignil::Value{merged}}, addr});
  };

  // CF = the last bit shifted out: bit (count-1) of the original value for
  // a right shift, bit (width-count) for a left shift. The index is masked
  // to width-1 so a zero count (whose CF the gate discards anyway) never
  // produces an out-of-range shift.
  auto rawIdx = newTemp(tmp, w);
  if (leftShift)
    out.append({ignil::Opcode::SUB,
                rawIdx,
                {ignil::Const{W, w}, ignil::Value{cnt}},
                addr});
  else
    out.append({ignil::Opcode::SUB,
                rawIdx,
                {ignil::Value{cnt}, ignil::Const{1, w}},
                addr});
  auto idx = newTemp(tmp, w);
  out.append({ignil::Opcode::AND,
              idx,
              {ignil::Value{rawIdx}, ignil::Const{W - 1, w}},
              addr});
  auto shifted = newTemp(tmp, w);
  out.append({ignil::Opcode::SHR,
              shifted,
              {ignil::Value{lhs}, ignil::Value{idx}},
              addr});
  auto bit = newTemp(tmp, w);
  out.append({ignil::Opcode::AND,
              bit,
              {ignil::Value{shifted}, ignil::Const{1, w}},
              addr});
  auto cf = newTemp(tmp, 1);
  out.append({ignil::Opcode::NEQ,
              cf,
              {ignil::Value{bit}, ignil::Const{0, w}},
              addr});
  gate(regCF_, cf);
  auto zf = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ,
              zf,
              {ignil::Value{result}, ignil::Const{0, w}},
              addr});
  gate(regZF_, zf);
  auto sf = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT,
              sf,
              {ignil::Value{result}, ignil::Const{0, w}},
              addr});
  gate(regSF_, sf);
  // PF from the result and AF cleared, both gated like CF/ZF/SF.
  gate(regPF_, parityBit(ignil::Value{result}, addr, out, tmp));
  auto afZero = newTemp(tmp, 1);
  out.append({ignil::Opcode::COPY, afZero, {ignil::Const{0, 1}}, addr});
  gate(regAF_, afZero);
  // OF for a 1-bit shift is "did the sign bit change", i.e. MSB(lhs) XOR
  // MSB(result) - which is exactly right for SHL (sign flipped), SHR (new MSB
  // is 0, so OF = old MSB) and SAR (MSB preserved, so OF = 0). For counts > 1
  // the ISA leaves OF undefined; we still emit this concrete, deterministic
  // value (gated like the rest) rather than UNDEF, because VMProtect folds the
  // whole RFLAGS word (pushfq) into its VM state and a symbolic OF poisons the
  // recovered control flow - the same reason PF/AF are modeled, not UNDEF.
  auto lhsSign = newTemp(tmp, 1);
  out.append({ignil::Opcode::SLT, lhsSign, {ignil::Value{lhs}, ignil::Const{0, w}}, addr});
  auto ofBit = newTemp(tmp, 1);
  out.append({ignil::Opcode::XOR, ofBit, {ignil::Value{lhsSign}, ignil::Value{sf}}, addr});
  gate(regOF_, ofBit);
}

void dragon::x86::X86IGNILLifter::liftXchg(const llvm::MCInst &mc,
                                           std::uint64_t addr,
                                           ignil::Block &out,
                                           std::uint32_t &tmp) const {
  auto a = toReg(mc.getOperand(0).getReg());
  auto b = toReg(mc.getOperand(1).getReg());
  if (!a || !b) {
    logger_.log("Error liftXchg in addr {:#x} bad operands", addr);
    if (a) emitUndef(*a, addr, out);
    if (b) emitUndef(*b, addr, out);
    return;
  }
  auto ta = newTemp(tmp, a->width);
  out.append({ignil::Opcode::COPY, ta, {*a}, addr});
  auto tb = newTemp(tmp, b->width);
  out.append({ignil::Opcode::COPY, tb, {*b}, addr});
  out.append({ignil::Opcode::COPY, *a, {ignil::Value{tb}}, addr});
  out.append({ignil::Opcode::COPY, *b, {ignil::Value{ta}}, addr});
}

void dragon::x86::X86IGNILLifter::liftXchgMem(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  // op[0]=reg(out), op[1]=reg(tied), op[2..6]=mem. Both are read before the
  // register is written (`xchg [esi], esp`).
  auto reg = toReg(mc.getOperand(0).getReg());
  if (!reg) {
    logger_.log("Error liftXchgMem in addr {:#x} no reg", addr);
    return;
  }

  const ignil::Width ptrWidth =
          (architecture_ == Architecture::X86_64) ? 64 : 32;
  auto addrTemp = emitAddress(mc, 2, ptrWidth, addr, out, tmp);
  auto memValue = newTemp(tmp, reg->width);
  out.append({ignil::Opcode::LOAD, memValue, {addrTemp}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {addrTemp, *reg}, addr});
  out.append({ignil::Opcode::COPY, *reg, {memValue}, addr});
}

void dragon::x86::X86IGNILLifter::liftBswap(const llvm::MCInst &mc,
                                            std::uint64_t addr,
                                            ignil::Block &out,
                                            std::uint32_t &tmp) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftBswap in addr {:#x} no dst", addr);
    return;
  }
  const ignil::Width w = dst->width;
  const std::uint64_t n = w / 8;
  auto v = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, v, {*dst}, addr});
  std::optional<ignil::Temp> acc;
  for (std::uint64_t i = 0; i < n; ++i) {
    // byte i of the source lands at byte n-1-i of the result
    auto sh = newTemp(tmp, w);
    out.append({ignil::Opcode::SHR,
                sh,
                {ignil::Value{v}, ignil::Const{8 * i, w}},
                addr});
    auto byte = newTemp(tmp, w);
    out.append({ignil::Opcode::AND,
                byte,
                {ignil::Value{sh}, ignil::Const{0xff, w}},
                addr});
    auto placed = newTemp(tmp, w);
    out.append({ignil::Opcode::SHL,
                placed,
                {ignil::Value{byte}, ignil::Const{8 * (n - 1 - i), w}},
                addr});
    if (!acc) {
      acc = placed;
    } else {
      auto merged = newTemp(tmp, w);
      out.append({ignil::Opcode::OR,
                  merged,
                  {ignil::Value{*acc}, ignil::Value{placed}},
                  addr});
      acc = merged;
    }
  }
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{*acc}}, addr});
}

void dragon::x86::X86IGNILLifter::liftShiftDouble(const llvm::MCInst &mc,
                                                  std::uint64_t addr,
                                                  ignil::Block &out,
                                                  std::uint32_t &tmp,
                                                  bool left,
                                                  bool byCL) const {
  auto dst = toReg(mc.getOperand(0).getReg());
  if (!dst) {
    logger_.log("Error liftShiftDouble in addr {:#x} no dst", addr);
    return;
  }
  auto src1 = toReg(mc.getOperand(1).getReg());
  auto src2 = toReg(mc.getOperand(2).getReg());
  if (!src1 || !src2) {
    logger_.log("Error liftShiftDouble in addr {:#x} no src", addr);
    emitUndef(*dst, addr, out);
    return;
  }
  const ignil::Width w = dst->width;
  const std::uint64_t W = w;
  const std::uint64_t countMask = (w == 64) ? 63 : 31;

  auto lhs = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, lhs, {*src1}, addr});
  auto other = newTemp(tmp, w);
  out.append({ignil::Opcode::COPY, other, {*src2}, addr});
  auto cnt = newTemp(tmp, w);
  if (byCL) {
    auto cnt8 = newTemp(tmp, 8);
    out.append({ignil::Opcode::AND,
                cnt8,
                {regCL_, ignil::Const{countMask, 8}},
                addr});
    if (w == 8)
      out.append({ignil::Opcode::COPY, cnt, {ignil::Value{cnt8}}, addr});
    else
      out.append({ignil::Opcode::ZEXT, cnt, {ignil::Value{cnt8}}, addr});
  } else {
    const auto imm = static_cast<std::uint64_t>(mc.getOperand(3).getImm()) & countMask;
    out.append({ignil::Opcode::COPY, cnt, {ignil::Const{imm, w}}, addr});
  }
  // shifted = left  ? (lhs << cnt) | (other >> (W - cnt))
  //                 : (lhs >> cnt) | (other << (W - cnt))
  // The complementary count is masked to W-1 so a zero count shifts `other`
  // by 0 instead of by W (which LLVM treats as poison); that partial result
  // is discarded by the mask below anyway.
  auto comp = newTemp(tmp, w);
  out.append({ignil::Opcode::SUB,
              comp,
              {ignil::Const{W, w}, ignil::Value{cnt}},
              addr});
  auto compM = newTemp(tmp, w);
  out.append({ignil::Opcode::AND,
              compM,
              {ignil::Value{comp}, ignil::Const{W - 1, w}},
              addr});
  auto part1 = newTemp(tmp, w);
  out.append({left ? ignil::Opcode::SHL : ignil::Opcode::SHR,
              part1,
              {ignil::Value{lhs}, ignil::Value{cnt}},
              addr});
  auto part2 = newTemp(tmp, w);
  out.append({left ? ignil::Opcode::SHR : ignil::Opcode::SHL,
              part2,
              {ignil::Value{other}, ignil::Value{compM}},
              addr});
  auto shifted = newTemp(tmp, w);
  out.append({ignil::Opcode::OR,
              shifted,
              {ignil::Value{part1}, ignil::Value{part2}},
              addr});
  // result = cnt != 0 ? shifted : lhs, as (shifted & m) | (lhs & ~m) with
  // m = all-ones when cnt != 0.
  auto nz = newTemp(tmp, 1);
  out.append({ignil::Opcode::NEQ,
              nz,
              {ignil::Value{cnt}, ignil::Const{0, w}},
              addr});
  auto nzW = newTemp(tmp, w);
  out.append({ignil::Opcode::ZEXT, nzW, {ignil::Value{nz}}, addr});
  auto mask = newTemp(tmp, w);
  out.append({ignil::Opcode::NEG, mask, {ignil::Value{nzW}}, addr});
  auto keepMask = newTemp(tmp, w);
  out.append({ignil::Opcode::NOT, keepMask, {ignil::Value{mask}}, addr});
  auto take = newTemp(tmp, w);
  out.append({ignil::Opcode::AND,
              take,
              {ignil::Value{shifted}, ignil::Value{mask}},
              addr});
  auto keep = newTemp(tmp, w);
  out.append({ignil::Opcode::AND,
              keep,
              {ignil::Value{lhs}, ignil::Value{keepMask}},
              addr});
  auto result = newTemp(tmp, w);
  out.append({ignil::Opcode::OR,
              result,
              {ignil::Value{take}, ignil::Value{keep}},
              addr});
  out.append({ignil::Opcode::COPY, *dst, {ignil::Value{result}}, addr});
  emitGatedShiftFlags(addr, out, tmp, lhs, cnt, result, left);
}

void dragon::x86::X86IGNILLifter::liftPushImm(const llvm::MCInst &mc,
                                              std::uint64_t addr,
                                              ignil::Block &out,
                                              std::uint32_t &tmp) const {
  const bool is64 = (architecture_ == Architecture::X86_64);
  const ignil::Width w = is64 ? 64 : 32;
  const std::uint64_t bytes = is64 ? 8 : 4;
  ignil::Reg sp = is64 ? regRSP_ : regESP_;
  const auto imm = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(mc.getOperand(0).getImm()));   // sign-extended
  auto newSP = newTemp(tmp, w);
  out.append({ignil::Opcode::SUB, newSP, {sp, ignil::Const{bytes, w}}, addr});
  out.append({ignil::Opcode::COPY, sp, {ignil::Value{newSP}}, addr});
  out.append({ignil::Opcode::STORE, std::nullopt, {sp, ignil::Const{imm & (is64 ? ~0ull : 0xffffffffull), w}}, addr});
}

dragon::ignil::Temp
dragon::x86::X86IGNILLifter::parityBit(ignil::Value result, std::uint64_t addr,
                                       ignil::Block &out,
                                       std::uint32_t &tmp) const {
  // PF is set when the low byte of the result has an even number of 1 bits:
  // fold the byte with three xor/shift steps, PF = (fold & 1) == 0.
  const ignil::Width w = ignil::widthOf(result);
  ignil::Value low = result;
  if (w > 8) {
    auto t = newTemp(tmp, 8);
    out.append({ignil::Opcode::TRUNC, t, {result}, addr});
    low = ignil::Value{t};
  }
  auto x1 = newTemp(tmp, 8);
  out.append({ignil::Opcode::SHR, x1, {low, ignil::Const{4, 8}}, addr});
  auto x2 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR, x2, {low, ignil::Value{x1}}, addr});
  auto x3 = newTemp(tmp, 8);
  out.append({ignil::Opcode::SHR, x3, {ignil::Value{x2}, ignil::Const{2, 8}}, addr});
  auto x4 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR, x4, {ignil::Value{x2}, ignil::Value{x3}}, addr});
  auto x5 = newTemp(tmp, 8);
  out.append({ignil::Opcode::SHR, x5, {ignil::Value{x4}, ignil::Const{1, 8}}, addr});
  auto x6 = newTemp(tmp, 8);
  out.append({ignil::Opcode::XOR, x6, {ignil::Value{x4}, ignil::Value{x5}}, addr});
  auto bit = newTemp(tmp, 8);
  out.append({ignil::Opcode::AND, bit, {ignil::Value{x6}, ignil::Const{1, 8}}, addr});
  auto pf = newTemp(tmp, 1);
  out.append({ignil::Opcode::EQ, pf, {ignil::Value{bit}, ignil::Const{0, 8}}, addr});
  return pf;
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::pfParity(ignil::Value result,
                                      std::uint64_t addr) const {
  return [this, result, addr](ignil::Block &out, std::uint32_t &tmp) {
    auto pf = parityBit(result, addr, out, tmp);
    out.append({ignil::Opcode::COPY, regPF_, {ignil::Value{pf}}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::afNibbleCarry(ignil::Value lhs, ignil::Value rhs,
                                           ignil::Value result,
                                           std::uint64_t addr) const {
  return [this, lhs, rhs, result, addr](ignil::Block &out, std::uint32_t &tmp) {
    // AF = bit 4 of (lhs ^ rhs ^ result): the carry/borrow across bit 3,
    // the same formula for add, sub, inc, dec and neg.
    const ignil::Width w = ignil::widthOf(result);
    auto t1 = newTemp(tmp, w);
    out.append({ignil::Opcode::XOR, t1, {lhs, rhs}, addr});
    auto t2 = newTemp(tmp, w);
    out.append({ignil::Opcode::XOR, t2, {ignil::Value{t1}, result}, addr});
    auto t3 = newTemp(tmp, w);
    out.append({ignil::Opcode::AND, t3, {ignil::Value{t2}, ignil::Const{0x10, w}}, addr});
    auto af = newTemp(tmp, 1);
    out.append({ignil::Opcode::NEQ, af, {ignil::Value{t3}, ignil::Const{0, w}}, addr});
    out.append({ignil::Opcode::COPY, regAF_, {ignil::Value{af}}, addr});
  };
}

dragon::x86::FlagSetter
dragon::x86::X86IGNILLifter::afAlwaysZero(std::uint64_t addr) const {
  return [this, addr](ignil::Block &out, std::uint32_t &) {
    out.append({ignil::Opcode::COPY, regAF_, {ignil::Const{0, 1}}, addr});
  };
}
