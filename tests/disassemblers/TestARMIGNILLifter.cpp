#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/assembler/Assembler.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#ifdef DRAGON_WITH_Z3
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace arm = dragon::arm;

#ifdef DRAGON_WITH_Z3
TEST_CASE("ARM scalar and SIMD transfers preserve raw bits and clear scalar upper bits",
          "[arm-vector-data][ignil][arm64]") {
  using namespace dragon;
  struct Case { const char *text; uint64_t low, high; };
  constexpr uint64_t input = 0x7ff0000000000011ull;
  const Case cases[] = {
    {"fmov d0, x1",input,0}, {"fmov s0, w1",0x11,0},
    {"fmov d0, #1.0",0x3ff0000000000000ull,0},
    {"fmov s0, #-2.0",0xc0000000,0},
    {"ldr d0, [x9]",0x1122334455667788ull,0},
    {"ldr q0, [x9]",0x1122334455667788ull,0x99aabbccddeeff00ull},
    {"ldur b0, [x9, #1]",0x77,0}, {"ldur h0, [x9, #1]",0x6677,0},
    {"ldur s0, [x9, #3]",0x22334455,0},
    {"ldur d0, [x9, #1]",0x0011223344556677ull,0},
    {"ldur q0, [x9]",0x1122334455667788ull,0x99aabbccddeeff00ull},
    {"movi v0.16b, #0x81",0x8181818181818181ull,0x8181818181818181ull},
    {"movi d0, #0xff00ff00ff00ff00",0xff00ff00ff00ff00ull,0},
    {"movi v0.4s, #0x12, msl #8",0x000012ff000012ffull,0x000012ff000012ffull},
    {"dup v0.2d, x1",input,input},
    {"ins v0.d[1], x1",0x1234,input},
    {"fmov v0.d[1], x1",0x1234,input},
  };
  const ignil::Reg low{arm::kOffV0,64,"V0.low"}, high{arm::kOffV0+8,64,"V0.high"};
  Assembler assembler{Architecture::ARM64,Configuration{}};
  Disassembler dis{Architecture::ARM64,Configuration{}};
  for (const auto &test : cases) {
    INFO(test.text);
    const auto instructions = dis.disassemble(assembler.assemble(test.text),0x1000);
    REQUIRE(instructions.size() == 1);
    const auto block = dis.liftTrace(instructions);
    REQUIRE(block.unlifted().empty());
    ignil::SymbolicExecutor ex;
    ex.setRegister(arm::X1,input);
    ex.setRegister(arm::X9,0x200000);
    ex.setRegister(low,0x1234);
    ex.setRegister(high,~0ull);
    std::vector<uint8_t> data;
    for (uint64_t value : {0x1122334455667788ull,0x99aabbccddeeff00ull})
      for (unsigned byte=0; byte<8; ++byte) data.push_back(value >> (byte*8));
    ex.mapMemory(0x200000,data);
    REQUIRE(ex.run(block).ok());
    CHECK(ex.evaluate(ex.readRegister(low)) == test.low);
    CHECK(ex.evaluate(ex.readRegister(high)) == test.high);
  }
}

TEST_CASE("ARM CLZ and RBIT include zero and in-place operands", "[arm-vector-data][ignil][arm64]") {
  using namespace dragon;
  Assembler assembler{Architecture::ARM64,Configuration{}};
  Disassembler dis{Architecture::ARM64,Configuration{}};
  for (const auto &[instruction, input, expected] :
       std::vector<std::tuple<std::string,uint64_t,uint64_t>>{
           {"clz x1, x1",0,64},{"clz x1, x1",1,63},
           {"clz w1, w1",0xffff000000000001ull,31},
           {"rbit x1, x1",1,0x8000000000000000ull},
           {"rbit w1, w1",0xffff000000000002ull,0x40000000},
           {"fmov x1, v0.d[1]",0x7ff0000000000011ull,0x7ff0000000000011ull}}) {
    INFO(instruction);
    const auto block = dis.liftTrace(dis.disassemble(assembler.assemble(instruction),0x1000));
    REQUIRE(block.unlifted().empty());
    ignil::SymbolicExecutor ex;
    ex.setRegister(arm::X1,input);
    ex.setRegister(ignil::Reg{arm::kOffV0+8,64,"V0.high"},input);
    REQUIRE(ex.run(block).ok());
    CHECK(ex.evaluate(ex.readRegister(arm::X1)) == expected);
  }
}

