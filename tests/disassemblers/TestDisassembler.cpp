#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>

#include <algorithm>
#include <set>

// -----------------------------------------------------------------------
// Helper
// -----------------------------------------------------------------------

static dragon::Disassembler makeDisassembler(
    dragon::Architecture arch,
    dragon::DisassemblyStrategy strategy = dragon::DisassemblyStrategy::LINEAR_SWEEP,
    dragon::Syntax syntax = dragon::Syntax::INTEL)
{
  dragon::Configuration config;
  config.disassemblyStrategy = strategy;
  config.syntax = syntax;
  return dragon::Disassembler(arch, config);
}

// -----------------------------------------------------------------------
// Valid / Invalid architecture
// -----------------------------------------------------------------------

TEST_CASE("Disassembler is valid for supported architectures",
          "[disassembler]") {
  dragon::Configuration config;

  SECTION("x86_64") {
    dragon::Disassembler dis(dragon::Architecture::X86_64, config);
    REQUIRE(dis.isValid());
  }
  SECTION("x86_32") {
    dragon::Disassembler dis(dragon::Architecture::X86_32, config);
    REQUIRE(dis.isValid());
  }
  SECTION("ARM32") {
    dragon::Disassembler dis(dragon::Architecture::ARM32, config);
    REQUIRE(dis.isValid());
  }
  SECTION("ARM64") {
    dragon::Disassembler dis(dragon::Architecture::ARM64, config);
    REQUIRE(dis.isValid());
  }
  SECTION("RISC-V 32") {
    dragon::Disassembler dis(dragon::Architecture::RISCV32, config);
    REQUIRE(dis.isValid());
  }
  SECTION("RISC-V 64") {
    dragon::Disassembler dis(dragon::Architecture::RISCV64, config);
    REQUIRE(dis.isValid());
  }
  SECTION("Unknown architecture") {
    dragon::Disassembler dis(dragon::Architecture::Unknown, config);
    REQUIRE_FALSE(dis.isValid());
  }
}

// -----------------------------------------------------------------------
// Empty / edge cases
// -----------------------------------------------------------------------

TEST_CASE("Disassembler edge cases", "[disassembler]") {
  auto dis = makeDisassembler(dragon::Architecture::X86_64);

  SECTION("empty bytes returns empty list") {
    auto instrs = dis.disassemble({}, 0x1000);
    REQUIRE(instrs.empty());
  }

  SECTION("single nop") {
    const std::vector<std::uint8_t> bytes = {0x90};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[0].size() == 1);
  }
}

// -----------------------------------------------------------------------
// x86-64
// -----------------------------------------------------------------------

TEST_CASE("Disassembler x86_64 - instruction fields", "[disassembler][x86_64]") {
  auto dis = makeDisassembler(dragon::Architecture::X86_64);

  SECTION("nop + ret - addresses and mnemonics") {
    const std::vector<std::uint8_t> bytes = {0x90, 0xC3};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[0].mnemonic() == "nop");
    REQUIRE(instrs[1].address() == 0x1001);
    REQUIRE(instrs[1].isReturn());
  }

  SECTION("mov rax, rcx - size, mnemonic, no control-flow flags") {
    // 48 89 C8 = mov rax, rcx
    const std::vector<std::uint8_t> bytes = {0x48, 0x89, 0xC8};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].size() == 3);
    REQUIRE(instrs[0].mnemonic() == "mov");
    REQUIRE_FALSE(instrs[0].isJump());
    REQUIRE_FALSE(instrs[0].isCall());
    REQUIRE_FALSE(instrs[0].isReturn());
    REQUIRE(instrs[0].branches().empty());
  }

  SECTION("raw bytes are preserved") {
    const std::vector<std::uint8_t> bytes = {0x48, 0x89, 0xC8};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs[0].bytes() == bytes);
  }

  SECTION("address increments correctly") {
    // nop (1), nop (1), ret (1)
    const std::vector<std::uint8_t> bytes = {0x90, 0x90, 0xC3};
    auto instrs = dis.disassemble(bytes, 0x2000);
    REQUIRE(instrs.size() == 3);
    REQUIRE(instrs[0].address() == 0x2000);
    REQUIRE(instrs[1].address() == 0x2001);
    REQUIRE(instrs[2].address() == 0x2002);
  }

  SECTION("ret imm16") {
    // C2 08 00 = ret 8
    const std::vector<std::uint8_t> bytes = {0xC2, 0x08, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isReturn());
  }
}

