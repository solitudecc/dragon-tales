#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#ifdef DRAGON_WITH_Z3
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif

#include <algorithm>
#include <set>
#include <vector>

namespace arm = dragon::arm;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static dragon::Disassembler makeDisassembler(dragon::Architecture arch) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  cfg.syntax = dragon::Syntax::INTEL;
  return dragon::Disassembler(arch, cfg);
}

static dragon::Graph makeGraph(dragon::Architecture arch) {
  return dragon::Graph(arch, dragon::Configuration{});
}

static bool blockDefines(const dragon::ignil::Block &block,
                         const dragon::ignil::Reg &reg) {
  for (const dragon::ignil::Op &op: block) {
    if (op.dst && std::holds_alternative<dragon::ignil::Reg>(*op.dst)) {
      const auto &dst = std::get<dragon::ignil::Reg>(*op.dst);
      if (dst.offset == reg.offset && dst.width == reg.width)
        return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------
// CMP (SUBS with discarded destination) must set NZCV flags.
//
// Regression test: liftInstruction()'s dispatch only checked the plain
// ADD/SUB/AND opcodes, never their S-suffixed counterparts (ADDS/SUBS/ANDS -
// the encodings behind cmp/cmn/tst), so flag-setting instructions were
// silently dropped from the IGNIL block (zero ops emitted for them).
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - cmp sets NZCV flags", "[ignil][arm64]") {
  // 0x1000: cmp w0, #5   71 00 14 1f -> LE: 1f 14 00 71
  // 0x1004: ret          c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x1f, 0x14, 0x00, 0x71, // cmp w0, #5
          0xc0, 0x03, 0x5f, 0xd6, // ret
  };

  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  const dragon::ignil::Block *entry = g.getIGNILBlock(0x1000);
  REQUIRE(entry != nullptr);

  // Before the fix this block held only the RET - no flag ops at all.
  REQUIRE(entry->ops().size() > 1);

  REQUIRE(blockDefines(*entry, arm::NF));
  REQUIRE(blockDefines(*entry, arm::ZF));
  REQUIRE(blockDefines(*entry, arm::CF));
  REQUIRE(blockDefines(*entry, arm::VF));
}

// -----------------------------------------------------------------------
// Full if/else function: the entry block's cmp/b.le must lower to a CJMP
// whose condition traces back to the ZF/NF flags set by the cmp.
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - conditional branch (cmp/b.le)",
          "[ignil][arm64]") {
  // Equivalent C:  int foo(int x) { if (x > 0) return 1; else return -1; }
  //
  // 0x1000: stp x29, x30, [sp, #-16]!   fd 7b bf a9
  // 0x1004: mov x29, sp                 fd 03 00 91
  // 0x1008: cmp w0, #0                  1f 00 00 71
  // 0x100c: b.le 0x101c                 8d 00 00 54
  // -- then-block --
  // 0x1010: mov w0, #1                  20 00 80 52
  // 0x1014: ldp x29, x30, [sp], #16     fd 7b c1 a8
  // 0x1018: ret                         c0 03 5f d6
  // -- else-block --
  // 0x101c: mov w0, #-1                 00 00 80 12
  // 0x1020: ldp x29, x30, [sp], #16     fd 7b c1 a8
  // 0x1024: ret                         c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0xfd, 0x7b, 0xbf, 0xa9, // stp x29, x30, [sp, #-16]!  (0x1000)
          0xfd, 0x03, 0x00, 0x91, // mov x29, sp                (0x1004)
          0x1f, 0x00, 0x00, 0x71, // cmp w0, #0                 (0x1008)
          0x8d, 0x00, 0x00, 0x54, // b.le 0x101c                (0x100c)
          0x20, 0x00, 0x80, 0x52, // mov w0, #1                 (0x1010)
          0xfd, 0x7b, 0xc1, 0xa8, // ldp x29, x30, [sp], #16    (0x1014)
          0xc0, 0x03, 0x5f, 0xd6, // ret                        (0x1018)
          0x00, 0x00, 0x80, 0x12, // mov w0, #-1                (0x101c)
          0xfd, 0x7b, 0xc1, 0xa8, // ldp x29, x30, [sp], #16    (0x1020)
          0xc0, 0x03, 0x5f, 0xd6, // ret                        (0x1024)
  };

  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  const dragon::ignil::Block *entry = g.getIGNILBlock(0x1000);
  REQUIRE(entry != nullptr);
  REQUIRE_FALSE(entry->empty());

  REQUIRE(blockDefines(*entry, arm::ZF));
  REQUIRE(blockDefines(*entry, arm::NF));

  const dragon::ignil::Op &terminator = entry->ops().back();
  REQUIRE(terminator.opcode == dragon::ignil::Opcode::CJMP);
  // taken = 0x101c (else-block), fall-through = 0x1010 (then-block)
  REQUIRE(std::get<dragon::ignil::Const>(terminator.srcs[1]).value == 0x101c);
  REQUIRE(std::get<dragon::ignil::Const>(terminator.srcs[2]).value == 0x1010);
}

