#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>

#include <algorithm>
#include <set>
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

TEST_CASE("X86Analysis - empty bytes yields empty graph", "[x86][analysis]") {
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble({}, 0x1000, g);
  REQUIRE(g.empty());
  REQUIRE(g.functions().empty());
  REQUIRE(g.blocks().empty());
}

// -----------------------------------------------------------------------
// Prologue detection
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - prologue detected as function start",
          "[x86][analysis][x86_64]") {
  // push rbp      55
  // mov rbp, rsp  48 89 E5
  // xor eax, eax  31 C0
  // pop rbp       5D
  // ret           C3
  const std::vector<std::uint8_t> bytes = {
          0x55, 0x48, 0x89, 0xE5, // push rbp; mov rbp, rsp
          0x31, 0xC0, // xor eax, eax
          0x5D, 0xC3, // pop rbp; ret
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
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

TEST_CASE("X86Analysis x86_32 - prologue detected as function start",
          "[x86][analysis][x86_32]") {
  // push ebp      55
  // mov ebp, esp  89 E5
  // xor eax, eax  31 C0
  // pop ebp       5D
  // ret           C3
  const std::vector<std::uint8_t> bytes = {
          0x55, 0x89, 0xE5, // push ebp; mov ebp, esp
          0x31, 0xC0, // xor eax, eax
          0x5D, 0xC3, // pop ebp; ret
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_32);
  auto g = makeGraph(dragon::Architecture::X86_32);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(fns.size() == 1);
  REQUIRE(fns[0].startAddress() == 0x1000);
  REQUIRE(fns[0].isValid());
}

// -----------------------------------------------------------------------
// Entry address without a recognizable prologue
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - leaf function with no push-rbp prologue is "
         "still recognized as a function",
         "[x86][analysis][x86_64]") {
  // A frame-pointer-omitted leaf function - the shape isCommonFunctionPrologue
  // has never recognized (no push rbp / mov rbp, rsp, no endbr64). Before
  // seeding baseAddr as a function start unconditionally, disassembling this
  // starting at its own entry produced zero functions even though every
  // instruction lifts cleanly - the exact class of bug documented for
  // ARM64 in ToDo/LifterGaps.md #3, just the x86 equivalent.
  //
  // sub rsp, 0x10                 48 83 EC 10
  // mov dword ptr [rsp], edi      89 3C 24
  // mov eax, dword ptr [rsp]      8B 04 24
  // add eax, 1                    83 C0 01
  // add rsp, 0x10                 48 83 C4 10
  // ret                           C3
  const std::vector<std::uint8_t> bytes = {
          0x48, 0x83, 0xec, 0x10, // sub rsp, 0x10
          0x89, 0x3c, 0x24,       // mov [rsp], edi
          0x8b, 0x04, 0x24,       // mov eax, [rsp]
          0x83, 0xc0, 0x01,       // add eax, 1
          0x48, 0x83, 0xc4, 0x10, // add rsp, 0x10
          0xc3,                   // ret
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(fns.size() == 1);
  REQUIRE(fns[0].startAddress() == 0x1000);
  REQUIRE(fns[0].isValid());
  REQUIRE(fns[0].numInstructions() == 6);
}

// -----------------------------------------------------------------------
// Call target → function start
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - call target becomes a function start",
          "[x86][analysis][x86_64]") {
  // 0x1000: E8 0A 00 00 00  call 0x100F  (0x1000 + 5 + 0xA = 0x100F)
  // 0x1005: C3              ret
  // 0x1006-0x100E: 90 * 9  padding
  // 0x100F: 55 48 89 E5     push rbp; mov rbp, rsp
  // 0x1013: C3              ret
  const std::vector<std::uint8_t> bytes = {
          0xE8, 0x0A, 0x00, 0x00, 0x00, // call 0x100F
          0xC3, // ret   (0x1005)
          0x90, 0x90, 0x90, 0x90, 0x90,
          0x90, 0x90, 0x90, 0x90, // 9 nops (0x1006-0x100E)
          0x55, 0x48, 0x89, 0xE5, // push rbp; mov rbp, rsp (0x100F)
          0xC3, // ret   (0x1013)
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(hasFunction(fns, 0x100F));

  const dragon::Instruction *callee = g.get(0x100F);
  REQUIRE(callee != nullptr);
  REQUIRE(callee->isFunctionStart());
}

// -----------------------------------------------------------------------
// Block discovery via conditional branch
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis - INT3 alignment does not create a function",
          "[x86][analysis][int3-padding]") {
  // lld fills the gap between compiled functions with 0xcc. The bytes at
  // 0x1001..0x1003 are padding, not an extra function before the leaf at 1004.
  const std::vector<std::uint8_t> bytes = {
      0xc3, 0xcc, 0xcc, 0x90, 0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
  for (auto arch : {dragon::Architecture::X86_32, dragon::Architecture::X86_64}) {
    auto dis = makeAnalyzer(arch);
    auto graph = makeGraph(arch);
    dis.disassemble(bytes, 0x1000, graph);
    REQUIRE(graph.validFunctionAddresses() == std::set<std::uint64_t>{0x1000, 0x1004});
    REQUIRE_FALSE(graph.get(0x1001)->isFunctionStart());
    REQUIRE_FALSE(graph.get(0x1002)->isFunctionStart());
  }
}

TEST_CASE("X86Analysis - explicit INT3 entry survives alignment discovery",
          "[x86][analysis][int3-padding]") {
  const std::vector<std::uint8_t> bytes = {0xc3, 0xcc, 0x90, 0xc3};
  for (auto arch : {dragon::Architecture::X86_32, dragon::Architecture::X86_64}) {
    auto dis = makeAnalyzer(arch);
    auto graph = makeGraph(arch);
    dragon::Disassembler::AnalysisHints hints;
    hints.entryPoints = {0x1000, 0x1001};
    dis.disassemble(bytes, 0x1000, graph, hints);
    REQUIRE(graph.validFunctionAddresses() == std::set<std::uint64_t>{0x1000, 0x1001, 0x1003});
    REQUIRE(graph.get(0x1001)->isFunctionStart());
    // It remains an actual breakpoint instruction, never rewritten as a NOP.
    REQUIRE(graph.get(0x1001)->bytes()[0] == 0xcc);
  }
}

TEST_CASE("X86Analysis x86_32 - RET imm16 discovers the following leaf",
          "[x86][analysis][int3-padding]") {
  // LLVM 21 calls these opcodes RET32/RETI32 rather than RETL/RETIL.
  const std::vector<std::uint8_t> bytes = {
      0xc2, 0x08, 0x00, 0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
  auto dis = makeAnalyzer(dragon::Architecture::X86_32);
  auto graph = makeGraph(dragon::Architecture::X86_32);
  dis.disassemble(bytes, 0x1000, graph);
  REQUIRE(graph.validFunctionAddresses() == std::set<std::uint64_t>{0x1000, 0x1003});
  REQUIRE(graph.get(0x1003)->isFunctionStart());
}

TEST_CASE("X86Analysis - unresolved indirect cases do not seed functions",
          "[x86][analysis][int3-padding]") {
  // Unknown dispatch followed by two generated case bodies. A RET alone
  // cannot establish that the next body is a different function. An
  // explicitly separate function, however, starts a fresh analysis region.
  const std::vector<std::uint8_t> bytes = {
      0xff, 0xe0, 0xb8, 1, 0, 0, 0, 0xc3, 0xb8, 2, 0, 0, 0, 0xc3};
  for (bool separateFunction : {false, true}) {
    DYNAMIC_SECTION("known separate function " << separateFunction) {
      auto dis = makeAnalyzer(dragon::Architecture::X86_64);
      auto graph = makeGraph(dragon::Architecture::X86_64);
      dragon::Disassembler::AnalysisHints hints;
      if (separateFunction)
        hints.entryPoints = {0x1000, 0x1002};
      dis.disassemble(bytes, 0x1000, graph, hints);
      if (separateFunction) {
        REQUIRE(graph.validFunctionAddresses() == std::set<std::uint64_t>{0x1000, 0x1002, 0x1008});
        REQUIRE(graph.get(0x1008)->isFunctionStart());
      } else {
        REQUIRE(graph.validFunctionAddresses() == std::set<std::uint64_t>{0x1000});
        REQUIRE_FALSE(graph.get(0x1008)->isFunctionStart());
      }
    }
  }
}

TEST_CASE("X86Analysis - data references keep post RET entries ambiguous",
          "[x86][analysis][int3-padding]") {
  for (auto arch : {dragon::Architecture::X86_32, dragon::Architecture::X86_64}) {
    for (bool explicitCode : {false, true}) {
      DYNAMIC_SECTION("arch " << static_cast<int>(arch) << " explicit code " << explicitCode) {
        // Read the bytes at 1010 as data, then return. The same bytes can
        // also encode a leaf, but layout alone cannot certify that entry.
        // Explicit entry evidence must still preserve such dual use.
        std::vector<std::uint8_t> bytes = {0x8b, 0x05, 0, 0, 0, 0, 0xc3};
        const std::uint32_t displacement = arch == dragon::Architecture::X86_32 ? 0x1010 : 10;
        for (unsigned i=0; i<4; ++i) bytes[2+i] = displacement >> (8*i);
        bytes.resize(16, 0x90);
        bytes.insert(bytes.end(), {0xb8, 7, 0, 0, 0, 0xc3, 0x90, 0x90,
                                   0xb8, 9, 0, 0, 0, 0xc3});
        auto dis = makeAnalyzer(arch);
        auto graph = makeGraph(arch);
        dragon::Disassembler::AnalysisHints hints;
        hints.entryPoints = {0x1000, 0x1018};
        if (explicitCode) hints.entryPoints.push_back(0x1010);
        dis.disassemble(bytes, 0x1000, graph, hints);
        const auto expected = explicitCode
            ? std::set<std::uint64_t>{0x1000, 0x1010, 0x1018}
            : std::set<std::uint64_t>{0x1000, 0x1018};
        REQUIRE(graph.validFunctionAddresses() == expected);
        REQUIRE(graph.get(0x1010)->isFunctionStart() == explicitCode);
        REQUIRE(graph.get(0x1018)->isFunctionStart());
      }
    }
  }
}

TEST_CASE("X86Analysis x86_64 - conditional branch creates multiple blocks",
          "[x86][analysis][x86_64]") {
  // 0x1000: 74 05  je  0x1007  (0x1000 + 2 + 5 = 0x1007)
  // 0x1002: C3     ret
  // 0x1003-0x1006: 90 90 90 90  padding
  // 0x1007: C3     ret
  const std::vector<std::uint8_t> bytes = {
          0x74, 0x05, // je 0x1007
          0xC3, // ret  (0x1002)
          0x90, 0x90, 0x90, 0x90, // padding
          0xC3, // ret  (0x1007)
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  auto blocks = g.blocks();
  REQUIRE(blocks.size() == 3);
  REQUIRE(hasBlock(blocks, 0x1000)); // entry (je)
  REQUIRE(hasBlock(blocks, 0x1002)); // fall-through
  REQUIRE(hasBlock(blocks, 0x1007)); // branch target
}

// -----------------------------------------------------------------------
// Overlapping instruction decodes (anti-disassembly trick) - found
// analyzing a real obfusheader.h-obfuscated binary (examples/obfusheader/,
// ToDo/Roadmap.md's "Not started" section originally, now fixed). A
// conditional jump's target can land in the middle of the bytes the
// fall-through path already decoded as a different instruction - two
// legitimately different, valid decodes sharing overlapping bytes. Real
// hardware only ever executes one of them per run; a static analysis has
// to keep both views without letting one corrupt the other.
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - branch target overlapping the fall-through's "
          "own bytes decodes independently on both paths",
          "[x86][analysis][x86_64][overlap]") {
  // 0x1000: 31 c0        xor eax, eax        ; ZF=1
  // 0x1002: 74 02        je 0x1006           ; always taken in practice
  // 0x1004: b8 90 c3 34 12  mov eax, 0x1234c390  (5 bytes: 0x1004-0x1008)
  // 0x1009: c3           ret
  //
  // 0x1006 (the je's target) is byte index 2 of that mov instruction (the
  // low byte of its imm32, value 0xc3) - decoded fresh from there instead
  // of inherited from the fall-through's own decode, it's a different,
  // equally valid instruction:
  // 0x1006: c3           ret
  //
  // Verified by hand with objdump from both offsets before writing this
  // (see ToDo/Roadmap.md) - not a hand-guessed encoding. Also verified via
  // a scratch driver that both blocks below are exactly 1 instruction each
  // (not 2/0) - a bare `ret` is itself recognized as a common function
  // epilogue, and the address right after *any* `ret` (0x1006's as much as
  // 0x1009's) is a real, independent block/function-start heuristic in
  // its own right, unrelated to the overlap trick this test targets. That
  // heuristic is why the fall-through's mov ends up alone in its own
  // block rather than merged with the trailing ret - correct, expected
  // behavior, not part of what's being tested here.
  const std::vector<std::uint8_t> bytes = {
          0x31, 0xc0,             // xor eax, eax   (0x1000)
          0x74, 0x02,             // je 0x1006      (0x1002)
          0xb8, 0x90, 0xc3, 0x34, 0x12, // mov eax, 0x1234c390 (0x1004)
          0xc3,                   // ret            (0x1009)
  };

  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  cfg.syntax = dragon::Syntax::INTEL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  auto blocks = g.blocks();
  REQUIRE(hasBlock(blocks, 0x1000)); // entry (xor; je)
  REQUIRE(hasBlock(blocks, 0x1004)); // fall-through (mov)
  REQUIRE(hasBlock(blocks, 0x1006)); // branch target (ret) - the overlap
  REQUIRE(hasBlock(blocks, 0x1009)); // fall-through's own trailing ret

  auto blockAt = [&](std::uint64_t addr) -> const dragon::BasicBlock & {
    auto it = std::find_if(blocks.begin(), blocks.end(),
                           [addr](const dragon::BasicBlock &b) {
                             return b.startAddress() == addr;
                           });
    REQUIRE(it != blocks.end());
    return *it;
  };

  // The fall-through path genuinely is the 5-byte mov - unaffected by the
  // other, overlapping path also existing in the same graph.
  const auto &fallThrough = blockAt(0x1004);
  auto fallInsns = fallThrough.instructions();
  REQUIRE(fallInsns.size() == 1);
  REQUIRE(fallInsns[0]->address() == 0x1004);

  // The branch target must be its OWN independent decode (just `ret`),
  // not silently reuse whatever instruction happens to be nearest in the
  // shared listing (0x1009's ret, wrongly picked up as this block's own
  // instruction because it sorts right after 0x1004-0x1008 in the shared
  // listing, was the actual bug this test pins down).
  const auto &target = blockAt(0x1006);
  auto targetInsns = target.instructions();
  REQUIRE(targetInsns.size() == 1);
  REQUIRE(targetInsns[0]->address() == 0x1006);
}

// -----------------------------------------------------------------------
// Full if/else function
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - if/else function structure",
          "[x86][analysis][x86_64]") {
  // Equivalent C:  int foo(int x) { if (x > 0) return 1; else return -1; }
  //
  // 0x1000: 55               push rbp
  // 0x1001: 48 89 E5         mov rbp, rsp
  // 0x1004: 85 FF            test edi, edi
  // 0x1006: 7E 07            jle +7  →  0x100F  (else-block)
  // -- then-block ---
  // 0x1008: B8 01 00 00 00   mov eax, 1
  // 0x100D: 5D               pop rbp
  // 0x100E: C3               ret
  // -- else-block ---
  // 0x100F: B8 FF FF FF FF   mov eax, -1
  // 0x1014: 5D               pop rbp
  // 0x1015: C3               ret
  const std::vector<std::uint8_t> bytes = {
          0x55, // push rbp        (0x1000)
          0x48, 0x89, 0xE5, // mov rbp, rsp    (0x1001)
          0x85, 0xFF, // test edi, edi   (0x1004)
          0x7E, 0x07, // jle +7          (0x1006)
          0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1      (0x1008)
          0x5D, // pop rbp         (0x100D)
          0xC3, // ret             (0x100E)
          0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov eax, -1     (0x100F)
          0x5D, // pop rbp         (0x1014)
          0xC3, // ret             (0x1015)
  };

  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
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
    REQUIRE(hasBlock(blocks, 0x1000)); // entry (up to jle)
    REQUIRE(hasBlock(blocks, 0x1008)); // then-block
    REQUIRE(hasBlock(blocks, 0x100F)); // else-block
  }

  SECTION("function owns all three blocks") {
    auto fns = g.functions();
    REQUIRE(fns[0].numBlocks() == 3);
    REQUIRE(fns[0].block(0x1000) != nullptr);
    REQUIRE(fns[0].block(0x1008) != nullptr);
    REQUIRE(fns[0].block(0x100F) != nullptr);
  }

  SECTION("entry block contains 4 instructions and terminates on the jle") {
    auto fns = g.functions();
    const dragon::BasicBlock *entry = fns[0].block(0x1000);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->size() == 4);
    REQUIRE(entry->terminatorAddress() == 0x1006);
    const dragon::Instruction *jle = entry->terminator();
    REQUIRE(jle != nullptr);
    REQUIRE(jle->isJump());
    REQUIRE(jle->isConditional());
  }

  SECTION("then-block and else-block each have 3 instructions ending in ret") {
    auto fns = g.functions();
    const dragon::BasicBlock *then_bb = fns[0].block(0x1008);
    const dragon::BasicBlock *else_bb = fns[0].block(0x100F);
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
// Two adjacent functions - function boundary stops BFS
// -----------------------------------------------------------------------

TEST_CASE("X86Analysis x86_64 - two adjacent prologues produce two distinct "
          "functions",
          "[x86][analysis][x86_64]") {
  // Function 1 (0x1000): push rbp; mov rbp, rsp; pop rbp  - no ret, falls
  // through Function 2 (0x1005): push rbp; mov rbp, rsp; pop rbp; ret
  //
  // Without the function-boundary BFS guard in Function.cpp, function 1 would
  // follow the fall-through successor at 0x1005 and absorb function 2's blocks.
  const std::vector<std::uint8_t> bytes = {
          0x55, 0x48, 0x89, 0xE5, 0x5D, // push rbp; mov rbp, rsp; pop rbp (fn1)
          0x55, 0x48, 0x89, 0xE5, 0x5D,
          0xC3, // push rbp; mov rbp, rsp; pop rbp; ret (fn2)
  };
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  auto fns = g.functions();
  REQUIRE(fns.size() == 2);
  REQUIRE(hasFunction(fns, 0x1000));
  REQUIRE(hasFunction(fns, 0x1005));

  SECTION("function 1 does not absorb function 2 blocks") {
    // functions() returns in address order (std::set iteration)
    REQUIRE(fns[0].startAddress() == 0x1000);
    REQUIRE(fns[0].numBlocks() == 1);
    REQUIRE(fns[0].block(0x1005) == nullptr);
  }

  SECTION("function 2 is self-contained") {
    REQUIRE(fns[1].startAddress() == 0x1005);
    REQUIRE(fns[1].isValid());
  }
}

// -----------------------------------------------------------------------
// Block boundaries at call targets and resolved indirect-jump targets.
//
// Regression: BasicBlock decided where a block ends from the instruction's
// isBlockStart flag only, but call targets and resolved indirect-jump targets
// were registered with Graph::addBlock() without flagging the instruction
// already at that address. A block then ran straight through the start of the
// next one, so two blocks (and their IGNIL) held the same instructions.
// -----------------------------------------------------------------------

static std::vector<std::uint64_t> addressesOf(const dragon::BasicBlock &bb) {
  std::vector<std::uint64_t> out;
  for (const auto *insn: bb.instructions())
    out.push_back(insn->address());
  return out;
}

static bool ignilTouches(const dragon::ignil::Block &ib, std::uint64_t addr) {
  return std::any_of(ib.begin(), ib.end(), [addr](const dragon::ignil::Op &op) {
    return op.address == addr;
  });
}

TEST_CASE("X86Analysis x86_64 - block ends where a call target begins",
          "[x86][analysis][blocks]") {
  // 0x1000: mov eax, 5     b8 05 00 00 00
  // 0x1005: call 0x100a    e8 00 00 00 00
  // 0x100a: ret            c3              <- call target: its own block
  const std::vector<std::uint8_t> bytes = {0xb8, 0x05, 0x00, 0x00, 0x00, 0xe8,
                                           0x00, 0x00, 0x00, 0x00, 0xc3};
  auto dis = makeAnalyzer(dragon::Architecture::X86_64);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  dragon::BasicBlock caller{&g, 0x1000};
  REQUIRE(caller.isValid());
  CHECK(addressesOf(caller) == std::vector<std::uint64_t>{0x1000, 0x1005});
  CHECK(caller.successors() == std::set<std::uint64_t>{0x100a});

  const auto *ib = g.getIGNILBlock(0x1000);
  REQUIRE(ib != nullptr);
  CHECK_FALSE(ignilTouches(*ib, 0x100a)); // the callee's RET is not ours
  CHECK(ib->ops().back().opcode == dragon::ignil::Opcode::JMP);

  // Flags agree with the Graph's sets.
  CHECK(g.get(0x100a)->isBlockStart());
  CHECK(g.get(0x100a)->isFunctionStart());
}

TEST_CASE("X86Analysis x86_64 - indirect-jump target splits a lifted block "
          "and its IGNIL is re-lifted",
          "[x86][analysis][blocks]") {
  // Round 1 (recursive traversal) lifts the block at 0x1010 as nop x4 + ret.
  // Round 2 resolves `jmp rax` to 0x1014, inside that block: the block must
  // be split and its stale IGNIL (which still holds the RET) replaced.
  //
  // 0x1000: je  0x1010            74 0e
  // 0x1002: mov rax, 0x1014       48 c7 c0 14 10 00 00
  // 0x1009: jmp rax               ff e0
  // 0x100b: int3 x5               cc cc cc cc cc   (never reached)
  // 0x1010: nop x4                90 90 90 90
  // 0x1014: ret                   c3
  const std::vector<std::uint8_t> bytes = {
          0x74, 0x0e, 0x48, 0xc7, 0xc0, 0x14, 0x10, 0x00, 0x00, 0xff, 0xe0,
          0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x90, 0x90, 0x90, 0x90, 0xc3};
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  auto g = makeGraph(dragon::Architecture::X86_64);
  dis.disassemble(bytes, 0x1000, g);

  REQUIRE(g.isValidBlock(0x1014)); // the jump was resolved

  dragon::BasicBlock nops{&g, 0x1010};
  REQUIRE(nops.isValid());
  CHECK(nops.terminatorAddress() == 0x1013);

  const auto *ib = g.getIGNILBlock(0x1010);
  REQUIRE(ib != nullptr);
  CHECK_FALSE(ignilTouches(*ib, 0x1014));
  REQUIRE_FALSE(ib->empty());
  const auto &last = ib->ops().back();
  REQUIRE(last.opcode == dragon::ignil::Opcode::JMP);
  CHECK(std::get<dragon::ignil::Const>(last.srcs[0]).value == 0x1014);

  const auto *retBlock = g.getIGNILBlock(0x1014);
  REQUIRE(retBlock != nullptr);
  CHECK(retBlock->ops().back().opcode == dragon::ignil::Opcode::RET);
}

// ---------------------------------------------------------------------------
// Recursive traversal, entry points, and the edges of an unconditional jump.
// ---------------------------------------------------------------------------

namespace {
dragon::Graph analyse(const std::vector<std::uint8_t> &bytes,
                      dragon::DisassemblyStrategy strategy,
                      std::vector<std::uint64_t> entries = {}) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = strategy;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g(dragon::Architecture::X86_64, cfg);
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = std::move(entries);
  dis.disassemble(bytes, 0x1000, g, hints);
  return g;
}
} // namespace

TEST_CASE("X86Analysis - recursive traversal follows call targets",
          "[x86][analysis][recursive]") {
  // 0x1000 call 0x1006 ; 0x1005 ret ; 0x1006 mov eax, 1 ; 0x100b ret
  // The callee used to be registered as a function start but never decoded.
  const std::vector<std::uint8_t> bytes = {0xe8, 0x01, 0x00, 0x00, 0x00, 0xc3,
                                           0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
  auto g = analyse(bytes, dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);
  REQUIRE(g.get(0x1006));
  REQUIRE(hasFunction(g.functions(), 0x1006));
}

TEST_CASE("X86Analysis - recursive traversal stops at an unconditional jmp",
          "[x86][analysis][recursive]") {
  // 0x1000 jmp 0x1007 ; 0x1002 <5 bytes that decode as mov eax, 0> ;
  // 0x1007 ret. Nothing reaches 0x1002; it used to be decoded as code.
  const std::vector<std::uint8_t> bytes = {0xeb, 0x05, 0xb8, 0x00, 0x00,
                                           0x00, 0x00, 0xc3};
  auto g = analyse(bytes, dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);
  CHECK(g.get(0x1007));
  CHECK_FALSE(g.get(0x1002));
}

TEST_CASE("X86Analysis - an unresolved indirect jmp has no successors",
          "[x86][analysis]") {
  // jmp rax ; ret - control never falls through `jmp rax`.
  const std::vector<std::uint8_t> bytes = {0xff, 0xe0, 0xc3};
  auto g = analyse(bytes, dragon::DisassemblyStrategy::LINEAR_SWEEP);
  const dragon::BasicBlock bb{&g, 0x1000};
  REQUIRE(bb.isValid());
  CHECK(bb.successors().empty()); // was {0x1002}
}

TEST_CASE("X86Analysis - entry points become functions with blocks",
          "[x86][analysis]") {
  // 0x1000 ret ; 0x1001 mov eax, 1 ; 0x1006 ret - 0x1001 is reached by
  // nothing in the code (a function called through a pointer).
  const std::vector<std::uint8_t> bytes = {0xc3, 0xb8, 0x01, 0x00,
                                           0x00, 0x00, 0xc3};
  for (auto strategy: {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                       dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    INFO("strategy " << static_cast<int>(strategy));
    auto g = analyse(bytes, strategy, {0x1000, 0x1001});
    REQUIRE(hasFunction(g.functions(), 0x1000));
    REQUIRE(hasFunction(g.functions(), 0x1001));
    CHECK(g.getIGNILBlock(0x1001));
  }
}

TEST_CASE("X86Analysis - a block that runs into the end of the code is kept",
          "[x86][analysis]") {
  // 0x1000 test edi, edi ; je 0x1005 ; ret
  // 0x1005 call 0x100a        <- last instruction of the bytes: a call to a
  //                              noreturn function at the end of .text
  // The block at 0x1005 was dropped as invalid (its fall-through has no
  // instruction), and with it the error path of the function.
  const std::vector<std::uint8_t> bytes = {0x85, 0xff, 0x74, 0x01, 0xc3,
                                           0xe8, 0x00, 0x00, 0x00, 0x00};
  for (auto strategy: {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                       dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    INFO("strategy " << static_cast<int>(strategy));
    auto g = analyse(bytes, strategy, {0x1000});
    const dragon::BasicBlock bb{&g, 0x1005};
    REQUIRE(bb.isValid());
    CHECK(bb.terminatorAddress() == 0x1005);
    CHECK(bb.successors().empty());
    CHECK(g.getIGNILBlock(0x1005) != nullptr);
    const dragon::Function fn{&g, 0x1000};
    REQUIRE(fn.isValid());
    CHECK(fn.block(0x1005) != nullptr);
  }
}