TEST_CASE("Disassembler x86_64 - branch detection", "[disassembler][x86_64][branch]") {
  auto dis = makeDisassembler(dragon::Architecture::X86_64);

  SECTION("unconditional direct jump - isJump, has target") {
    // EB 05 = jmp +5  →  target = 0x1000 + 2 + 5 = 0x1007
    const std::vector<std::uint8_t> bytes = {0xEB, 0x05};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE_FALSE(instrs[0].isCall());
    REQUIRE_FALSE(instrs[0].isReturn());
    REQUIRE(instrs[0].branches().contains(0x1007));
  }

  SECTION("conditional je - isJump, isConditional, has target") {
    // 74 05 = je +5  →  target = 0x1007
    const std::vector<std::uint8_t> bytes = {0x74, 0x05};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].isConditional());
    REQUIRE_FALSE(instrs[0].branches().empty());
  }

  SECTION("indirect jump - isJump, no known target") {
    // FF E0 = jmp rax
    const std::vector<std::uint8_t> bytes = {0xFF, 0xE0};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].branches().empty());
  }
}

TEST_CASE("Disassembler x86_64 - call detection", "[disassembler][x86_64][call]") {
  auto dis = makeDisassembler(dragon::Architecture::X86_64);

  SECTION("direct call - isCall") {
    // E8 00 00 00 00 = call +0  →  target = 0x1005
    const std::vector<std::uint8_t> bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isCall());
    REQUIRE_FALSE(instrs[0].isJump());
    REQUIRE_FALSE(instrs[0].isReturn());
  }

  SECTION("indirect call - isCall, no branch target") {
    // FF D0 = call rax
    const std::vector<std::uint8_t> bytes = {0xFF, 0xD0};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isCall());
    REQUIRE(instrs[0].branches().empty());
  }
}

// -----------------------------------------------------------------------
// x86-32
// -----------------------------------------------------------------------

TEST_CASE("Disassembler x86_32", "[disassembler][x86_32]") {
  auto dis = makeDisassembler(dragon::Architecture::X86_32);

  SECTION("nop") {
    const std::vector<std::uint8_t> bytes = {0x90};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].mnemonic() == "nop");
    REQUIRE(instrs[0].size() == 1);
  }

  SECTION("ret") {
    const std::vector<std::uint8_t> bytes = {0xC3};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isReturn());
  }

  SECTION("address increments correctly") {
    // nop (1) + ret (1)
    const std::vector<std::uint8_t> bytes = {0x90, 0xC3};
    auto instrs = dis.disassemble(bytes, 0x400);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x400);
    REQUIRE(instrs[0].size() == 1);
    REQUIRE(instrs[1].address() == 0x401);
  }

  SECTION("direct call") {
    // E8 00 00 00 00 = call +0
    const std::vector<std::uint8_t> bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isCall());
  }

  SECTION("unconditional jump") {
    // EB 05 = jmp +5  →  target = 0x1007
    const std::vector<std::uint8_t> bytes = {0xEB, 0x05};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].branches().contains(0x1007));
  }

  SECTION("conditional jump") {
    // 74 05 = je +5
    const std::vector<std::uint8_t> bytes = {0x74, 0x05};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].isConditional());
  }
}

// -----------------------------------------------------------------------
// ARM64
// -----------------------------------------------------------------------