TEST_CASE("ARM floating integer conversions saturate and zero NaNs", "[arm-fp][ignil][arm64]") {
  using namespace dragon;
  struct Case { const char *text; uint64_t source, expected; };
  const Case cases[] = {
    {"fcvtzs x0, d1",0x7ff8000000000001ull,0},
    {"fcvtzs x0, d1",0x7ff0000000000000ull,0x7fffffffffffffffull},
    {"fcvtzs x0, d1",0xfff0000000000000ull,0x8000000000000000ull},
    {"fcvtzs w0, d1",0x41e0000000000000ull,0x7fffffffull},
    {"fcvtzs w0, d1",0xfff0000000000000ull,0x80000000ull},
    {"fcvtzu x0, d1",0xfff0000000000000ull,0},
    {"fcvtzu x0, d1",0x7ff0000000000000ull,~0ull},
    {"fcvtzs x0, s1",0x7f800000,0x7fffffffffffffffull},
    {"fcvtzu w0, s1",0x7f800000,0xffffffffull},
    {"fcvtzu w0, s1",0x7fc00001,0},
  };
  Assembler assembler{Architecture::ARM64,Configuration{}};
  Disassembler dis{Architecture::ARM64,Configuration{}};
  for (const auto &test : cases) {
    INFO(test.text);
    const auto block = dis.liftTrace(dis.disassemble(assembler.assemble(test.text),0x1000));
    REQUIRE(block.unlifted().empty());
    ignil::SymbolicExecutor executor;
    executor.setRegister(ignil::Reg{arm::kOffV1,64,"V1.low"},test.source);
    REQUIRE(executor.run(block).ok());
    CHECK(executor.evaluate(executor.readRegister(arm::X0)) == test.expected);
  }
}

