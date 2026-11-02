#pragma once

#include <dragon/Architecture.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>

#include "utils/Logger.hpp"

#include <functional>
#include <map>
#include <span>
#include <unordered_map>

namespace dragon {

// Forward declarations, avoid using long headers
class BasicBlock;
class Graph;
class Instruction;

namespace arm {

/// AArch64 NZCV condition flag identifiers, used as keys in the
/// emitArithFlags map.
enum class ARMFlag : std::uint8_t { N, Z, C, V };

/// Callable that appends IGNIL ops for a single flag into a Block.
/// Receives the block and the running temp-id counter by reference.
using FlagSetter = std::function<void(ignil::Block &, std::uint32_t &)>;

/// A lifter for AArch64 (ARM64) machine instructions to IGNIL.
///
/// AArch64 differs from x86 in ways that shape this class:
///  - Data-processing instructions are 3-address (Rd, Rn, Rm/imm), so unlike
///    x86 there is no tied dst/src1 operand to save before overwriting.
///  - CMP/CMN/TST/MOV are true architectural aliases (SUBS/ADDS/ANDS/ORR
///    with Rd=WZR/XZR), not separate opcodes. The lifter exploits this: a
///    write to the zero register is simply skipped, so the generic
///    arithmetic/logic helpers implement the "compare" and "move" forms for
///    free without dedicated liftCmp/liftMov functions.
///  - Only S-suffixed opcodes (ADDS/SUBS/ANDS) touch NZCV; plain ADD/SUB/AND
///    leave flags untouched, so - unlike x86 - most instructions emit no
///    flag ops at all.
///  - The carry flag has the opposite polarity from x86 for subtraction:
///    AArch64 C=1 means "no borrow" (UGE), x86 CF=1 means "borrow" (ULT).
///  - Branch/compare-and-branch/test-and-branch immediates are a signed
///    instruction-count offset (not bytes) from the branch's own address
///    (not address+size like x86), so targets need an explicit x4 scale.
///
/// Construction scans the MCRegisterInfo once to build the LLVM-register
/// integer -> ignil::Reg mapping. Lifting itself is then a pure
/// integer-comparison dispatch with no string work.
/// Not all the instruction set is supported.
class ARMIGNILLifter {
  public:
  ARMIGNILLifter(internal::Logger &logger, Architecture architecture,
                 const llvm::MCInstrInfo &instrInfo,
                 const llvm::MCRegisterInfo &regInfo);

  /// Lift a complete basic block into an ignil::Block.
  [[nodiscard]] ignil::Block liftBlock(const BasicBlock &block,
                                       const Graph &graph) const;

  /// Lift an arbitrary linear sequence of instructions - e.g. a captured
  /// execution trace, not necessarily a single valid basic block or a
  /// coherent CFG - into one flat ignil::Block. See
  /// X86IGNILLifter::liftTrace for the full rationale; identical here.
  [[nodiscard]] ignil::Block
  liftTrace(std::span<const Instruction> trace) const;

  private:
  // Register-offset (LDR/STR *roW, *roX) and unscaled-immediate (LDUR/STUR)
  // general-register loads and stores, by opcode.
  struct MemForm {
    ignil::Width memWidth; // bits transferred
    bool isStore;
    bool signExtend;       // LDRS*: sign-extend the loaded value
    enum class Addr : std::uint8_t { RegW, RegX, Unscaled, Base } addrForm;
  };
  std::unordered_map<unsigned, MemForm> memForms_;

