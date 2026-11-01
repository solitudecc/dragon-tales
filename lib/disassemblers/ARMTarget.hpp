#pragma once

#include "LLVMTarget.hpp"

#include <llvm/Support/TargetSelect.h>
#include <mutex>

namespace dragon::internal {

/// Initialize LLVM for ARM/AArch64 and return a populated LLVMTarget.
/// Registration is done once via std::call_once - thread safe.
///
/// @param is64bit  true for AArch64, false for ARM32.
/// @param cpu      LLVM CPU name.  Empty → "generic".
///                 ARM32 examples: "arm926ej-s" (ARMv5TEJ, enables BX/BLX),
///                                 "cortex-a8"  (ARMv7-A, Thumb2, NEON).
///                 ARM64 examples: "cortex-a53", "apple-m1".
/// @param features LLVM feature string.  Empty → defaults implied by cpu.
///                 ARM32 examples: "+v4t" (adds BX), "+v7,+neon".
///                 ARM64 examples: "+sve", "+crypto".
[[nodiscard]] inline LLVMTarget initARM(
    bool               is64bit,
    const std::string &cpu      = "",
    const std::string &features = "") {
  static std::once_flag flag;
  std::call_once(flag, [] {
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTarget();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMDisassembler();
    LLVMInitializeARMAsmPrinter();

    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64Disassembler();
    LLVMInitializeAArch64AsmPrinter();
  });

  const std::string triple = is64bit
      ? "aarch64-unknown-unknown"
      : "arm-unknown-unknown";

  // AArch64 with no explicit features: everything on, as llvm-objdump does
  // ("+all"). The generic CPU alone leaves out the extensions real binaries
  // use - LSE atomics (casal, ldadd: any modern atomic), crypto (aese,
  // pmull, sm4e), SVE (glibc's string routines), MTE - and those bytes did
  // not decode at all: 11,671 instructions of libcrypto, 369 of libc, 12 of
  // libstdc++ were missing. Undecodable bytes break linear sweep and CFGs.
  const std::string effective =
      (is64bit && features.empty()) ? std::string{"+all"} : features;
  return buildTarget(triple, cpu.empty() ? "generic" : cpu, effective);
}

} // namespace dragon::internal