TEST_CASE("ARM unordered floating comparisons set NZCV", "[arm-fp][ignil][arm64]") {
  using namespace dragon;
  Assembler assembler{Architecture::ARM64,Configuration{}};
  Disassembler dis{Architecture::ARM64,Configuration{}};
  for (const auto *text : {"fcmp d0, d1","fcmpe d0, #0.0"}) {
    auto block = dis.liftTrace(dis.disassemble(assembler.assemble(text),0x1000));
    REQUIRE(block.unlifted().empty());
    ignil::SymbolicExecutor executor;
    executor.setRegister(arm::D0,0x7ff8000000000001ull);
    executor.setRegister(arm::D1,0x3ff0000000000000ull);
    REQUIRE(executor.run(block).ok());
    CHECK(executor.evaluate(executor.readRegister(arm::NF)) == 0);
    CHECK(executor.evaluate(executor.readRegister(arm::ZF)) == 0);
    CHECK(executor.evaluate(executor.readRegister(arm::CF)) == 1);
    CHECK(executor.evaluate(executor.readRegister(arm::VF)) == 1);
  }
}
#endif

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

  SECTION("cls: X0 becomes UNDEF and the address is reported") {
    const std::vector<std::uint8_t> bytes = {
            0x60, 0x00, 0x80, 0xd2, // mov x0, #3      @ 0x1000
            0x20, 0x14, 0xc0, 0xda, // cls x0, x1      @ 0x1004
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

#ifdef DRAGON_WITH_Z3
// `subs w20, w20, #1` (a loop counter) / `adds x1, x1, x1`: Rd is also an
// operand. C and V were computed from the operand read after Rd had been
// written, i.e. from the result. Found by the real-block Unicorn difftest.
TEST_CASE("ARM IGNIL lifting - adds/subs with Rd == Rn", "[ignil][arm64]") {
  struct Case {
    std::uint32_t word;
    std::uint64_t x1;
    unsigned nzcv;
  };
  const Case cases[] = {
          {0xf1000421, 0x0, 0x8},                // subs x1, x1, #1   (0-1)
          {0xab010021, 0x8000000000000000, 0x7}, // adds x1, x1, x1   (2^63*2)
          {0x71000421, 0x1, 0x6},                // subs w1, w1, #1   (1-1)
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
    dragon::ignil::SymbolicExecutor ex;
    ex.setRegister(arm::X1, c.x1);
    REQUIRE(ex.run(block).ok());
    const unsigned nzcv =
            (*ex.evaluate(ex.readRegister(arm::NF)) << 3) |
            (*ex.evaluate(ex.readRegister(arm::ZF)) << 2) |
            (*ex.evaluate(ex.readRegister(arm::CF)) << 1) |
            *ex.evaluate(ex.readRegister(arm::VF));
    CHECK(nzcv == c.nzcv);
  }
}
#endif

// -----------------------------------------------------------------------
// Regression: LLVM marks store-release (STLR, STLLR) and several other real
// stores with "unmodeled side effects", the flag used to recognise nops and
// barriers, so they were treated as no-ops - `stlr w1, [x0]` lifted to
// nothing and the store vanished from the IR without being reported. Now
// the acquire/release forms are lifted as plain loads/stores, the other
// side-effect stores (and SYS: `dc zva`) are reported unlifted, and only
// barriers/hints/prefetches stay no-ops.
// -----------------------------------------------------------------------
namespace {
std::vector<std::uint8_t> a64Word(std::uint32_t w) {
  return {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
          static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24)};
}
} // namespace

TEST_CASE("ARM IGNIL lifting - side-effect stores are not no-ops",
          "[ignil][arm64]") {
  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  // Reported as unlifted: stores without a lifter.
  for (const std::uint32_t w: {0xd50b7420u,   // dc zva, x0
                               0xd9a00001u,   // stgm x1, [x0]
                               0xf83f9002u}) { // st64b x2, [x0]
    auto insns = dis.disassemble(a64Word(w), 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    CHECK_FALSE(dis.liftTrace(insns).unlifted().empty());
  }
  // Still no-ops: barrier and prefetch write nothing.
  for (const std::uint32_t w: {0xd5033bbfu,   // dmb ish
                               0xf9800000u}) { // prfm pldl1keep, [x0]
    auto insns = dis.disassemble(a64Word(w), 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    CHECK(dis.liftTrace(insns).unlifted().empty());
  }
}

#ifdef DRAGON_WITH_Z3
TEST_CASE("ARM IGNIL lifting - acquire/release loads and stores",
          "[ignil][arm64]") {
  std::vector<std::uint8_t> page(0x100);
  for (std::size_t i = 0; i < page.size(); ++i)
    page[i] = static_cast<std::uint8_t>(i * 37 + 11);
  auto le = [&](std::size_t off, unsigned bytes) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i)
      v |= std::uint64_t{page[off + i]} << (8 * i);
    return v;
  };
  constexpr std::uint64_t kX1 = 0x1122334455667788;

  struct Store {
    std::uint32_t word;
    std::uint64_t at;
    unsigned bytes;
  };
  const Store stores[] = {
          {0x889ffc01, 0x200040, 4}, // stlr w1, [x0]
          {0xc89ffc01, 0x200040, 8}, // stlr x1, [x0]
          {0x089ffc01, 0x200040, 1}, // stlrb w1, [x0]
          {0x489ffc01, 0x200040, 2}, // stlrh w1, [x0]
          {0xc89f7c01, 0x200040, 8}, // stllr x1, [x0]
          {0x991fc001, 0x20003c, 4}, // stlur w1, [x0, #-4]
  };
  auto dis = makeDisassembler(dragon::Architecture::ARM64);
  for (const auto &c: stores) {
    auto insns = dis.disassemble(a64Word(c.word), 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    const auto block = dis.liftTrace(insns);
    REQUIRE(block.unlifted().empty());
    dragon::ignil::SymbolicExecutor ex;
    ex.mapMemory(0x200000, page);
    ex.setRegister(arm::X0, 0x200040);
    ex.setRegister(arm::X1, kX1);
    REQUIRE(ex.run(block).ok());
    const std::uint64_t mask = c.bytes == 8 ? ~0ull : (1ull << (8 * c.bytes)) - 1;
    CHECK(ex.evaluate(ex.readMemory(c.at, 8 * c.bytes)) == (kX1 & mask));
    // the byte after the store is untouched
    CHECK(ex.evaluate(ex.readMemory(c.at + c.bytes, 8)) ==
          le(c.at + c.bytes - 0x200000, 1));
  }

  struct Load {
    std::uint32_t word;
    std::size_t off;
    unsigned bytes;
  };
  const Load loads[] = {
          {0xc8dffd20, 0x40, 8}, // ldar x0, [x9]
          {0x08dffd20, 0x40, 1}, // ldarb w0, [x9]
          {0xb8bfc120, 0x40, 4}, // ldapr w0, [x9]
          {0xd9408120, 0x48, 8}, // ldapur x0, [x9, #8]
  };
  for (const auto &c: loads) {
    auto insns = dis.disassemble(a64Word(c.word), 0x1000);
    REQUIRE(insns.size() == 1);
    INFO(insns[0].disassembly());
    const auto block = dis.liftTrace(insns);
    REQUIRE(block.unlifted().empty());
    dragon::ignil::SymbolicExecutor ex;
    ex.mapMemory(0x200000, page);
    ex.setRegister(arm::X0, 0xffffffffffffffff);
    ex.setRegister(arm::X9, 0x200040);
    REQUIRE(ex.run(block).ok());
    CHECK(ex.evaluate(ex.readRegister(arm::X0)) == le(c.off, c.bytes));
  }
}
#endif