TEST_CASE("Disassembler ARM64", "[disassembler][arm64]") {
  auto dis = makeDisassembler(dragon::Architecture::ARM64);

  SECTION("nop - size and address") {
    // D503201F = nop
    const std::vector<std::uint8_t> bytes = {0x1F, 0x20, 0x03, 0xD5};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[0].size() == 4);
  }

  SECTION("ret - isReturn") {
    // D65F03C0 = ret
    const std::vector<std::uint8_t> bytes = {0xC0, 0x03, 0x5F, 0xD6};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isReturn());
  }

  SECTION("all instructions are 4 bytes") {
    // nop + ret
    const std::vector<std::uint8_t> bytes = {
        0x1F, 0x20, 0x03, 0xD5,  // nop
        0xC0, 0x03, 0x5F, 0xD6,  // ret
    };
    auto instrs = dis.disassemble(bytes, 0x2000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x2000);
    REQUIRE(instrs[0].size() == 4);
    REQUIRE(instrs[1].address() == 0x2004);
    REQUIRE(instrs[1].size() == 4);
  }

  SECTION("bl - isCall") {
    // 94000001 = bl #4  (branch-and-link 1 instruction forward)
    const std::vector<std::uint8_t> bytes = {0x01, 0x00, 0x00, 0x94};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isCall());
    REQUIRE_FALSE(instrs[0].isReturn());
  }

  SECTION("b - isJump, not a call") {
    // 14000001 = b #4  (unconditional branch 1 instruction forward)
    const std::vector<std::uint8_t> bytes = {0x01, 0x00, 0x00, 0x14};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE_FALSE(instrs[0].isCall());
  }

  SECTION("cbz - isJump, isConditional") {
    // B4000001 = cbz x1, #4  (compare-and-branch-if-zero)
    const std::vector<std::uint8_t> bytes = {0x01, 0x00, 0x00, 0xB4};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].isConditional());
  }

  // Regression test: the mnemonic/operand split used to be derived from
  // MCInstPrinter::getMnemonic(), which returns LLVM's raw/canonical opcode
  // name (e.g. "movz", "subs", "b.") rather than the alias printInst()
  // actually renders (e.g. "mov", "cmp", "b.le"). Whenever the two diverged,
  // the split against `full` either failed outright (empty operands) or
  // matched a spurious prefix (garbled operands, e.g. "b. le\t#12").
  SECTION("aliased instructions produce correct mnemonic and operands") {
    // mov x0, #42
    {
      const std::vector<std::uint8_t> bytes = {0x40, 0x05, 0x80, 0xD2};
      auto instrs = dis.disassemble(bytes, 0x1000);
      REQUIRE(instrs.size() == 1);
      REQUIRE(instrs[0].mnemonic() == "mov");
      REQUIRE(instrs[0].operands() == "x0, #42");
    }
    // cmp x0, x1  (encoded as SUBS with a discarded destination)
    {
      const std::vector<std::uint8_t> bytes = {0x1F, 0x00, 0x01, 0xEB};
      auto instrs = dis.disassemble(bytes, 0x1000);
      REQUIRE(instrs.size() == 1);
      REQUIRE(instrs[0].mnemonic() == "cmp");
      REQUIRE(instrs[0].operands() == "x0, x1");
    }
    // b.le #12  (Bcc, whose raw mnemonic "b." is itself a prefix of "b.le")
    {
      const std::vector<std::uint8_t> bytes = {0x6D, 0x00, 0x00, 0x54};
      auto instrs = dis.disassemble(bytes, 0x1000);
      REQUIRE(instrs.size() == 1);
      REQUIRE(instrs[0].mnemonic() == "b.le");
      REQUIRE(instrs[0].operands() == "#12");
    }
  }
}

// -----------------------------------------------------------------------
// ARM32
// -----------------------------------------------------------------------

