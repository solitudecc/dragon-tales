#pragma once
#include <cstdint>
#include <string>

namespace dragon {

/// Syntax representation for the assembly instructions
/// this is part of the configuration to choose
enum class Syntax : std::uint8_t {
  INTEL = 0, ///< default, used by most of RE tools
  ATT, ///< AT&T style, used by GAS
};

/// Strategy used for the disassembly of the instructions in
/// the disassembler, for the moment, let's keep LINEAR_SWEEP
/// and RECURSIVE_TRAVERSAL. Soon, we can add an approach for
/// applying a mix to fill gaps
enum class DisassemblyStrategy : std::uint8_t {
  LINEAR_SWEEP = 0, ///< Go from the first to the last bytes disassembling
                    ///< instructions.
  RECURSIVE_TRAVERSAL, ///< Follow the control-flow in order to disassemble
                       ///< instructions.
};

/// Configuration shared by Disassembler and Assembler.
///
/// In addition to syntax and strategy, two LLVM-level knobs are exposed:
///
///   cpu     -> selects the target micro-architecture.  An empty string lets
///              LLVM choose its built-in "generic" baseline for the triple,
///              which may not enable optional ISA extensions.
///              Examples:
///                "cortex-a8"   → ARMv7-A with Thumb2, NEON, VFPv3
///                "arm926ej-s"  → ARMv5TEJ, enables BX/BLX
///                "skylake"     → x86-64 with AVX2, BMI2, etc.
///                "sifive-u74"  → RISC-V 64 with M/A/C extensions
///
///   features -> fine-grained ISA feature flags, comma-separated.
///              A '+' prefix enables a feature; a '-' prefix disables it.
///              These are combined with those implied by `cpu`.
///              Examples:
///                "+v4t"        → enable ARMv4T on ARM (adds BX)
///                "+v7,+neon"   → ARMv7 with NEON
///                "+avx2"       → enable AVX2 on x86
///                "+m,+a,+c"    → RISC-V multiply, atomics, compressed
///
/// Leaving both empty is safe and targets the most conservative baseline
/// for each architecture.
/// Calling convention used to interpret registers at function boundaries
/// (argument recovery). Default is the platform's usual one for the
/// architecture: System V on x86-64, AAPCS64 on AArch64.
enum class CallingConvention : std::uint8_t {
  Default = 0,
  SysV,  ///< x86-64 System V: RDI, RSI, RDX, RCX, R8, R9
  Win64, ///< Microsoft x64: RCX, RDX, R8, R9 (Windows PE)
};

struct Configuration {
  Syntax syntax = Syntax::INTEL;
  DisassemblyStrategy disassemblyStrategy = DisassemblyStrategy::LINEAR_SWEEP;

  /// LLVM CPU name forwarded to MCSubtargetInfo.
  /// Empty string selects the "generic" baseline for the target triple.
  std::string cpu;

  /// LLVM feature string forwarded to MCSubtargetInfo.
  /// Individual features separated by commas, each prefixed with '+' or '-'.
  /// Empty string uses the defaults implied by `cpu`.
  std::string features;

  /// When true, the library emits diagnostic messages to stderr.
  /// Set to false by default so library users are not surprised by output.
  bool logging = false;

  /// Calling convention of the code. loader::disassembleImage sets Win64 for
  /// an x86-64 PE image.
  CallingConvention callingConvention = CallingConvention::Default;

#ifdef DRAGON_WITH_Z3
  /// When true, the disassembler uses Z3 to enumerate possible targets of
  /// indirect jumps that the backward slicer cannot resolve to a single
  /// constant. Has no effect when the library is built without Z3
  /// (-DDRAGON_USE_Z3=OFF).
  bool useZ3ForPathDiscovery = false;
#endif
};

} // namespace dragon
