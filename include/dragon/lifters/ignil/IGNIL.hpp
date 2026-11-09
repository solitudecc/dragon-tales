#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dragon::ignil {

/// Width of a value or operation in bits.
/// Common widths: 1 (flag/boolean), 8, 16, 32, 64.
using Width = std::uint8_t;

// -----------------------------------------------------------------------
// Values - the operands and destinations of IGNIL operations
// -----------------------------------------------------------------------

/// A constant integer value of a given bit width.
/// Signedness is decided by the operation.
struct Const {
  std::uint64_t value;
  Width width;
};

/// An architectural register - these registers are modeled as a
/// location in the register address space. The lifter assigns
/// offsets; IGNIL itself is architecture-agnostic.
///
/// Sub-registers use the same base offset with different size:
///   RAX -> Reg{0, 64}
///   EAX -> Reg{0, 32}
///   AX  -> Reg{0, 16}
///   AL  -> Reg{0,  8}
///   AH  -> Reg{1,  8} (byte 1 of RAX)
struct Reg {
  std::uint32_t offset; // byte offset in the register address space
  Width width; // bit width
  std::string name; // an optional name to improve output
};

/// A maximal (architectural) register: the widest view of one storage slot in
/// the register address space - RAX (never EAX/AX/AL/AH), V0 (never S0/D0/Q0).
///
/// A list of these describes a target's register file, which is what lets an
/// analysis resolve how many bytes a *sub*-register write really touches.
/// That matters for the zero-extend rule: a 32-bit write clears the rest of
/// its parent, and the parent is 8 bytes for x86-64's EAX and AArch64's W0 but
/// 16 bytes for AArch64's S0, whose parent V0 is 128-bit.
///
/// Ready-made lists: dragon::x86::registerFile(), dragon::arm::registerFile().
struct RegExtent {
  std::uint32_t offset; // byte offset of the parent register
  std::uint32_t byteSize; // its size in bytes
};

/// SSA temporary value, the registers need to
/// be loaded into temporary values to be used
/// acting as a memory location.
struct Temp {
  std::uint32_t id;
  Width width;
};

/// Any value usable as operand or destination
/// We use a variant as no expansion will be done
/// in the future.
using Value = std::variant<Const, Reg, Temp>;

/// For any type of the Value type, returns the
/// Width, we can use std::visit
[[nodiscard]] Width widthOf(const Value &v);

/// Opcode for the operations of IGNIL, we need
/// to have operations that provide information
/// about sign of the operands.
enum class Opcode : std::uint8_t {
  // Data movement
  COPY, // dst = srcs[0]
  LOAD, // dst = memory[srcs[0]] (width bytes)
  STORE, // memory[srcs[0]] = srcs[1] (no dst)

  // Arithmetic (sign-agnostic)
  ADD,
  SUB,
  NEG,
  MUL,

  // Arithmetic (sign-dependent)
  SDIV,
  UDIV,
  SMOD,
  UMOD,

  // Bitwise (sign-agnostic)
  AND,
  OR,
  XOR,
  NOT,
  SHL, // logical shift left
  SHR, // logical shift right (zero fill)
  SAR, // arithmetic shift right (sign fill)

  // Width conversion (IGNIL use explicit conversions)
  ZEXT, // zero-extend srcs[0] to dst width
  SEXT, // sign-extend srcs[0] to dst width
  TRUNC, // truncate    srcs[0] to dst width

  // Comparison - always produce a 1-bit result
  EQ, // ==
  NEQ, // !=
  // Unsigned comparison
  ULT, // <
  ULE, // <=
  UGT, // >
  UGE, // >=
  // Signed comparison
  SLT, // <
  SLE, // <=
  SGT, // >
  SGE, // >=

  // Control flow
  JMP, // unconditional - srcs[0] = target address
  CJMP, // srcs[0] = condition (1-bit)
  // srcs[1] = taken address
  // srcs[2] = not-taken address
  CALL, // srcs[0] = target address
  RET, // no operands

  // Symbolic unknown - models unresolvable inputs
  UNDEF,