TEST_CASE("Disassembler ARM32 - generic CPU", "[disassembler][arm32]") {
  // "generic" targets ARMv4 baseline: B, BL, and data-processing work;
  // BX requires ARMv4T (HasV4TOps) and is not available here.
  auto dis = makeDisassembler(dragon::Architecture::ARM32);

  SECTION("mov r0, r0 - decodes as one 4-byte instruction") {
    // E1A00000 = mov r0, r0
    const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0xA0, 0xE1};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].size() == 4);
    REQUIRE(instrs[0].address() == 0x1000);
  }

  SECTION("bl - isCall") {
    // EB000000 = bl #0  (target = PC+8 = 0x1008)
    const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x00, 0xEB};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isCall());
    REQUIRE(instrs[0].callees() == std::set<std::uint64_t>{0x1008});
  }

  SECTION("b - isJump, not a call") {
    // EA000000 = b #0  (target = PC+8 = 0x1008)
    const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x00, 0xEA};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
    REQUIRE_FALSE(instrs[0].isCall());
    REQUIRE(instrs[0].branches() == std::set<std::uint64_t>{0x1008});
  }

  // Regression: A32 branch targets were computed as address + size + imm
  // (x86's rule, 4 bytes short) instead of address + 8 + imm, and `b` (Bcc
  // with the AL predicate) was flagged conditional. Expected values are what
  // llvm-objdump --triple=armv7 prints for the same words.
  SECTION("A32 branch targets are relative to PC = address + 8") {
    struct Case {
      std::uint32_t word;
      std::uint64_t target;
      bool conditional;
    };
    const Case cases[] = {
            {0xEA000002, 0x1010, false}, // b   #8
            {0x0A000002, 0x1010, true},  // beq #8
            {0xEAFFFFFD, 0x0FFC, false}, // b   #-12
    };
    for (const auto &c: cases) {
      const std::vector<std::uint8_t> bytes = {
              static_cast<std::uint8_t>(c.word),
              static_cast<std::uint8_t>(c.word >> 8),
              static_cast<std::uint8_t>(c.word >> 16),
              static_cast<std::uint8_t>(c.word >> 24)};
      auto instrs = dis.disassemble(bytes, 0x1000);
      REQUIRE(instrs.size() == 1);
      CHECK(instrs[0].branches() == std::set<std::uint64_t>{c.target});
      CHECK(instrs[0].isConditional() == c.conditional);
    }
  }

  SECTION("A32 bl target is relative to PC = address + 8") {
    // EBFFFFFF = bl #-4 at 0x1008 -> 0x1008 + 8 - 4 = 0x100c
    const std::vector<std::uint8_t> bytes = {0xFF, 0xFF, 0xFF, 0xEB};
    auto instrs = dis.disassemble(bytes, 0x1008);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].callees() == std::set<std::uint64_t>{0x100c});
  }

  SECTION("address increments by 4 per instruction") {
    const std::vector<std::uint8_t> bytes = {
        0x00, 0x00, 0xA0, 0xE1,  // mov r0, r0  (0x1000)
        0x00, 0x00, 0x00, 0xEA,  // b #0        (0x1004)
    };
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[1].address() == 0x1004);
  }
}

TEST_CASE("Disassembler ARM32 - ARMv5TEJ CPU (arm926ej-s)", "[disassembler][arm32]") {
  // "arm926ej-s" enables HasV4TOps, making BX/BLX available.
  dragon::Configuration config;
  config.cpu = "arm926ej-s";
  auto dis = dragon::Disassembler(dragon::Architecture::ARM32, config);
  REQUIRE(dis.isValid());

  SECTION("bx lr - isReturn or isJump") {
    // E12FFF1E = bx lr
    const std::vector<std::uint8_t> bytes = {0x1E, 0xFF, 0x2F, 0xE1};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE((instrs[0].isReturn() || instrs[0].isJump()));
    REQUIRE_FALSE(instrs[0].isCall());
  }

  SECTION("bx lr - address increments correctly alongside generic instructions") {
    const std::vector<std::uint8_t> bytes = {
        0x00, 0x00, 0xA0, 0xE1,  // mov r0, r0  (0x1000)
        0x1E, 0xFF, 0x2F, 0xE1,  // bx lr       (0x1004)
    };
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[1].address() == 0x1004);
  }
}

// -----------------------------------------------------------------------
// RISC-V 32
// -----------------------------------------------------------------------

TEST_CASE("Disassembler RISC-V 32", "[disassembler][riscv32]") {
  auto dis = makeDisassembler(dragon::Architecture::RISCV32);

  SECTION("nop (addi x0, x0, 0) - size and address") {
    // 00000013 = addi x0, x0, 0
    const std::vector<std::uint8_t> bytes = {0x13, 0x00, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[0].size() == 4);
  }

  SECTION("ret (jalr x0, 0(x1)) - decodes to one 4-byte instruction") {
    // 00008067 = jalr x0, 0(x1)  (the ret pseudo-instruction)
    // LLVM's RISC-V MC layer sets isTerminator/isBarrier but not isBranch or
    // isReturn on JALR, so we only assert it decodes correctly.
    const std::vector<std::uint8_t> bytes = {0x67, 0x80, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].size() == 4);
  }

  SECTION("address increments by 4 per instruction") {
    // nop + ret
    const std::vector<std::uint8_t> bytes = {
        0x13, 0x00, 0x00, 0x00,  // addi x0, x0, 0
        0x67, 0x80, 0x00, 0x00,  // jalr x0, 0(x1)
    };
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x1000);
    REQUIRE(instrs[1].address() == 0x1004);
  }
}