  struct VectorMemory {
    unsigned genericOpcode;
    ignil::Width width;
    bool store;
    bool pair;
  };
  std::unordered_map<unsigned, VectorMemory> vectorMemory_;
  enum class VectorMove : std::uint8_t { Register, Immediate, InsertHigh, ExtractHigh };
  std::unordered_map<unsigned, VectorMove> vectorMoves_;
  enum class LaneKind : std::uint8_t { Immediate, ImmediateMsl, Immediate64,
    DupGpr, DupLane, InsertGpr, InsertLane, Extract, And, Or, Xor, Bic, Orn, Not };
  struct LaneOp { LaneKind kind; ignil::Width lane; ignil::Width vector; bool invert = false; };
  std::unordered_map<unsigned,LaneOp> laneOps_;
  enum class FpKind : std::uint8_t { Add, Sub, Mul, Div, Neg, Abs, Compare,
    FromSigned, FromUnsigned, ToSigned, ToUnsigned, Select, Floor, Ceil, Trunc,
    Sqrt, Extend, Narrow };
  struct FpOp { FpKind kind; ignil::Width width; bool zero = false; };
  std::unordered_map<unsigned,FpOp> fpOps_;

  // Extended-register ADD/SUB (*rx, *rx64), by opcode.
  struct ExtAddSub {
    bool isSub;
    bool setFlags;
  };
  std::unordered_map<unsigned, ExtAddSub> extAddSub_;

  // Other integer data-processing instructions, by opcode (liftIntOp).
  enum class IntKind : std::uint8_t {
    Bic, Bics, Orn, Eon,              // Rd = Rn op ~shift(Rm)
    Ubfm, Sbfm,                       // bitfield move (lsl/lsr/asr #, sxtw, ubfx, ...)
    Bfm,                              // bitfield insert (bfi, bfxil)
    Lslv, Lsrv, Asrv, Rorv,           // shift by register
    Csel, Csinc, Csinv, Csneg,        // conditional select
    Ccmp, Ccmn,                       // conditional compare (reg or imm)
    Madd, Msub,                       // Ra +/- Rn * Rm
    Smaddl, Umaddl, Smsubl, Umsubl,   // Xa +/- ext(Wn) * ext(Wm)
    Smulh, Umulh,                     // high half of a 128-bit product
    Udiv, Sdiv,
    Extr,                             // (Rn:Rm) >> lsb
    Rev, Rev16, Rev32,                // byte reversal
    Clz, Rbit,
  };
  struct IntOp {
    IntKind kind;
    ignil::Width width;
    bool immForm = false; // CCMP/CCMN with an immediate second operand
  };
  std::unordered_map<unsigned, IntOp> intOps_;

  internal::Logger &logger_;

  Architecture architecture_;
  const llvm::MCInstrInfo *instrInfo_; // used for MCInstrDesc flag checks

  /// Lift a single instruction, appending Ops to out.
  /// tempId is the next available temporary index (incremented as needed).
  void liftInstruction(const Instruction &insn, ignil::Block &out,
                       std::uint32_t &tempId) const;

  // Per-opcode-family lifting helpers
  void liftMovz(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                std::uint32_t &tmp) const;
  void liftMovn(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                std::uint32_t &tmp) const;
  void liftMovk(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                std::uint32_t &tmp) const;
  void liftAddSub(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                  std::uint32_t &tmp) const;
  void liftLogic(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                 std::uint32_t &tmp) const;
  void liftLoad(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                std::uint32_t &tmp, ignil::Width widthOverride = 0) const;
  void liftStore(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                 std::uint32_t &tmp, ignil::Width widthOverride = 0) const;
  void liftLoadPair(const llvm::MCInst &mc, std::uint64_t addr,
                    ignil::Block &out, std::uint32_t &tmp, ignil::Width widthOverride = 0) const;
  void liftStorePair(const llvm::MCInst &mc, std::uint64_t addr,
                     ignil::Block &out, std::uint32_t &tmp, ignil::Width widthOverride = 0) const;
  void writeScalar(const ignil::Reg &reg, ignil::Value value,
                   std::uint64_t address, ignil::Block &out, std::uint32_t &tmp) const;
  void liftVectorMove(const llvm::MCInst &mc, VectorMove kind,
                      std::uint64_t address, ignil::Block &out, std::uint32_t &tmp) const;
  void liftLaneOp(const llvm::MCInst &mc, const LaneOp &op,
                  std::uint64_t address, ignil::Block &out, std::uint32_t &tmp) const;
  void liftFpOp(const llvm::MCInst &mc, const FpOp &op,
                std::uint64_t address, ignil::Block &out, std::uint32_t &tmp) const;
  void liftAdr(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
               std::uint32_t &tmp) const;
  void liftBcc(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
               std::uint32_t &tmp) const;
  void liftCbz(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
               std::uint32_t &tmp) const;
  void liftTbz(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
               std::uint32_t &tmp) const;
  // Fallback control-flow lifters, dispatched from MCInstrDesc flags (mirrors
  // x86's generic isBranch()/isConditionalBranch() fallback) rather than
  // cached opcodes, since B/BR share one shape and BL/BLR share another.
  void liftBranch(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                  std::uint32_t &tmp) const;
  void liftCall(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
                std::uint32_t &tmp) const;
  void liftRet(const llvm::MCInst &mc, std::uint64_t addr, ignil::Block &out,
               std::uint32_t &tmp) const;