// -----------------------------------------------------------------------
// mov w0, #-1 (MOVNWi) must produce a Const whose value fits its declared
// width.
//
// Regression test: liftMovn() computed `value = ~(imm << shift)` as a full
// 64-bit bitwise-NOT without masking to the destination's width, so a
// 32-bit `mov w0, #-1` produced Const{0xFFFFFFFFFFFFFFFF, width=32} -
// silently invalid (a 32-bit-wide Const whose value needs 64 bits). Any
// consumer that rebuilds an APInt from it (e.g. LLVMLifter) hits an
// "Value is not an N-bit unsigned value" assertion.
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - mov w0, #-1 masks the constant to 32 bits",
          "[ignil][arm64]") {
  // 0x1000: mov w0, #-1   00 00 80 12
  // 0x1004: ret           c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x00, 0x00, 0x80, 0x12, // mov w0, #-1
          0xc0, 0x03, 0x5f, 0xd6, // ret
  };

  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  const dragon::ignil::Block *entry = g.getIGNILBlock(0x1000);
  REQUIRE(entry != nullptr);
  REQUIRE_FALSE(entry->empty());

  const dragon::ignil::Op &movOp = entry->ops().front();
  REQUIRE(movOp.opcode == dragon::ignil::Opcode::COPY);
  const auto &c = std::get<dragon::ignil::Const>(movOp.srcs[0]);
  REQUIRE(c.width == 32);
  REQUIRE(c.value == 0xFFFFFFFFu); // not 0xFFFFFFFFFFFFFFFF
}

// -----------------------------------------------------------------------
// A block that falls straight into the next block (no explicit branch at
// its end) must still get an explicit JMP terminator in its IGNIL block.
//
// Regression test: liftBlock() only emitted a JMP/CJMP/RET when the last
// machine instruction in the block was itself a branch/return. A block
// like the else-arm below (whose last real instruction is just `mov`, with
// execution falling through into the next block) produced an IGNIL block
// with no terminator at all - invalid input for LLVMLifter, which hangs
// LLVM's mem2reg pass when it computes dominator trees over a basic block
// with no terminator instruction.
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - fall-through block gets an explicit JMP",
          "[ignil][arm64]") {
  // 0x1000: cmp x0, x1     1f 00 01 eb
  // 0x1004: b.le 0x100c    4d 00 00 54
  // 0x1008: mov x0, #1     20 00 80 d2   (falls through, no branch!)
  // 0x100c: ret            c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x1f, 0x00, 0x01, 0xeb, // cmp x0, x1   @ 0x1000
          0x4d, 0x00, 0x00, 0x54, // b.le 0x100c  @ 0x1004
          0x20, 0x00, 0x80, 0xd2, // mov x0, #1   @ 0x1008
          0xc0, 0x03, 0x5f, 0xd6, // ret          @ 0x100c
  };

  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  const dragon::ignil::Block *fallThroughBlock = g.getIGNILBlock(0x1008);
  REQUIRE(fallThroughBlock != nullptr);
  REQUIRE_FALSE(fallThroughBlock->empty());

  const dragon::ignil::Op &terminator = fallThroughBlock->ops().back();
  REQUIRE(terminator.opcode == dragon::ignil::Opcode::JMP);
  REQUIRE(std::get<dragon::ignil::Const>(terminator.srcs[0]).value == 0x100c);
}

