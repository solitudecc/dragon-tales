#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

#include <algorithm>
#include <vector>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static dragon::Disassembler makeAnalyzer(dragon::Architecture arch) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  cfg.syntax = dragon::Syntax::INTEL;
  return dragon::Disassembler(arch, cfg);
}

static dragon::Graph makeGraph(dragon::Architecture arch) {
  return dragon::Graph(arch, dragon::Configuration{});
}

static bool hasFunction(const std::vector<dragon::Function> &fns,
                        std::uint64_t addr) {
  return std::any_of(fns.begin(), fns.end(), [addr](const dragon::Function &f) {
    return f.startAddress() == addr;
  });
}

static bool hasBlock(const std::vector<dragon::BasicBlock> &blocks,
                     std::uint64_t addr) {
  return std::any_of(blocks.begin(), blocks.end(),
                     [addr](const dragon::BasicBlock &b) {
                       return b.startAddress() == addr;
                     });
}

// -----------------------------------------------------------------------
// Basic sanity
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis - empty bytes yields empty graph", "[arm][analysis]") {
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble({}, 0x1000, g);
  REQUIRE(g.empty());
  REQUIRE(g.functions().empty());
  REQUIRE(g.blocks().empty());
}

// -----------------------------------------------------------------------
// Prologue detection
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis arm64 - prologue detected as function start",
          "[arm][analysis][arm64]") {
  // 0x1000: stp x29, x30, [sp, #-16]!   a9 bf 7b fd -> LE: fd 7b bf a9
  // 0x1004: mov x29, sp                 (add x29, sp, #0)  fd 03 00 91
  // 0x1008: mov w0, #1                  20 00 80 52
  // 0x100c: ldp x29, x30, [sp], #16     fd 7b c1 a8
  // 0x1010: ret                         c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0xfd, 0x7b, 0xbf, 0xa9, // stp x29, x30, [sp, #-16]!
          0xfd, 0x03, 0x00, 0x91, // mov x29, sp
          0x20, 0x00, 0x80, 0x52, // mov w0, #1
          0xfd, 0x7b, 0xc1, 0xa8, // ldp x29, x30, [sp], #16
          0xc0, 0x03, 0x5f, 0xd6, // ret
  };
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  REQUIRE_FALSE(g.empty());

  SECTION("one function at the base address") {
    auto fns = g.functions();
    REQUIRE(fns.size() == 1);
    REQUIRE(fns[0].startAddress() == 0x1000);
    REQUIRE(fns[0].isValid());
  }

  SECTION("prologue instruction flags") {
    const dragon::Instruction *first = g.get(0x1000);
    REQUIRE(first != nullptr);
    REQUIRE(first->isFunctionStart());
    REQUIRE(first->isPrologue());
    REQUIRE(first->isBlockStart());
  }
}

// -----------------------------------------------------------------------
// Entry address without a recognizable prologue
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis arm64 - frame-pointer-omitting leaf function is "
         "still recognized as a function",
         "[arm][analysis][arm64]") {
  // The exact class of bug documented in ToDo/LifterGaps.md #3: a real
  // leaf function (no calls, so nothing forces x29/x30 preservation) has
  // no `stp x29, x30` / `mov x29, sp` at all, so isCommonFunctionPrologue
  // never matches it. Before seeding baseAddr as a function start
  // unconditionally, graph.functions() came back empty for this even
  // though the code disassembles and lifts to IGNIL perfectly cleanly -
  // matches clang -O1's actual output for `int leaf(int n) { return n+1; }`.
  //
  // 0x1000: add w0, w0, #1   00 04 00 11
  // 0x1004: ret              c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x00, 0x04, 0x00, 0x11, // add w0, w0, #1
          0xc0, 0x03, 0x5f, 0xd6, // ret
  };
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(fns.size() == 1);
  REQUIRE(fns[0].startAddress() == 0x1000);
  REQUIRE(fns[0].isValid());
  REQUIRE(fns[0].numInstructions() == 2);
}

// -----------------------------------------------------------------------
// Call target -> function start
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis arm64 - call target becomes a function start",
          "[arm][analysis][arm64]") {
  // 0x1000: bl 0x1010            94 00 00 04 -> LE: 04 00 00 94
  // 0x1004: ret                  c0 03 5f d6
  // 0x1008: nop                  1f 20 03 d5
  // 0x100c: nop                  1f 20 03 d5
  // 0x1010: stp x29, x30, [sp, #-16]!  fd 7b bf a9
  // 0x1014: mov x29, sp                 fd 03 00 91
  // 0x1018: ldp x29, x30, [sp], #16    fd 7b c1 a8
  // 0x101c: ret                         c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x04, 0x00, 0x00, 0x94, // bl 0x1010
          0xc0, 0x03, 0x5f, 0xd6, // ret          (0x1004)
          0x1f, 0x20, 0x03, 0xd5, // nop          (0x1008)
          0x1f, 0x20, 0x03, 0xd5, // nop          (0x100c)
          0xfd, 0x7b, 0xbf, 0xa9, // stp x29, x30, [sp, #-16]!  (0x1010)
          0xfd, 0x03, 0x00, 0x91, // mov x29, sp                (0x1014)
          0xfd, 0x7b, 0xc1, 0xa8, // ldp x29, x30, [sp], #16    (0x1018)
          0xc0, 0x03, 0x5f, 0xd6, // ret                        (0x101c)
  };
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(hasFunction(fns, 0x1010));

  const dragon::Instruction *callee = g.get(0x1010);
  REQUIRE(callee != nullptr);
  REQUIRE(callee->isFunctionStart());
}