// -----------------------------------------------------------------------
// RISC-V 64
// -----------------------------------------------------------------------

TEST_CASE("Disassembler RISC-V 64", "[disassembler][riscv64]") {
  auto dis = makeDisassembler(dragon::Architecture::RISCV64);

  SECTION("nop (addi x0, x0, 0)") {
    const std::vector<std::uint8_t> bytes = {0x13, 0x00, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].size() == 4);
  }

  SECTION("ret (jalr x0, 0(x1)) - decodes to one 4-byte instruction") {
    // LLVM's RISC-V MC layer sets isTerminator/isBarrier but not isBranch or
    // isReturn on JALR, so we only assert it decodes correctly.
    const std::vector<std::uint8_t> bytes = {0x67, 0x80, 0x00, 0x00};
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].size() == 4);
  }

  SECTION("address increments by 4 per instruction") {
    const std::vector<std::uint8_t> bytes = {
        0x13, 0x00, 0x00, 0x00,
        0x67, 0x80, 0x00, 0x00,
    };
    auto instrs = dis.disassemble(bytes, 0x2000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].address() == 0x2000);
    REQUIRE(instrs[1].address() == 0x2004);
  }

  SECTION("RISC-V 32 and 64 decode same base encodings identically") {
    const std::vector<std::uint8_t> bytes = {
        0x13, 0x00, 0x00, 0x00,  // nop
        0x67, 0x80, 0x00, 0x00,  // ret
    };
    auto dis32 = makeDisassembler(dragon::Architecture::RISCV32);
    auto dis64 = makeDisassembler(dragon::Architecture::RISCV64);
    auto instrs32 = dis32.disassemble(bytes, 0x1000);
    auto instrs64 = dis64.disassemble(bytes, 0x1000);
    REQUIRE(instrs32.size() == instrs64.size());
    for (std::size_t i = 0; i < instrs32.size(); ++i) {
      REQUIRE(instrs32[i].size() == instrs64[i].size());
      REQUIRE(instrs32[i].address() == instrs64[i].address());
    }
  }
}

// -----------------------------------------------------------------------
// Linear sweep vs Recursive traversal
// -----------------------------------------------------------------------

TEST_CASE("Linear sweep and recursive traversal agree on straight-line code",
          "[disassembler][strategy]") {
  // nop; nop; ret - no branches, no dead code
  const std::vector<std::uint8_t> bytes = {0x90, 0x90, 0xC3};

  auto linear    = makeDisassembler(dragon::Architecture::X86_64,
                                    dragon::DisassemblyStrategy::LINEAR_SWEEP);
  auto recursive = makeDisassembler(dragon::Architecture::X86_64,
                                    dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);

  auto instrsL = linear.disassemble(bytes, 0x1000);
  auto instrsR = recursive.disassemble(bytes, 0x1000);

  REQUIRE(instrsL.size() == instrsR.size());
  for (std::size_t i = 0; i < instrsL.size(); ++i) {
    REQUIRE(instrsL[i].address() == instrsR[i].address());
    REQUIRE(instrsL[i].mnemonic() == instrsR[i].mnemonic());
  }
}

TEST_CASE("Recursive traversal skips dead code after ret",
          "[disassembler][strategy]") {
  // C3 = ret (terminates the trace), 90 = nop (unreachable from 0x1000)
  const std::vector<std::uint8_t> bytes = {0xC3, 0x90};

  SECTION("linear sweep decodes both instructions") {
    auto dis = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::LINEAR_SWEEP);
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].isReturn());
  }

  SECTION("recursive traversal stops at ret") {
    auto dis = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isReturn());
  }
}

TEST_CASE("Recursive traversal skips dead code after indirect jump",
          "[disassembler][strategy]") {
  // FF E0 = jmp rax (indirect, no known target - terminates the trace)
  // 90    = nop (unreachable from 0x1000)
  const std::vector<std::uint8_t> bytes = {0xFF, 0xE0, 0x90};

  SECTION("linear sweep decodes all bytes") {
    auto dis = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::LINEAR_SWEEP);
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 2);
    REQUIRE(instrs[0].isJump());
    REQUIRE(instrs[0].branches().empty());
  }

  SECTION("recursive traversal stops at indirect jump") {
    auto dis = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);
    auto instrs = dis.disassemble(bytes, 0x1000);
    REQUIRE(instrs.size() == 1);
    REQUIRE(instrs[0].isJump());
  }
}

