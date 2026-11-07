#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace {
constexpr std::uint64_t base = 0x1000, table = 0x1080;
using Bytes = std::vector<std::uint8_t>;

void word(Bytes &bytes,std::uint64_t address,std::uint32_t value) {
  REQUIRE(address >= base);
  REQUIRE(address - base + 4 <= bytes.size());
  for (unsigned i = 0; i < 4; ++i)
    bytes[address - base + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

struct Fixture {
  Bytes code;
  std::uint64_t dispatch;
  unsigned entrySize;
};

Fixture fixture(bool relative,bool call = false) {
  // These are the permanent writable-table machine fixtures. The table is
  // external to the 128-byte code region, and each case is an ordinary RET.
  std::vector<std::uint32_t> words = relative
      ? std::vector<std::uint32_t>{0x10000401,0x528001c3,0x79000023,0xb9400040,
          0x7100041f,0x54000168,0x78605820,0x10000063,0x8b20a860,0xd61f0000}
      : std::vector<std::uint32_t>{0xd2821003,0xd2820c04,0xf9000064,0xb9400020,
          0x7100041f,0x54000168,0xf8605860,0xd61f0000};
  REQUIRE_FALSE((relative && call));
  if (call) {
    // Preserve LR around a genuine BL to the same leaf that the table also
    // reaches. A switch edge must not erase this independent callee evidence.
    words.insert(words.begin(), {0xaa1e03f4,0x94000013,0xaa1403fe});
    words[8] = 0x54000108; // B.HI 1040 from 1020 after the inserted call.
  }
  Fixture result{Bytes(128),relative ? 0x1024u : call ? 0x1028u : 0x101cu,
                 relative ? 2u : 8u};
  for (std::size_t i = 0; i < words.size(); ++i) word(result.code,base + 4 * i,words[i]);
  for (auto address : {0x1040u,0x1050u,0x1060u}) word(result.code,address,0xd65f03c0);
  return result;
}

dragon::Configuration configuration(dragon::DisassemblyStrategy strategy) {
  dragon::Configuration result;
  result.disassemblyStrategy = strategy;
#ifdef DRAGON_WITH_Z3
  result.useZ3ForPathDiscovery = false;
#endif
  return result;
}

dragon::Disassembler::AnalysisHints contents(const Fixture &f) {
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = {base};
  hints.readKnownMemory = [size = f.entrySize](const dragon::Disassembler::MemoryReadSite &,
      std::uint64_t address,std::span<std::uint8_t> out) {
    if (out.size() != size || (address != table && address != table + size)) return false;
    const std::uint64_t value = size == 2 ? (address == table ? 14u : 10u)
        : (address == table ? 0x1060u : 0x1050u);
    for (std::size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    return true;
  };
  return hints;
}

void tableTargets(const dragon::Graph &graph,const Fixture &f) {
  REQUIRE(graph.get(f.dispatch));
  REQUIRE(graph.get(f.dispatch)->isJump());
  REQUIRE(graph.get(f.dispatch)->hasIndirectTarget());
  CHECK(graph.get(f.dispatch)->branches() == std::set<std::uint64_t>{0x1050,0x1060});
}
} // namespace

TEST_CASE("ARM table cases remain in their root rather than post RET leaf guesses",
          "[arm64][table-contents][arm-case-membership]") {
  for (auto strategy : {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                        dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL})
    for (bool relative : {false,true}) {
      CAPTURE(strategy,relative);
      const auto f = fixture(relative);
      const auto config = configuration(strategy);
      dragon::Disassembler dis{dragon::Architecture::ARM64,config};
      dragon::Graph graph{dragon::Architecture::ARM64,config};
      dis.disassemble(f.code,base,graph,contents(f));
      tableTargets(graph,f);
      const dragon::Function root{&graph,base};
      REQUIRE(root.isValid());
      for (auto address : {0x1050u,0x1060u}) {
        CAPTURE(address);
        REQUIRE(graph.get(address));
        CHECK_FALSE(graph.isValidFunction(address));
        CHECK_FALSE(graph.get(address)->isFunctionStart());
        CHECK(root.block(address) != nullptr);
        const auto *ir = graph.getIGNILBlock(address);
        REQUIRE(ir);
        REQUIRE(ir->unlifted().empty());
        REQUIRE_FALSE(ir->ops().empty());
        CHECK(ir->ops().back().opcode == dragon::ignil::Opcode::RET);
      }
    }
}

TEST_CASE("ARM table edges preserve explicit independent entries and direct callees",
          "[arm64][table-contents][arm-case-membership]") {
  for (auto strategy : {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                        dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL})
    for (bool call : {false,true}) {
      CAPTURE(strategy,call);
      const auto f = fixture(false,call);
      const auto config = configuration(strategy);
      dragon::Disassembler dis{dragon::Architecture::ARM64,config};
      dragon::Graph graph{dragon::Architecture::ARM64,config};
      auto hints = contents(f);
      if (!call) hints.entryPoints.push_back(0x1050);
      dis.disassemble(f.code,base,graph,hints);
      tableTargets(graph,f);
      REQUIRE(graph.get(0x1050));
      CHECK(graph.isValidFunction(0x1050));
      CHECK(graph.get(0x1050)->isFunctionStart());
      const dragon::Function root{&graph,base};
      REQUIRE(root.isValid());
      CHECK(root.block(0x1050) == nullptr);
      if (call) {
        REQUIRE(graph.get(0x1004));
        CHECK(graph.get(0x1004)->isCall());
        CHECK(graph.get(0x1004)->callees() == std::set<std::uint64_t>{0x1050});
      }
    }
}

TEST_CASE("ARM table edges preserve independently recognized prologues",
          "[arm64][table-contents][arm-case-membership]") {
  const auto config = configuration(dragon::DisassemblyStrategy::LINEAR_SWEEP);
  auto f = fixture(false);
  word(f.code,0x1050,0xa9bf7bfd); // STP X29,X30,[SP,#-16]!
  word(f.code,0x1054,0x910003fd); // MOV X29,SP
  word(f.code,0x1058,0xa8c17bfd); // LDP X29,X30,[SP],#16
  word(f.code,0x105c,0xd65f03c0); // RET
  dragon::Disassembler dis{dragon::Architecture::ARM64,config};
  dragon::Graph graph{dragon::Architecture::ARM64,config};
  dis.disassemble(f.code,base,graph,contents(f));
  tableTargets(graph,f);
  REQUIRE(graph.get(0x1050));
  CHECK(graph.get(0x1050)->isPrologue());
  CHECK(graph.isValidFunction(0x1050));
  const dragon::Function root{&graph,base};
  REQUIRE(root.isValid());
  CHECK(root.block(0x1050) == nullptr);
}

TEST_CASE("ARM post RET discovery remains active in separate regions without dispatch",
          "[arm64][table-contents][arm-case-membership]") {
  const auto config = configuration(dragon::DisassemblyStrategy::LINEAR_SWEEP);
  Bytes code(0x98);
  word(code,0x1000,0xd61f0000); // BR X0, unresolved within region 1000.
  word(code,0x1040,0xd65f03c0);
  word(code,0x1050,0xd65f03c0); // A possible case, layout alone is insufficient.
  word(code,0x1080,0xd65f03c0); // Separate explicit function; no dispatcher.
  word(code,0x1090,0x528000e0); // MOV W0,#7 after RET and zero-word padding.
  word(code,0x1094,0xd65f03c0);
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = {base,0x1080};
  dragon::Disassembler dis{dragon::Architecture::ARM64,config};
  dragon::Graph graph{dragon::Architecture::ARM64,config};
  dis.disassemble(code,base,graph,hints);
  REQUIRE(graph.get(base));
  CHECK(graph.get(base)->hasIndirectTarget());
  CHECK_FALSE(graph.isValidFunction(0x1050));
  CHECK(graph.isValidFunction(0x1080));
  CHECK(graph.isValidFunction(0x1090));
}

TEST_CASE("ARM32 indirect regions keep layout guesses separate from real entries",
          "[arm32][table-contents][arm-case-membership]") {
  auto config = configuration(dragon::DisassemblyStrategy::LINEAR_SWEEP);
  config.cpu = "arm926ej-s"; // Same ARMv5TEJ BX support as TestDisassembler.
  Bytes code(0x4c);
  for (std::uint64_t address = base; address < base + code.size(); address += 4)
    word(code,address,0xe1a00000); // MOV R0,R0, ordinary non-padding layout.
  word(code,0x1000,0xeb00000e); // BL 1040: PC+8 plus 14 words.
  word(code,0x1004,0xe12fff10); // BX R0: unresolved non-return dispatch.
  word(code,0x1008,0xe12fff1e); // BX LR
  word(code,0x100c,0xe3a00001); // MOV R0,#1: possible case after a RET.
  word(code,0x1010,0xe12fff1e);
  word(code,0x1020,0xe12fff1e); // Explicit independent entry.
  word(code,0x1024,0xe3a00002); // Normal post-RET function in that region.
  word(code,0x1028,0xe12fff1e);
  word(code,0x1040,0xe12fff1e); // Genuine direct callee.
  word(code,0x1044,0xe3a00003);
  word(code,0x1048,0xe12fff1e);
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = {base,0x1020};
  dragon::Disassembler dis{dragon::Architecture::ARM32,config};
  dragon::Graph graph{dragon::Architecture::ARM32,config};
  dis.disassemble(code,base,graph,hints);
  REQUIRE(graph.get(0x1004));
  REQUIRE(graph.get(0x1004)->isJump());
  REQUIRE(graph.get(0x1004)->hasIndirectTarget());
  REQUIRE_FALSE(graph.get(0x1004)->isReturn());
  REQUIRE(graph.get(base));
  CHECK(graph.get(base)->callees() == std::set<std::uint64_t>{0x1040});
  CHECK_FALSE(graph.isValidFunction(0x100c));
  CHECK(graph.isValidFunction(0x1020));
  CHECK(graph.isValidFunction(0x1024));
  CHECK(graph.isValidFunction(0x1040));
  CHECK(graph.isValidFunction(0x1044));
  const dragon::Function root{&graph,base};
  REQUIRE(root.isValid());
  CHECK(root.block(0x1020) == nullptr);
  CHECK(root.block(0x1040) == nullptr);
  // A32 has no general semantic lifter. This case claims only decoding,
  // region ownership and entry preservation, not machine execution support.
}

TEST_CASE("ARM classifiers retain their own target IDs across interleaved architectures",
          "[arm][table-contents][arm-case-membership][arm-classifier-cache]") {
  // Exercise both orders in this process. The assertions use real decoded
  // prologues and post-RET discovery, so silently losing either classifier
  // cannot pass as an unresolved-table precision choice.
  for (bool firstWide : {true,false})
    for (bool wide : {firstWide,!firstWide,firstWide}) {
      CAPTURE(firstWide,wide);
      auto config = configuration(dragon::DisassemblyStrategy::LINEAR_SWEEP);
      if (!wide) config.cpu = "arm926ej-s";
      const auto architecture = wide ? dragon::Architecture::ARM64 : dragon::Architecture::ARM32;
      Bytes code(0x30);
      for (std::uint64_t address = base; address < base + code.size(); address += 4)
        word(code,address,wide ? 0xd503201f : 0xe1a00000);
      word(code,0x1000,wide ? 0xd65f03c0 : 0xe12fff1e); // RET / BX LR
      word(code,0x1004,wide ? 0x528000e0 : 0xe3a00007); // MOV W0/R0,#7
      word(code,0x1008,wide ? 0xd65f03c0 : 0xe12fff1e);
      word(code,0x1020,wide ? 0xa9bf7bfd : 0xe92d4800); // STP FP,LR / PUSH R11,LR
      word(code,0x1024,wide ? 0x910003fd : 0xe28db000); // MOV FP,SP / ADD R11,SP,#0
      word(code,0x1028,wide ? 0xa8c17bfd : 0xe8bd4800); // LDP FP,LR / POP R11,LR
      word(code,0x102c,wide ? 0xd65f03c0 : 0xe12fff1e);
      dragon::Disassembler dis{architecture,config};
      dragon::Graph graph{architecture,config};
      dis.disassemble(code,base,graph);
      REQUIRE(graph.get(base));
      REQUIRE(graph.get(0x1004));
      REQUIRE(graph.get(0x1020));
      CHECK(graph.get(base)->isReturn());
      CHECK(graph.isValidFunction(0x1004));
      CHECK(graph.get(0x1004)->isFunctionStart());
      CHECK(graph.isValidFunction(0x1020));
      CHECK(graph.get(0x1020)->isFunctionStart());
      CHECK(graph.get(0x1020)->isPrologue());
    }
}