// -----------------------------------------------------------------------
// A32 (ARM32) has no semantic lifter. Regression: it used to be run through
// the AArch64 lifter, whose opcode table is looked up *by name* - so A32's
// BL and Bcc (names shared with AArch64) were lifted with AArch64 operand
// layouts: `bl #-4` @0x1008 became CALL(0xff8), `b #8` became a conditional
// jump to 0x103c. Now only the decoded control flow is kept and every other
// instruction is reported through Block::unlifted().
// -----------------------------------------------------------------------

static dragon::Disassembler makeA32Disassembler() {
  dragon::Configuration cfg;
  cfg.cpu = "arm926ej-s"; // ARMv5TEJ: has BX
  return dragon::Disassembler(dragon::Architecture::ARM32, cfg);
}

static bool contains(const std::vector<std::uint64_t> &v, std::uint64_t x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

TEST_CASE("ARM IGNIL lifting - A32 call target and unlifted skeleton",
          "[ignil][arm32]") {
  const std::vector<std::uint8_t> bytes = {
          0x05, 0x00, 0xa0, 0xe3, // mov r0, #5     @ 0x1000
          0x01, 0x00, 0x80, 0xe2, // add r0, r0, #1 @ 0x1004
          0xff, 0xff, 0xff, 0xeb, // bl  0x100c     @ 0x1008
          0x1e, 0xff, 0x2f, 0xe1, // bx  lr         @ 0x100c
  };
  auto dis = makeA32Disassembler();
  auto g = makeGraph(dragon::Architecture::ARM32);
  dis.disassemble(bytes, 0x1000, g);

  REQUIRE(g.validFunctionAddresses() == std::set<std::uint64_t>{0x1000, 0x100c});

  const dragon::ignil::Block *entry = g.getIGNILBlock(0x1000);
  REQUIRE(entry != nullptr);

  bool sawCall = false;
  for (const auto &op: *entry) {
    // No A32 register has an IGNIL mapping: nothing may be written to one
    // (the old path wrote AArch64 registers for A32 code).
    REQUIRE_FALSE((op.dst && std::holds_alternative<dragon::ignil::Reg>(*op.dst)));
    if (op.opcode == dragon::ignil::Opcode::CALL) {
      sawCall = true;
      REQUIRE(std::get<dragon::ignil::Const>(op.srcs[0]).value == 0x100c);
    }
  }
  REQUIRE(sawCall);

  CHECK(contains(entry->unlifted(), 0x1000));
  CHECK(contains(entry->unlifted(), 0x1004));
}

TEST_CASE("ARM IGNIL lifting - A32 b / beq terminators match the CFG",
          "[ignil][arm32]") {
  const std::vector<std::uint8_t> bytes = {
          0x02, 0x00, 0x00, 0x0a, // beq 0x1010     @ 0x1000
          0x02, 0x00, 0x00, 0xea, // b   0x1014     @ 0x1004
          0x00, 0x00, 0xa0, 0xe1, // mov r0, r0     @ 0x1008
          0x00, 0x00, 0xa0, 0xe1, // mov r0, r0     @ 0x100c
          0x00, 0x00, 0xa0, 0xe1, // mov r0, r0     @ 0x1010
          0x1e, 0xff, 0x2f, 0xe1, // bx  lr         @ 0x1014
  };
  auto dis = makeA32Disassembler();
  auto g = makeGraph(dragon::Architecture::ARM32);
  dis.disassemble(bytes, 0x1000, g);

  const dragon::ignil::Block *beq = g.getIGNILBlock(0x1000);
  REQUIRE(beq != nullptr);
  const auto &cj = beq->ops().back();
  REQUIRE(cj.opcode == dragon::ignil::Opcode::CJMP);
  // Flags are never computed for A32, so the condition is UNDEF - the edges
  // are what matters.
  REQUIRE(std::holds_alternative<dragon::ignil::Temp>(cj.srcs[0]));
  CHECK(std::get<dragon::ignil::Const>(cj.srcs[1]).value == 0x1010);
  CHECK(std::get<dragon::ignil::Const>(cj.srcs[2]).value == 0x1004);

  const dragon::ignil::Block *b = g.getIGNILBlock(0x1004);
  REQUIRE(b != nullptr);
  const auto &j = b->ops().back();
  REQUIRE(j.opcode == dragon::ignil::Opcode::JMP);
  CHECK(std::get<dragon::ignil::Const>(j.srcs[0]).value == 0x1014);

  // `b` is unconditional: no fall-through block after it.
  CHECK_FALSE(g.isValidBlock(0x1008));
}

// -----------------------------------------------------------------------
// AArch64 instruction with no lifter. Regression: it was dropped with no
// trace, so `mov x0,#3; mul x0,x0,x1; ret` read back as X0 = 3. (MUL is
// lifted since; CLZ, still without a lifter, stands in for it.)
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - unlifted AArch64 instruction clobbers its defs",
          "[ignil][arm64]") {
  auto dis = makeDisassembler(dragon::Architecture::ARM64);

  SECTION("clz: X0 becomes UNDEF and the address is reported") {
    const std::vector<std::uint8_t> bytes = {
            0x60, 0x00, 0x80, 0xd2, // mov x0, #3      @ 0x1000
            0x20, 0x10, 0xc0, 0xda, // clz x0, x1      @ 0x1004
            0xc0, 0x03, 0x5f, 0xd6, // ret             @ 0x1008
    };
    auto g = makeGraph(dragon::Architecture::ARM64);
    dis.disassemble(bytes, 0x1000, g);
    const dragon::ignil::Block *blk = g.getIGNILBlock(0x1000);
    REQUIRE(blk != nullptr);

    const dragon::ignil::Op *lastX0 = nullptr;
    for (const auto &op: *blk)
      if (op.dst && std::holds_alternative<dragon::ignil::Reg>(*op.dst) &&
          std::get<dragon::ignil::Reg>(*op.dst).offset == arm::X0.offset)
        lastX0 = &op;
    REQUIRE(lastX0 != nullptr);
    CHECK(lastX0->opcode == dragon::ignil::Opcode::UNDEF);
    CHECK(lastX0->address == 0x1004);
    CHECK(blk->unlifted() == std::vector<std::uint64_t>{0x1004});
  }

  SECTION("adcs clobbers NZCV; nop is not a gap") {
    const std::vector<std::uint8_t> bytes = {
            0x20, 0x00, 0x02, 0xba, // adcs x0, x1, x2 @ 0x1000
            0x1f, 0x20, 0x03, 0xd5, // nop             @ 0x1004
            0xc0, 0x03, 0x5f, 0xd6, // ret             @ 0x1008
    };
    auto g = makeGraph(dragon::Architecture::ARM64);
    dis.disassemble(bytes, 0x1000, g);
    const dragon::ignil::Block *blk = g.getIGNILBlock(0x1000);
    REQUIRE(blk != nullptr);
    CHECK(blockDefines(*blk, arm::NF));
    CHECK(blockDefines(*blk, arm::ZF));
    CHECK(blockDefines(*blk, arm::CF));
    CHECK(blockDefines(*blk, arm::VF));
    CHECK(blk->unlifted() == std::vector<std::uint64_t>{0x1000});
  }
}