  // Floating-point arithmetic (IEEE754 double, width always 64). Operands
  // and dst are the *same* raw-bit-pattern Const/Reg/Temp every other
  // opcode uses - floatness is decided by the operation, exactly like
  // SDIV/UDIV already decide signedness for the same underlying bits
  // rather than Value itself carrying a signed/unsigned tag. A lifter
  // reinterprets (bitcasts) a Reg/Temp/Const's bits as a double only when
  // an op here consumes it.
  //
  // Appended after UNDEF (not grouped with ADD/SUB/MUL above, where they
  // would read more naturally) rather than interleaved into the enum:
  // dragon_c.h's dragon_ignil_opcode_t and python/dragon.py's IGNILOpcode
  // both mirror this enum as hardcoded, value-for-value integer constants
  // (checked at compile time for a few spot values via static_assert in
  // dragon_c.cpp, but *not* exhaustively - a mid-enum insertion silently
  // shifts every later value with no compiler error to catch the ones the
  // static_asserts don't happen to cover). New opcodes always go at the
  // end for exactly this reason.
  FADD,
  FSUB,
  FMUL,
  FDIV,

  // Signed int <-> float conversion. Unsigned and binary32 variants are
  // appended below to preserve the existing C/Python opcode values.
  SITOFP, // dst(double, width 64) = (double)(signed)srcs[0]
  FPTOSI, // dst(int) = srcs[0], interpreted as double, truncated toward zero

  // An instruction whose result IGNIL cannot express, but which is still a
  // determinate value rather than an unknown one: CPUID, RDTSC and friends.
  //
  // Distinct from UNDEF, which says "anything at all". OPAQUE says "a value
  // named by (kind, result index) over these operands", so two CPUIDs with
  // the same leaf agree, and a consumer that only needs that much can reason
  // about it. Op::aux carries the kind and which result this op defines,
  // since an Op has one dst and these instructions define several registers:
  // CPUID becomes four OPAQUE ops sharing srcs and differing only in aux.
  OPAQUE,

  // IEEE754 double comparisons, 1-bit result; operands are 64-bit slots
  // reinterpreted as doubles like FADD's. Ordered: false if either operand
  // is NaN. FUNO is the unordered test (either is NaN). x86 COMISD/UCOMISD
  // set ZF = FUNO|FEQ, PF = FUNO, CF = FUNO|FLT.
  FLT,  // srcs[0] <  srcs[1], ordered
  FEQ,  // srcs[0] == srcs[1], ordered (+0 == -0)
  FUNO, // srcs[0] or srcs[1] is NaN

  // IEEE754 binary32, evaluated directly at 24-bit precision (RNE).
  // Raw bit patterns remain integer slots. Comparisons return one bit.
  FADD32, FSUB32, FMUL32, FDIV32,
  SITOFP32, FPTOSI32,
  FLT32, FEQ32, FUNO32,
  FPEXT,   // binary32 -> binary64 (exact for finite values)
  FPTRUNC, // binary64 -> binary32 (one RNE rounding)
  UITOFP, UITOFP32,
  FPTOUI, FPTOUI32, // float -> unsigned integer, toward zero
  FSQRT, FSQRT32,
  FFLOOR, FFLOOR32,
  FCEIL, FCEIL32,
  FTRUNC, FTRUNC32, // round to integral floating-point value, toward zero
  // Intra-instruction control flow. aux identifies a LABEL within this block;
  // LOCALCJMP takes srcs[0] when true, otherwise continues to the next op.
  LABEL, LOCALJMP, LOCALCJMP,

  // An unmodelled execution boundary (for example SYSCALL or SVC). No
  // operands or destination. Stop before this machine instruction: no
  // post-state or normal return is claimed, even when executing a trace.
  // This is distinct from an undefined *value* produced by UNDEF.
  UNSUPPORTED,
};

[[nodiscard]] constexpr bool isLocalControl(Opcode opcode) {
  return opcode == Opcode::LABEL || opcode == Opcode::LOCALJMP || opcode == Opcode::LOCALCJMP;
}