// -----------------------------------------------------------------------
// Block discovery via conditional branch
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis arm64 - conditional branch creates multiple blocks",
          "[arm][analysis][arm64]") {
  // 0x1000: cbz w0, 0x1010       34 00 00 80 -> LE: 80 00 00 34
  // 0x1004: ret                  c0 03 5f d6
  // 0x1008: nop                  1f 20 03 d5
  // 0x100c: nop                  1f 20 03 d5
  // 0x1010: ret                  c0 03 5f d6
  const std::vector<std::uint8_t> bytes = {
          0x80, 0x00, 0x00, 0x34, // cbz w0, 0x1010
          0xc0, 0x03, 0x5f, 0xd6, // ret   (0x1004)
          0x1f, 0x20, 0x03, 0xd5, // nop   (0x1008)
          0x1f, 0x20, 0x03, 0xd5, // nop   (0x100c)
          0xc0, 0x03, 0x5f, 0xd6, // ret   (0x1010)
  };
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  auto blocks = g.blocks();
  REQUIRE(blocks.size() == 3);
  REQUIRE(hasBlock(blocks, 0x1000)); // entry (cbz)
  REQUIRE(hasBlock(blocks, 0x1004)); // fall-through
  REQUIRE(hasBlock(blocks, 0x1010)); // branch target
}

// -----------------------------------------------------------------------
// Full if/else function
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis arm64 - if/else function structure",
          "[arm][analysis][arm64]") {
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

  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  SECTION("all instructions are in the graph") { REQUIRE(g.size() == 10); }

  SECTION("one function detected at the prologue") {
    auto fns = g.functions();
    REQUIRE(fns.size() == 1);
    REQUIRE(fns[0].startAddress() == 0x1000);
    REQUIRE(fns[0].isValid());
  }

  SECTION("three basic blocks: entry, then-branch, else-branch") {
    auto blocks = g.blocks();
    REQUIRE(blocks.size() == 3);
    REQUIRE(hasBlock(blocks, 0x1000)); // entry (up to b.le)
    REQUIRE(hasBlock(blocks, 0x1010)); // then-block
    REQUIRE(hasBlock(blocks, 0x101c)); // else-block
  }

  SECTION("function owns all three blocks") {
    auto fns = g.functions();
    REQUIRE(fns[0].numBlocks() == 3);
    REQUIRE(fns[0].block(0x1000) != nullptr);
    REQUIRE(fns[0].block(0x1010) != nullptr);
    REQUIRE(fns[0].block(0x101c) != nullptr);
  }

  SECTION("entry block contains 4 instructions and terminates on the b.le") {
    auto fns = g.functions();
    const dragon::BasicBlock *entry = fns[0].block(0x1000);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->size() == 4);
    REQUIRE(entry->terminatorAddress() == 0x100c);
    const dragon::Instruction *ble = entry->terminator();
    REQUIRE(ble != nullptr);
    REQUIRE(ble->isJump());
    REQUIRE(ble->isConditional());
  }

  SECTION("then-block and else-block each have 3 instructions ending in ret") {
    auto fns = g.functions();
    const dragon::BasicBlock *then_bb = fns[0].block(0x1010);
    const dragon::BasicBlock *else_bb = fns[0].block(0x101c);
    REQUIRE(then_bb != nullptr);
    REQUIRE(else_bb != nullptr);
    REQUIRE(then_bb->size() == 3);
    REQUIRE(else_bb->size() == 3);
    REQUIRE(then_bb->terminator()->isReturn());
    REQUIRE(else_bb->terminator()->isReturn());
  }

  SECTION("function instruction count is the sum of all blocks") {
    auto fns = g.functions();
    REQUIRE(fns[0].numInstructions() == 10);
  }
}

// -----------------------------------------------------------------------
// Regression: a block ran through the start of a BL target (see the x86
// equivalent in TestX86Analysis.cpp).
// -----------------------------------------------------------------------

TEST_CASE("ARMAnalysis ARM64 - block ends where a bl target begins",
          "[arm64][analysis][blocks]") {
  // 0x1000: mov x0, #5   a0 00 80 d2
  // 0x1004: bl  0x1008   01 00 00 94
  // 0x1008: ret          c0 03 5f d6   <- bl target: its own block
  const std::vector<std::uint8_t> bytes = {0xa0, 0x00, 0x80, 0xd2,
                                           0x01, 0x00, 0x00, 0x94,
                                           0xc0, 0x03, 0x5f, 0xd6};
  auto dis = makeAnalyzer(dragon::Architecture::ARM64);
  auto g = makeGraph(dragon::Architecture::ARM64);
  dis.disassemble(bytes, 0x1000, g);

  dragon::BasicBlock caller{&g, 0x1000};
  REQUIRE(caller.isValid());
  CHECK(caller.terminatorAddress() == 0x1004);

  const auto *ib = g.getIGNILBlock(0x1000);
  REQUIRE(ib != nullptr);
  for (const auto &op: *ib)
    CHECK(op.address != 0x1008);
  CHECK(ib->ops().back().opcode == dragon::ignil::Opcode::JMP);
  CHECK(g.get(0x1008)->isFunctionStart());
}