// -----------------------------------------------------------------------
// ROR-shifted logical operands. Regression: emitShiftedReg emitted UNDEF for
// ROR (IGNIL has no rotate), so `eor x0, x1, x2, ror #9` lost its value.
// Found by differential testing against Unicorn; expected values below are
// Unicorn's.
// -----------------------------------------------------------------------

#ifdef DRAGON_WITH_Z3
TEST_CASE("ARM IGNIL lifting - ROR-shifted logical operand", "[ignil][arm64]") {
  struct Case {
    std::uint32_t word;     // instruction encoding
    std::uint64_t x1, x2;   // inputs
    std::uint64_t x0;       // expected result (Unicorn)
  };
  const Case cases[] = {
          // eor x0, x1, x2, ror #9
          {0xcac22420, 0x5f0fa65e03262fdf, 0x6da30e2e5275a86a,
           0x5f0fa65e03262fdfULL ^ ((0x6da30e2e5275a86aULL >> 9) |
                                    (0x6da30e2e5275a86aULL << 55))},
          // orr w0, w1, w2, ror #9  (32-bit rotate, result zero-extended)
          {0x2ac22420, 0x455ca3fa737a18bd, 0xff7565d790ffb3e4, 0xf37a7ffd},
  };
  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  for (const auto &c: cases) {
    const std::vector<std::uint8_t> bytes = {
            static_cast<std::uint8_t>(c.word), static_cast<std::uint8_t>(c.word >> 8),
            static_cast<std::uint8_t>(c.word >> 16),
            static_cast<std::uint8_t>(c.word >> 24)};
    auto insns = dis.disassemble(bytes, 0x1000);
    REQUIRE(insns.size() == 1);
    const auto block = dis.liftTrace(insns);
    for (const auto &op: block)
      REQUIRE(op.opcode != dragon::ignil::Opcode::UNDEF);

    dragon::ignil::SymbolicExecutor ex;
    ex.setRegister(arm::X1, c.x1);
    ex.setRegister(arm::X2, c.x2);
    ex.setRegister(arm::X0, 0x1111111111111111);
    REQUIRE(ex.run(block).ok());
    INFO(insns[0].disassembly());
    CHECK(ex.evaluate(ex.readRegister(arm::X0)) == c.x0);
  }
}
#endif