  // AArch64 instruction with no lifter: UNDEF everything it defines (NZCV
  // included) and record it in Block::unlifted().
  void liftUnknown(const llvm::MCInst &mc, std::uint64_t addr,
                   ignil::Block &out) const;

  // A32 (ARM32): no semantic lifter exists. Emits only the control flow the
  // disassembler decoded (RET/CALL/JMP/CJMP with an UNDEF condition) and
  // records every other value-producing instruction as unlifted.
  void liftA32(const Instruction &insn, ignil::Block &out,
               std::uint32_t &tmp) const;

  // mayStore, minus LLVM's "unmodeled side effects => mayStore" (nop, dmb).
  [[nodiscard]] bool writesMemory(const llvm::MCInstrDesc &desc) const;

  // Helpers
  // Translate an LLVM register integer to an ignil::Reg.
  // Returns nullopt for unknown registers (caller emits UNDEF).
  [[nodiscard]] std::optional<ignil::Reg> toReg(unsigned llvmReg) const;

  // True if reg is XZR/WZR - the AArch64 zero register. Reads of it are
  // architecturally always 0; writes to it are architecturally discarded.
  [[nodiscard]] bool isZeroReg(const ignil::Reg &reg) const;

  // Resolve a source operand to a Value: register operands to XZR/WZR
  // become Const{0}, everything else behaves like toReg/plain immediate.
  [[nodiscard]] ignil::Value toSrcValue(const llvm::MCOperand &op,
                                        ignil::Width width) const;

  // Allocate a fresh temporary
  [[nodiscard]] ignil::Temp newTemp(std::uint32_t &tempId,
                                    ignil::Width width) const;

  // Decode a shifted-register operand pair (Rm, combined shift-type+amount
  // immediate as used by *rs opcodes of ADD/SUB/AND/ORR/EOR) and emit the
  // shift, returning the Temp/Value that holds the shifted value. Emits
  // nothing and returns the plain register value when the shift is zero.
  [[nodiscard]] ignil::Value
  emitShiftedReg(const llvm::MCInst &mc, unsigned regOpIdx, unsigned shiftOpIdx,
                 ignil::Width width, std::uint64_t addr, ignil::Block &out,
                 std::uint32_t &tmp) const;

  // Decode an extended-register operand (Rm, arith-extend immediate as used
  // by ADD/SUB *rx: UXTB/UXTH/UXTW/UXTX/SXTB/SXTH/SXTW/SXTX plus a left
  // shift of 0-4) into a `width`-bit value.
  [[nodiscard]] ignil::Value
  emitExtendedReg(const llvm::MCInst &mc, unsigned regOpIdx, unsigned extOpIdx,
                  ignil::Width width, std::uint64_t addr, ignil::Block &out,
                  std::uint32_t &tmp) const;

  // The integer data-processing instructions described by intOps_.
  void liftIntOp(const llvm::MCInst &mc, const IntOp &op, std::uint64_t addr,
                 ignil::Block &out, std::uint32_t &tmp) const;

  // LDR/STR with a register offset (`[xn, wm, uxtw #s]`, `[xn, xm, lsl #s]`)
  // or an unscaled immediate (LDUR/STUR), described by memForms_.
  void liftMemForm(const llvm::MCInst &mc, const MemForm &form,
                   std::uint64_t addr, ignil::Block &out,
                   std::uint32_t &tmp) const;

