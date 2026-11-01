#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>

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
// trace, so `mov x0,#3; mul x0,x0,x1; ret` read back as X0 = 3.
// -----------------------------------------------------------------------

TEST_CASE("ARM IGNIL lifting - unlifted AArch64 instruction clobbers its defs",
          "[ignil][arm64]") {
  auto dis = makeDisassembler(dragon::Architecture::ARM64);

  SECTION("mul: X0 becomes UNDEF and the address is reported") {
    const std::vector<std::uint8_t> bytes = {
            0x60, 0x00, 0x80, 0xd2, // mov x0, #3      @ 0x1000
            0x00, 0x7c, 0x01, 0x9b, // mul x0, x0, x1  @ 0x1004
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
