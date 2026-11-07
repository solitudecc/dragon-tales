#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace {
constexpr std::uint64_t base = 0x1000, tableAddress = 0x1080;

struct MutableTable {
  dragon::Architecture architecture;
  std::vector<std::uint8_t> code;
  std::vector<std::uint8_t> initialTable;
  std::uint64_t dispatch, store;
  unsigned entrySize;
};

void put(std::vector<std::uint8_t> &bytes, std::size_t offset,
         std::uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

MutableTable x64Table(bool relative) {
  // The external table is writable Guest data, not self-modifying code.
  // Both forms really replace entry zero's target 0x1040 with 0x1060.
  const std::vector<std::uint8_t> absoluteCode{
      0xb9,0x60,0x10,0,0, 0x48,0x89,0x0c,0x25,0x80,0x10,0,0,
      0x83,0x3e,1, 0x77,0x2e, 0x8b,0x06,
      0xff,0x24,0xc5,0x80,0x10,0,0};
  const std::vector<std::uint8_t> relativeCode{
      0xc7,0x04,0x25,0x80,0x10,0,0,0xe0,0xff,0xff,0xff,
      0x83,0x3e,1, 0x77,0x30, 0x8b,0x06,
      0x48,0x8d,0x15,0x67,0,0,0, 0x48,0x63,0x04,0x82,
      0x48,0x01,0xd0, 0xff,0xe0};
  MutableTable fixture{dragon::Architecture::X86_64,
      std::vector<std::uint8_t>(128, 0xcc),
      std::vector<std::uint8_t>(relative ? 8 : 16),
      relative ? 0x1020u : 0x1014u, relative ? 0x1000u : 0x1005u,
      relative ? 4u : 8u};
  const auto &prefix = relative ? relativeCode : absoluteCode;
  std::copy(prefix.begin(), prefix.end(), fixture.code.begin());
  for (auto address : {0x1040u, 0x1050u, 0x1060u})
    fixture.code[address - base] = 0xc3;
  put(fixture.initialTable, 0, relative ? 0xffffffc0u : 0x1040u,
      fixture.entrySize);
  put(fixture.initialTable, fixture.entrySize,
      relative ? 0xffffffd0u : 0x1050u, fixture.entrySize);
  return fixture;
}

MutableTable a64Table(bool relative) {
  // Relative: ADR x1,table; MOV w3,14; STRH w3,[x1]; LDR w0,[x2];
  // CMP w0,1; B.HI 1040; LDRH w0,[x1,w0,UXTW#1]; ADR x3,1028;
  // ADD x0,x3,w0,SXTH#2; BR x0. Entries 6,10 become targets 1040,1050;
  // the real STRH changes entry zero to 14, hence target 1060.
  const std::vector<std::uint32_t> relativeWords{
      0x10000401,0x528001c3,0x79000023,0xb9400040,0x7100041f,
      0x54000168,0x78605820,0x10000063,0x8b20a860,0xd61f0000};
  // Absolute: MOV x3,1080; MOV x4,1060; STR x4,[x3]; LDR w0,[x1];
  // CMP w0,1; B.HI 1040; LDR x0,[x3,w0,UXTW#3]; BR x0.
  const std::vector<std::uint32_t> absoluteWords{
      0xd2821003,0xd2820c04,0xf9000064,0xb9400020,
      0x7100041f,0x54000168,0xf8605860,0xd61f0000};
  MutableTable fixture{dragon::Architecture::ARM64,
      std::vector<std::uint8_t>(128),
      std::vector<std::uint8_t>(relative ? 4 : 16),
      relative ? 0x1024u : 0x101cu, 0x1008, relative ? 2u : 8u};
  const auto &words = relative ? relativeWords : absoluteWords;
  for (std::size_t i = 0; i < words.size(); ++i)
    put(fixture.code, 4 * i, words[i], 4);
  for (auto address : {0x1040u, 0x1050u, 0x1060u})
    put(fixture.code, address - base, 0xd65f03c0, 4); // RET x30
  put(fixture.initialTable, 0, relative ? 6u : 0x1040u, fixture.entrySize);
  put(fixture.initialTable, fixture.entrySize,
      relative ? 10u : 0x1050u, fixture.entrySize);
  return fixture;
}

void checkUntrustedTable(const MutableTable &fixture) {
  for (auto strategy : {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                        dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    CAPTURE(strategy);
    dragon::Configuration config;
    config.disassemblyStrategy = strategy;
    dragon::Disassembler dis{fixture.architecture, config};
    dragon::Graph graph{fixture.architecture, config};
    dragon::Disassembler::AnalysisHints hints;
    // A successful initial-image reader is deliberately insufficient to
    // establish the table's contents at a later machine LOAD.
    hints.readMemory = [&](std::uint64_t address, std::span<std::uint8_t> out) {
      if (address < tableAddress || address - tableAddress > fixture.initialTable.size() ||
          out.size() > fixture.initialTable.size() - (address - tableAddress))
        return false;
      std::copy_n(fixture.initialTable.begin() + (address - tableAddress),
                  out.size(), out.begin());
      return true;
    };
    dis.disassemble(fixture.code, base, graph, hints);
    const auto *jump = graph.get(fixture.dispatch);
    REQUIRE(jump);
    const dragon::BasicBlock dispatch{&graph,
        fixture.architecture == dragon::Architecture::X86_64
            ? (fixture.entrySize == 8 ? 0x1012u : 0x1010u) : 0x1018u};
    REQUIRE(dispatch.isValid());
    const auto *block = graph.getIGNILBlock(dispatch.startAddress());
    REQUIRE(block);
    REQUIRE(block->unlifted().empty());
    REQUIRE_FALSE(block->ops().empty());
    const auto &tail = block->ops().back();
    REQUIRE(tail.opcode == dragon::ignil::Opcode::JMP);
    REQUIRE(tail.srcs.size() == 1);
    const auto expression = dragon::BackwardSlicer{8}.slice(
        tail.srcs[0], dispatch.startAddress(), graph);
    const auto candidate = dragon::slice::detectPossibleSwitch(
        expression, dispatch.startAddress(), graph, 8);
    REQUIRE(candidate);
    CHECK(candidate->tableBase == tableAddress);
    CHECK(candidate->entrySize == fixture.entrySize);
    CHECK(candidate->numCases == 2);
    // This is not an unsupported STORE or a failed bound/shape test.
    bool foundStore = false;
    for (const auto &owner : graph.blocks()) {
      const auto *ir = graph.getIGNILBlock(owner.startAddress());
      if (!ir) continue;
      for (const auto &op : ir->ops())
        if (op.address == fixture.store && op.opcode == dragon::ignil::Opcode::STORE) {
          foundStore = true;
          REQUIRE(op.srcs.size() == 2);
          CHECK(dragon::ignil::widthOf(op.srcs[1]) == fixture.entrySize * 8);
        }
    }
    REQUIRE(foundStore);
    CHECK(jump->branches().empty());
  }
}
} // namespace

TEST_CASE("Writable x64 absolute tables do not inherit initial image targets",
          "[table-contents][image-table]") {
  checkUntrustedTable(x64Table(false));
}

TEST_CASE("Writable x64 relative tables do not inherit initial image targets",
          "[table-contents][image-table]") {
  checkUntrustedTable(x64Table(true));
}

TEST_CASE("Writable A64 absolute tables do not inherit initial image targets",
          "[table-contents][image-table]") {
  checkUntrustedTable(a64Table(false));
}

TEST_CASE("Writable A64 relative tables do not inherit initial image targets",
          "[table-contents][image-table]") {
  checkUntrustedTable(a64Table(true));
}

TEST_CASE("Trusted table contents describe the exact LOAD after Guest writes",
          "[table-contents][known-memory]") {
  for (const auto &fixture : {x64Table(false), x64Table(true),
                              a64Table(false), a64Table(true)}) {
    for (auto strategy : {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                          dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(fixture.architecture, fixture.entrySize, strategy);
      dragon::Configuration config;
      config.disassemblyStrategy = strategy;
      dragon::Disassembler dis{fixture.architecture, config};
      dragon::Disassembler::AnalysisHints hints;
      unsigned imageReads = 0;
      hints.readMemory = [&](std::uint64_t address, std::span<std::uint8_t> out) {
        ++imageReads;
        if (address < tableAddress || address - tableAddress > fixture.initialTable.size() ||
            out.size() > fixture.initialTable.size() - (address - tableAddress)) return false;
        std::copy_n(fixture.initialTable.begin() + (address - tableAddress), out.size(), out.begin());
        return true;
      };
      auto after = fixture.initialTable;
      put(after, 0, fixture.entrySize == 2 ? 14u :
          fixture.entrySize == 4 ? 0xffffffe0u : 0x1060u, fixture.entrySize);
      struct Request {
        dragon::Disassembler::MemoryReadSite site;
        std::uint64_t address;
        std::size_t size;
      };
      std::vector<Request> requests;
      auto available = after.size();
      hints.readKnownMemory = [&](const dragon::Disassembler::MemoryReadSite &site,
          std::uint64_t address, std::span<std::uint8_t> out) {
        requests.push_back({site, address, out.size()});
        if (address < tableAddress || address - tableAddress > available ||
            out.size() > available - (address - tableAddress)) return false;
        std::copy_n(after.begin() + (address - tableAddress), out.size(), out.begin());
        return true;
      };
      dragon::Graph known{fixture.architecture, config};
      dis.disassemble(fixture.code, base, known, hints);
      REQUIRE(known.get(fixture.dispatch));
      CHECK(known.get(fixture.dispatch)->branches() == std::set<std::uint64_t>{0x1050, 0x1060});
      REQUIRE_FALSE(requests.empty());
      std::set<std::uint64_t> slots;
      const std::uint64_t expectedPC = fixture.architecture == dragon::Architecture::ARM64
          ? 0x1018 : fixture.entrySize == 4 ? 0x1019 : 0x1014;
      for (const auto &request : requests) {
        const auto *block = known.getIGNILBlock(request.site.blockAddress);
        REQUIRE(block);
        REQUIRE(request.site.opIndex < block->ops().size());
        const auto &operation = block->ops()[request.site.opIndex];
        CHECK(operation.opcode == dragon::ignil::Opcode::LOAD);
        CHECK(operation.address == expectedPC);
        CHECK(request.site.instructionAddress == expectedPC);
        CHECK(request.size == fixture.entrySize);
        CHECK(request.address >= tableAddress);
        CHECK(request.address + request.size <= tableAddress + after.size());
        slots.insert(request.address);
      }
      CHECK(slots == std::set<std::uint64_t>{tableAddress, tableAddress + fixture.entrySize});
      CHECK(imageReads == 0);

      // Entry zero was also really written by the Guest; neither that fact
      // nor its explicit assertion supplies entry one's missing contents.
      available = fixture.entrySize;
      dragon::Graph partial{fixture.architecture, config};
      dis.disassemble(fixture.code, base, partial, hints);
      REQUIRE(partial.get(fixture.dispatch));
      CHECK(partial.get(fixture.dispatch)->branches().empty());
      hints.readKnownMemory = {};
      dragon::Graph cleared{fixture.architecture, config};
      dis.disassemble(fixture.code, base, cleared, hints);
      REQUIRE(cleared.get(fixture.dispatch));
      CHECK(cleared.get(fixture.dispatch)->branches().empty());
      CHECK(known.get(fixture.dispatch)->branches() == std::set<std::uint64_t>{0x1050, 0x1060});
      CHECK(imageReads == 0);
    }
  }
}