// -----------------------------------------------------------------------
// Register-offset / unscaled loads and stores and extended-register
// ADD/SUB. These were unlifted (UNDEF + Block::unlifted), which is what kept
// gcc's AArch64 jump-table dispatch - `ldrh w3, [x3, w20, uxtw #1]; ...;
// add x3, x0, w3, sxth #2; br x3` - from slicing to anything. Expected values
// are Unicorn's; the same forms (140 templates) pass the Unicorn difftest.
// -----------------------------------------------------------------------

#ifdef DRAGON_WITH_Z3
TEST_CASE("ARM IGNIL lifting - register-offset memory and extended add/sub",
          "[ignil][arm64]") {
  struct Case {
    std::uint32_t word;
    std::uint64_t x1, x2, x9, x10;
    std::uint64_t x0; // Unicorn
  };
  const Case cases[] = {
          {0x8b22a820, 0x123456789abcdef0, 0xfedcba98765487f3, 0x0, 0x0, 0x123456789abafebc}, // add x0, x1, w2, sxth #2
          {0x6b220020, 0x1111111180000005, 0xffffffffffffff07, 0x0, 0x0, 0x7ffffffe}, // subs w0, w1, w2, uxtb
          {0x8b22c020, 0x1000, 0xfffffff0, 0x0, 0x0, 0xff0}, // add x0, x1, w2, sxtw
          {0x786a5920, 0x0, 0x0, 0x200040, 0xdeadbeef00000005, 0xe2bd}, // ldrh w0, [x9, w10, uxtw #1]
          {0x38aa6920, 0x0, 0x0, 0x200040, 0xfffffffffffffffd, 0xffffffffffffffdc}, // ldrsb x0, [x9, x10]
          {0xf86ad920, 0x0, 0x0, 0x200080, 0xabcdef01fffffffe, 0x3e19f4cfaa85603b}, // ldr x0, [x9, w10, sxtw #3]
          {0xb89f4120, 0x0, 0x0, 0x200040, 0x0, 0xfffffffffed9b48f}, // ldursw x0, [x9, #-12]
  };
  std::vector<std::uint8_t> page(0x100);
  for (std::size_t i = 0; i < page.size(); ++i)
    page[i] = static_cast<std::uint8_t>(i * 37 + 11);

  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  for (const auto &c: cases) {
    const std::vector<std::uint8_t> bytes = {
            static_cast<std::uint8_t>(c.word), static_cast<std::uint8_t>(c.word >> 8),
            static_cast<std::uint8_t>(c.word >> 16),
            static_cast<std::uint8_t>(c.word >> 24)};
    auto insns = dis.disassemble(bytes, 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    const auto block = dis.liftTrace(insns);
    REQUIRE(block.unlifted().empty());

    dragon::ignil::SymbolicExecutor ex;
    ex.mapMemory(0x200000, page);
    ex.setRegister(arm::X0, 0x1111111111111111);
    ex.setRegister(arm::X1, c.x1);
    ex.setRegister(arm::X2, c.x2);
    ex.setRegister(arm::X9, c.x9);
    ex.setRegister(arm::X10, c.x10);
    REQUIRE(ex.run(block).ok());
    CHECK(ex.evaluate(ex.readRegister(arm::X0)) == c.x0);
  }
}
#endif

// -----------------------------------------------------------------------
// Integer data processing that was unlifted: bitfield moves and inserts
// (lsl/lsr/asr #, sxtw, ubfx, sbfiz, bfi, ...), shifts by register,
// conditional select/compare, multiply-add/long/high, division, EXTR, REV.
// Expected values are Unicorn's; 220 such templates pass the difftest.
// Inputs: x0 = 0x5555..., x1, x2, x3 below, NZCV = 0110 (Z, C).
// -----------------------------------------------------------------------

#ifdef DRAGON_WITH_Z3
TEST_CASE("ARM IGNIL lifting - integer data processing", "[ignil][arm64]") {
  struct Case {
    std::uint32_t word;
    std::uint64_t x0;
    unsigned nzcv;
  };
  const Case cases[] = {
          {0xb37b2020, 0x5555555555557df5, 0x6}, // bfi x0, x1, #5, #9
          {0x53042c20, 0xde, 0x6},               // ubfx w0, w1, #4, #8
          {0x937e1820, 0xffffffffffffffbc, 0x6}, // sbfiz x0, x1, #2, #7
          {0x1ac22820, 0xffffff89, 0x6},         // asr w0, w1, w2
          {0x9a821420, 0xfedcba99, 0x6},         // csinc x0, x1, x2, ne
          {0xfa510825, 0x5555555555555555, 0xa}, // ccmp x1, #17, #5, eq
          {0x3a421023, 0x5555555555555555, 0x3}, // ccmn w1, w2, #3, ne
          {0x9b427c20, 0xffffffff81b39cb4, 0x6}, // smulh x0, x1, x2
          {0x9ba20c20, 0x9a204c72e039302c, 0x6}, // umaddl x0, w1, w2, x3
          {0x9ac20c20, 0xffffffff80924925, 0x6}, // sdiv x0, x1, x2
          {0x1adf0820, 0x0, 0x6},                // udiv w0, w1, wzr
          {0xdac00420, 0x23816745ab89efcd, 0x6}, // rev16 x0, x1
          {0x93c23420, 0x6f7800000007f6e5, 0x6}, // extr x0, x1, x2, #13
  };
  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  for (const auto &c: cases) {
    const std::vector<std::uint8_t> bytes = {
            static_cast<std::uint8_t>(c.word), static_cast<std::uint8_t>(c.word >> 8),
            static_cast<std::uint8_t>(c.word >> 16),
            static_cast<std::uint8_t>(c.word >> 24)};
    auto insns = dis.disassemble(bytes, 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    const auto block = dis.liftTrace(insns);
    REQUIRE(block.unlifted().empty());

    dragon::ignil::SymbolicExecutor ex;
    ex.setRegister(arm::X0, 0x5555555555555555);
    ex.setRegister(arm::X1, 0x8123456789abcdef);
    ex.setRegister(arm::X2, 0x00000000fedcba98);
    ex.setRegister(arm::X3, 0x1111222233334444);
    ex.setRegister(arm::NF, 0);
    ex.setRegister(arm::ZF, 1);
    ex.setRegister(arm::CF, 1);
    ex.setRegister(arm::VF, 0);
    REQUIRE(ex.run(block).ok());
    CHECK(ex.evaluate(ex.readRegister(arm::X0)) == c.x0);
    const unsigned nzcv =
            (*ex.evaluate(ex.readRegister(arm::NF)) << 3) |
            (*ex.evaluate(ex.readRegister(arm::ZF)) << 2) |
            (*ex.evaluate(ex.readRegister(arm::CF)) << 1) |
            *ex.evaluate(ex.readRegister(arm::VF));
    CHECK(nzcv == c.nzcv);
  }
}
#endif