TEST_CASE("Recursive traversal follows direct branch targets",
          "[disassembler][strategy]") {
  // je 0x1007 (74 05) - conditional branch to 0x1007
  // nop (90)          - fall-through at 0x1002
  // ret (C3)          - fall-through terminates at 0x1003
  //   [gap: 0x1004–0x1006 not in buffer, branch target is relative]
  // nop (90) at 0x1007 - reachable via je
  // ret (C3) at 0x1008
  //
  // Buffer starts at 0x1000:
  // 0x1000: 74 05   je 0x1007
  // 0x1002: 90      nop
  // 0x1003: C3      ret
  // 0x1004: 90 90 90  padding (decoded by linear sweep)
  // 0x1007: 90      nop
  // 0x1008: C3      ret
  const std::vector<std::uint8_t> bytes = {
      0x74, 0x05,        // je 0x1007
      0x90,              // nop  (0x1002)
      0xC3,              // ret  (0x1003)
      0x90, 0x90, 0x90,  // padding (0x1004-0x1006)
      0x90,              // nop  (0x1007)
      0xC3,              // ret  (0x1008)
  };

  auto linear    = makeDisassembler(dragon::Architecture::X86_64,
                                    dragon::DisassemblyStrategy::LINEAR_SWEEP);
  auto recursive = makeDisassembler(dragon::Architecture::X86_64,
                                    dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);

  auto instrsL = linear.disassemble(bytes, 0x1000);
  auto instrsR = recursive.disassemble(bytes, 0x1000);

  // Linear sweep decodes every instruction sequentially.
  // je is 2 bytes → 1 instruction, so 9 bytes produce 8 instructions total.
  REQUIRE(instrsL.size() == 8);

  // Recursive traversal reaches the branch target at 0x1007
  auto found1007 = std::any_of(instrsR.begin(), instrsR.end(),
      [](const dragon::Instruction &i){ return i.address() == 0x1007; });
  REQUIRE(found1007);
}

TEST_CASE("Recursive traversal - AT&T syntax produces same instruction count",
          "[disassembler][strategy][syntax]") {
  const std::vector<std::uint8_t> bytes = {0x90, 0xC3};

  auto intel = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL,
                                dragon::Syntax::INTEL);
  auto att   = makeDisassembler(dragon::Architecture::X86_64,
                                dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL,
                                dragon::Syntax::ATT);

  REQUIRE(intel.disassemble(bytes, 0x1000).size() ==
          att.disassemble(bytes, 0x1000).size());
}

// AArch64 decodes the architecture extensions real binaries use with the
// default configuration (no cpu/features): LSE atomics (any modern atomic
// operation), crypto, SVE, MTE. With LLVM's generic CPU alone none of these
// decoded - 11,671 instructions of a distribution libcrypto, 369 of libc
// and 12 of libstdc++ were missing, breaking linear sweep around them.
TEST_CASE("Disassembler arm64 - extensions decode by default",
          "[disassembler][arm64]") {
  struct Case {
    std::uint32_t word;
    const char *mnemonic;
  };
  const Case cases[] = {{0xc8e0fc41, "casal"},  {0xb8e00041, "ldaddal"},
                        {0xf8e08041, "swpal"},  {0x4e284820, "aese"},
                        {0x0ee2e020, "pmull"},  {0x2518e3e0, "ptrue"},
                        {0x9adf1020, "irg"}};
  dragon::Disassembler dis(dragon::Architecture::ARM64,
                           dragon::Configuration{});
  for (const auto &c: cases) {
    INFO(c.mnemonic);
    const std::vector<std::uint8_t> bytes = {
            static_cast<std::uint8_t>(c.word), static_cast<std::uint8_t>(c.word >> 8),
            static_cast<std::uint8_t>(c.word >> 16),
            static_cast<std::uint8_t>(c.word >> 24)};
    auto insns = dis.disassemble(bytes, 0x1000);
    REQUIRE(insns.size() == 1);
    CHECK(insns[0].size() == 4);
    CHECK(insns[0].mnemonic() == c.mnemonic);
  }
}