  // Decode an AArch64 logical-immediate encoded field (13-bit N:immr:imms
  // packed bitmask, as used by ANDWri/ORRWri/EORWri and S-variants) into its
  // actual integer value. Standard ARM ARM DecodeBitMasks algorithm.
  [[nodiscard]] static std::uint64_t
  decodeLogicalImmediate(std::uint64_t encoded, unsigned regSize);

  // Decode the unsigned-immediate / pre/post-indexed AArch64 memory operand
  // starting at mc.getOperand(opStart) (base register, offset immediate) and
  // emit the arithmetic ops that compute the effective address, returning
  // the Temp that holds it. scale is the element size in bytes implied by
  // the *ui addressing mode (LDRXui scales by 8, LDRWui by 4, LDRBui by 1,
  // ...); pre/post-indexed forms pass scale=1 since their immediate is
  // already byte-granular.
  [[nodiscard]] ignil::Temp emitAddress(const llvm::MCInst &mc,
                                        unsigned opStart, std::int64_t scale,
                                        ignil::Width ptrWidth,
                                        std::uint64_t addr, ignil::Block &out,
                                        std::uint32_t &tmp) const;

  // Emit ops that compute the 1-bit boolean condition for a Bcc/CSel-family
  // instruction from the current NZCV flags. condCode is the AArch64
  // CondCode immediate (0-15, AArch64CC::CondCode enum) found in the
  // instruction's condition operand.
  [[nodiscard]] ignil::Temp emitCondition(unsigned condCode, std::uint64_t addr,
                                          ignil::Block &out,
                                          std::uint32_t &tmp) const;

  // Emit UNDEF for a destination register (conservative for
  // instructions we do not yet lift).
  void emitUndef(ignil::Reg dst, std::uint64_t addr, ignil::Block &out) const;

  // NZCV helpers - iterate every NZCV register; for each flag present in
  // flagMap the corresponding FlagSetter is called. Unlike x86, flags absent
  // from flagMap are left untouched (no Op emitted) rather than UNDEF'd,
  // since non-S AArch64 opcodes architecturally do not affect NZCV at all.
  void emitArithFlags(std::uint64_t addr, ignil::Block &out, std::uint32_t &tmp,
                      const std::map<ARMFlag, FlagSetter> &flagMap) const;

  // FlagSetter factories - each captures the operands it needs and returns a
  // callable suitable for the flagMap passed to emitArithFlags.

  // Z = (value == 0)
  [[nodiscard]] FlagSetter zfEqZero(ignil::Value value,
                                    std::uint64_t addr) const;
  // N = sign bit of value
  [[nodiscard]] FlagSetter nfSignBit(ignil::Value value,
                                     std::uint64_t addr) const;

  // C for ADD(S): unsigned overflow  ->  ULT(result, addend)
  [[nodiscard]] FlagSetter cfAddOverflow(ignil::Value result,
                                         ignil::Value addend,
                                         std::uint64_t addr) const;
  // C for SUB(S)/CMP: NO unsigned borrow  ->  UGE(lhs, rhs). Opposite
  // polarity from x86's cfSubBorrow (ULT) - see class comment.
  [[nodiscard]] FlagSetter cfSubNoBorrow(ignil::Value lhs, ignil::Value rhs,
                                         std::uint64_t addr) const;

  // V for ADD(S): same-sign inputs produce opposite-sign result
  [[nodiscard]] FlagSetter ofAddOverflow(ignil::Value lhs, ignil::Value rhs,
                                         ignil::Value result,
                                         std::uint64_t addr) const;
  // V for SUB(S)/CMP: different-sign inputs, result sign differs from lhs
  [[nodiscard]] FlagSetter ofSubOverflow(ignil::Value lhs, ignil::Value rhs,
                                         ignil::Value result,
                                         std::uint64_t addr) const;