/// The instructions OPAQUE models. Appended to, never reordered - the value
/// is packed into Op::aux and crosses the C shim.
enum class OpaqueKind : std::uint16_t {
  CPUID,  ///< srcs = (leaf, subleaf); 4 results: EAX, EBX, ECX, EDX
  RDTSC,  ///< no srcs; 2 results: EAX (low), EDX (high)
  RDTSCP, ///< no srcs; 3 results: EAX (low), EDX (high), ECX (processor id)
};

/// Lower-case name used when lowering to Z3 and to LLVM ("cpuid", ...).
[[nodiscard]] std::string_view opaqueKindName(OpaqueKind kind);

/// True when the result is a function of the operands alone, so two
/// executions with equal operands must agree (CPUID). False when each
/// execution is its own event (RDTSC, RDTSCP) and must not be treated as
/// equal to another one - consumers key those on the op's address instead.
[[nodiscard]] bool opaqueIsPure(OpaqueKind kind);

/// How many results the kind defines; valid result indices are [0, count).
[[nodiscard]] unsigned opaqueResultCount(OpaqueKind kind);

/// Pack / unpack Op::aux for an OPAQUE op.
[[nodiscard]] constexpr std::uint32_t makeOpaqueAux(OpaqueKind kind,
                                                    unsigned resultIndex) {
  return (static_cast<std::uint32_t>(kind) << 8) | (resultIndex & 0xffu);
}
[[nodiscard]] constexpr OpaqueKind opaqueKindOf(std::uint32_t aux) {
  return static_cast<OpaqueKind>(aux >> 8);
}
[[nodiscard]] constexpr unsigned opaqueIndexOf(std::uint32_t aux) {
  return aux & 0xffu;
}

/// Opcode representation, we have a structure
/// modeled by each one of the operations, this
/// one must be simple enough, and each instruction
/// in Assembly will be 1 to N IGNIL Op.
struct Op {
  // operation from enum
  Opcode opcode;

  // Destination - absent for STORE, control flow, and local labels/branches
  std::optional<Value> dst;

  // Source operands (count and semantics depend on opcode)
  std::vector<Value> srcs;

  // Address of the machine instruction that produced this Op.
  // Multiple Ops from the same machine instruction share
  // this address. Zero for synthetic Ops.
  std::uint64_t address{};

  // Opcode-specific payload; OPAQUE uses makeOpaqueAux(kind,
  // resultIndex); local control flow uses a label ID. Deliberately last: every existing Op is aggregate-built as
  // {opcode, dst, srcs, address}, and inserting a field before `address`
  // would silently feed the address into this one instead.
  std::uint32_t aux{};
};

/// A block of IGNIL operations, this is a translation of an
/// assembly block, but the number of instructions may be bigger
/// since 1 assembly instruction may be translated to N IGNIL
/// instructions.
class Block {
  public:
  explicit Block(std::uint64_t startAddress);

  void append(Op op);

  [[nodiscard]] const std::vector<Op> &ops() const;
  [[nodiscard]] std::uint64_t startAddress() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] size_t size() const;

  // Used for iteration
  [[nodiscard]] std::vector<Op>::const_iterator begin() const;
  [[nodiscard]] std::vector<Op>::const_iterator end() const;

  /// Machine addresses of instructions the lifter had no semantics for.
  ///
  /// Such an instruction is not dropped silently: every register it defines
  /// (explicitly or implicitly) and, if it writes them, the flags are marked
  /// UNDEF. But an UNDEF that later dies is invisible, and an instruction whose
  /// only effect is a memory write leaves no op at all - so a consumer that
  /// needs the block to be a complete account of the code (a symbolic
  /// executor rebuilding a function, say) checks this list rather than trusting
  /// the ops alone. The optimisation passes carry it over to their result.
  void noteUnlifted(std::uint64_t address);
  [[nodiscard]] const std::vector<std::uint64_t> &unlifted() const;

  private:
  std::uint64_t startAddress_;
  std::vector<Op> ops_;
  std::vector<std::uint64_t> unlifted_;
};

/// Utility functions that can be used for debugging purposes
[[nodiscard]] std::string_view opcodeName(Opcode op);
[[nodiscard]] std::string toString(const Value &v);
[[nodiscard]] std::string toString(const Op &op);
[[nodiscard]] std::string toString(const Block &block);

} // namespace dragon::ignil