  // C/V for ANDS: logical ops never produce carry/overflow, so both are
  // architecturally hardwired to 0 (unlike ADD(S)/SUB(S) where they're
  // computed from the operands).
  [[nodiscard]] FlagSetter cfAlwaysZero(std::uint64_t addr) const;
  [[nodiscard]] FlagSetter vfAlwaysZero(std::uint64_t addr) const;

  // Cached opcode integers (same approach as ARMInfo/X86IGNILLifter)
  unsigned opMOVZWi_{}, opMOVZXi_{};
  unsigned opMOVNWi_{}, opMOVNXi_{};
  unsigned opMOVKWi_{}, opMOVKXi_{};
  unsigned opADDWrr_{}, opADDXrr_{}, opADDWri_{}, opADDXri_{};
  unsigned opADDWrs_{}, opADDXrs_{};
  unsigned opADDSWrr_{}, opADDSXrr_{}, opADDSWri_{}, opADDSXri_{};
  unsigned opADDSWrs_{}, opADDSXrs_{};
  unsigned opSUBWrr_{}, opSUBXrr_{}, opSUBWri_{}, opSUBXri_{};
  unsigned opSUBWrs_{}, opSUBXrs_{};
  unsigned opSUBSWrr_{}, opSUBSXrr_{}, opSUBSWri_{}, opSUBSXri_{};
  unsigned opSUBSWrs_{}, opSUBSXrs_{};
  unsigned opANDWrr_{}, opANDXrr_{}, opANDWri_{}, opANDXri_{};
  unsigned opANDWrs_{}, opANDXrs_{};
  unsigned opANDSWri_{}, opANDSXri_{}, opANDSWrs_{}, opANDSXrs_{};
  unsigned opORRWrr_{}, opORRXrr_{}, opORRWri_{}, opORRXri_{};
  unsigned opORRWrs_{}, opORRXrs_{};
  unsigned opEORWrr_{}, opEORXrr_{}, opEORWri_{}, opEORXri_{};
  unsigned opEORWrs_{}, opEORXrs_{};
  unsigned opLDRWui_{}, opLDRXui_{}, opLDRBui_{}, opLDRBBui_{};
  unsigned opLDRHui_{}, opLDRHHui_{};
  unsigned opLDRSBWui_{}, opLDRSBXui_{}, opLDRSHWui_{}, opLDRSHXui_{};
  unsigned opLDRSWui_{};
  unsigned opLDRWpre_{}, opLDRWpost_{}, opLDRXpre_{}, opLDRXpost_{};
  unsigned opSTRWui_{}, opSTRXui_{}, opSTRBui_{}, opSTRBBui_{};
  unsigned opSTRHui_{}, opSTRHHui_{};
  unsigned opSTRWpre_{}, opSTRWpost_{}, opSTRXpre_{}, opSTRXpost_{};
  unsigned opLDPWi_{}, opLDPXi_{}, opLDPWpre_{}, opLDPXpre_{};
  unsigned opLDPWpost_{}, opLDPXpost_{};
  unsigned opSTPWi_{}, opSTPXi_{}, opSTPWpre_{}, opSTPXpre_{};
  unsigned opSTPWpost_{}, opSTPXpost_{};
  unsigned opADR_{}, opADRP_{};
  unsigned opBcc_{};
  unsigned opCBZW_{}, opCBZX_{}, opCBNZW_{}, opCBNZX_{};
  unsigned opTBZW_{}, opTBZX_{}, opTBNZW_{}, opTBNZX_{};


  // LLVM register integer -> ignil::Reg mapping
  // Built once in constructor by scanning MCRegisterInfo +
  // the static name->offset table:
  std::unordered_map<unsigned, ignil::Reg> regMap_;

  // Well-known IGNIL Reg objects (generated in the constructor by name).
  ignil::Reg regNF_{}, regZF_{}, regCF_{}, regVF_{};
  ignil::Reg regSP_{}; // needed for LDP/STP pre/post-index and address math
  ignil::Reg regXZR_{}; // the zero register - used by isZeroReg()
  unsigned llvmNZCV_{}; // LLVM register number of NZCV (0 if not found)
};

} // namespace arm
} // namespace dragon
