#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/loader/ImageDisassembler.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace dragon;
using Bytes = std::vector<std::uint8_t>;
constexpr std::array architectures{Architecture::X86_32, Architecture::X86_64,
                                   Architecture::ARM64};
constexpr std::array strategies{DisassemblyStrategy::LINEAR_SWEEP,
                                DisassemblyStrategy::RECURSIVE_TRAVERSAL};

Configuration configuration(DisassemblyStrategy strategy) {
  Configuration config;
  config.disassemblyStrategy = strategy;
  return config;
}

void appendWord(Bytes &bytes, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Bytes ret(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0xc0, 0x03, 0x5f, 0xd6}
                                      : Bytes{0xc3};
}

Bytes transfer(Architecture arch, std::uint64_t pc, std::uint64_t target,
               bool call) {
  Bytes bytes;
  if (arch == Architecture::ARM64) {
    REQUIRE((pc & 3) == 0);
    REQUIRE((target & 3) == 0);
    // B/BL use a signed word displacement from the instruction itself.
    appendWord(bytes, (call ? 0x94000000u : 0x14000000u) |
                          (static_cast<std::uint32_t>((target - pc) / 4) &
                           0x03ffffffu));
  } else {
    bytes.push_back(call ? 0xe8 : 0xe9);
    appendWord(bytes, static_cast<std::uint32_t>(target - (pc + 5)));
  }
  return bytes;
}

Bytes conditionalSelf(Architecture arch) {
  // CBZ W0,pc / JE pc: one known taken edge and a separate fallthrough.
  return arch == Architecture::ARM64 ? Bytes{0x00, 0x00, 0x00, 0x34}
                                      : Bytes{0x74, 0xfe};
}

void append(Bytes &bytes, const Bytes &tail) {
  bytes.insert(bytes.end(), tail.begin(), tail.end());
}

void missingEntry(const Graph &graph, std::uint64_t address) {
  REQUIRE(graph.get(address) == nullptr);
  CHECK_FALSE(graph.isValidBlock(address));
  CHECK_FALSE(graph.isValidFunction(address));
  CHECK(graph.getIGNILBlock(address) == nullptr);
  CHECK_FALSE(BasicBlock{&graph, address}.isValid());
  CHECK_FALSE(Function{&graph, address}.isValid());
}

void decodedBlock(const Graph &graph, std::uint64_t address) {
  REQUIRE(graph.get(address) != nullptr);
  CHECK(graph.isValidBlock(address));
  CHECK(graph.get(address)->isBlockStart());
  CHECK(BasicBlock{&graph, address}.isValid());
  CHECK(graph.getIGNILBlock(address) != nullptr);
}

void decodedEntry(const Graph &graph, std::uint64_t address) {
  decodedBlock(graph, address);
  CHECK(graph.isValidFunction(address));
  CHECK(graph.get(address)->isFunctionStart());
  CHECK(Function{&graph, address}.isValid());
}

// An ordinary ELF64 executable with two disjoint RX sections and matching
// PT_LOAD records. No symbols, relocations or metadata supply hidden roots.
// The small image is built in memory; no assembler/linker is needed at runtime.
Bytes twoSectionElf(Architecture arch, std::uint64_t firstAddress,
                    const Bytes &first, std::uint64_t secondAddress,
                    const Bytes &second, std::uint64_t entry) {
  REQUIRE((arch == Architecture::X86_64 || arch == Architecture::ARM64));
  REQUIRE(first.size() <= 0x100);
  REQUIRE(second.size() <= 0x100);
  constexpr std::size_t namesAt = 0x300, headersAt = 0x340;
  const std::string names{"\0.first\0.second\0.shstrtab\0", 26};
  Bytes bytes(headersAt + 4 * 64);
  auto put = [&](std::size_t at, std::uint64_t value, unsigned count) {
    REQUIRE(at + count <= bytes.size());
    for (unsigned i = 0; i < count; ++i)
      bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  };
  bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
  bytes[4] = 2; bytes[5] = 1; bytes[6] = 1; // ELF64 little endian version 1.
  put(16, 2, 2); // ET_EXEC.
  put(18, arch == Architecture::ARM64 ? 183 : 62, 2);
  put(20, 1, 4); put(24, entry, 8);
  put(32, 64, 8); put(40, headersAt, 8);
  put(52, 64, 2); put(54, 56, 2); put(56, 2, 2);
  put(58, 64, 2); put(60, 4, 2); put(62, 3, 2);
  auto section = [&](unsigned index, std::uint64_t address,
                     std::size_t fileOffset, const Bytes &code, unsigned name) {
    const auto ph = 64 + index * 56;
    put(ph, 1, 4); put(ph + 4, 5, 4); // PT_LOAD, PF_R | PF_X.
    put(ph + 8, fileOffset, 8); put(ph + 16, address, 8);
    put(ph + 24, address, 8); put(ph + 32, code.size(), 8);
    put(ph + 40, code.size(), 8); put(ph + 48, 1, 8);
    const auto sh = headersAt + (index + 1) * 64;
    put(sh, name, 4); put(sh + 4, 1, 4); // SHT_PROGBITS.
    put(sh + 8, 6, 8); // SHF_ALLOC | SHF_EXECINSTR.
    put(sh + 16, address, 8); put(sh + 24, fileOffset, 8);
    put(sh + 32, code.size(), 8); put(sh + 48, 1, 8);
    std::copy(code.begin(), code.end(), bytes.begin() + fileOffset);
  };
  section(0, firstAddress, 0x100, first, 1);
  section(1, secondAddress, 0x200, second, 8);
  std::copy(names.begin(), names.end(), bytes.begin() + namesAt);
  const auto namesHeader = headersAt + 3 * 64;
  put(namesHeader, 16, 4); put(namesHeader + 4, 3, 4); // SHT_STRTAB.
  put(namesHeader + 24, namesAt, 8); put(namesHeader + 32, names.size(), 8);
  put(namesHeader + 48, 1, 8);
  return bytes;
}
} // namespace

TEST_CASE("Decoded entries keep real external zero targets without empty registration",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies)
      for (bool call : {false, true}) {
        CAPTURE(arch, strategy, call);
        auto config = configuration(strategy);
        Disassembler dis{arch, config};
        Graph graph{arch, config};
        auto bytes = transfer(arch, 0x1000, 0, call);
        if (call) append(bytes, ret(arch));
        dis.disassemble(bytes, 0x1000, graph);
        const auto *instruction = graph.get(0x1000);
        REQUIRE(instruction);
        REQUIRE(instruction->isCall() == call);
        REQUIRE(instruction->isJump() == !call);
        if (call) CHECK(instruction->callees() == std::set<std::uint64_t>{0});
        else {
          CHECK(instruction->branches() == std::set<std::uint64_t>{0});
          CHECK(BasicBlock{&graph, 0x1000}.successors() ==
                std::set<std::uint64_t>{0});
        }
        decodedEntry(graph, 0x1000);
        missingEntry(graph, 0);
      }
}

TEST_CASE("Decoded entries reject a truncated explicit root while retaining a good root",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      auto config = configuration(strategy);
      Disassembler dis{arch, config};
      Graph graph{arch, config};
      auto bytes = ret(arch);
      const auto bad = 0x1000 + bytes.size();
      bytes.push_back(arch == Architecture::ARM64 ? 0xff : 0x0f);
      Disassembler::AnalysisHints hints;
      hints.entryPoints = {bad, 0x1000};
      dis.disassemble(bytes, 0x1000, graph, hints);
      REQUIRE(graph.get(0x1000));
      REQUIRE(graph.get(0x1000)->isReturn());
      decodedEntry(graph, 0x1000);
      missingEntry(graph, bad);
    }
}

TEST_CASE("Decoded entries accept actual code at address zero",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      auto config = configuration(strategy);
      Disassembler dis{arch, config};
      Graph graph{arch, config};
      dis.disassemble(ret(arch), 0, graph);
      decodedEntry(graph, 0);
      CHECK(graph.get(0)->isReturn());
    }
}

TEST_CASE("Decoded entries retain an external conditional fallthrough edge",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      auto config = configuration(strategy);
      Disassembler dis{arch, config};
      Graph graph{arch, config};
      const auto bytes = conditionalSelf(arch);
      const auto fallthrough = 0x1000 + bytes.size();
      dis.disassemble(bytes, 0x1000, graph);
      REQUIRE(graph.get(0x1000));
      REQUIRE(graph.get(0x1000)->isConditional());
      CHECK(graph.get(0x1000)->branches() == std::set<std::uint64_t>{0x1000});
      CHECK(BasicBlock{&graph, 0x1000}.successors() ==
            std::set<std::uint64_t>{0x1000, fallthrough});
      decodedEntry(graph, 0x1000);
      missingEntry(graph, fallthrough);
    }
}

TEST_CASE("Decoded entries keep a nonempty prefix that reaches the stream end",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies)
      for (bool call : {false, true}) {
        CAPTURE(arch, strategy, call);
        auto config = configuration(strategy);
        Disassembler dis{arch, config};
        Graph graph{arch, config};
        const auto bytes = call ? transfer(arch, 0x1000, 0, true)
            : arch == Architecture::ARM64 ? Bytes{0x00, 0x04, 0x00, 0x11}
                                          : Bytes{0x31, 0xc0};
        dis.disassemble(bytes, 0x1000, graph);
        decodedEntry(graph, 0x1000);
        const BasicBlock block{&graph, 0x1000};
        REQUIRE(block.isValid());
        CHECK(block.terminatorAddress() == 0x1000);
        CHECK(block.instructions().size() == 1);
        CHECK(block.successors().empty());
        const auto *ir = graph.getIGNILBlock(0x1000);
        REQUIRE(ir);
        CHECK_FALSE(ir->ops().empty());
        if (call) {
          CHECK(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0});
          missingEntry(graph, 0);
        }
      }
}

TEST_CASE("Decoded entries can acquire a formerly external target in a later fragment",
          "[decoded-entries][analysis]") {
  for (auto arch : architectures)
    for (auto strategy : strategies)
      for (bool call : {false, true}) {
        CAPTURE(arch, strategy, call);
        auto config = configuration(strategy);
        Disassembler dis{arch, config};
        Graph graph{arch, config};
        auto bytes = transfer(arch, 0x1000, 0, call);
        if (call) append(bytes, ret(arch));
        dis.disassemble(bytes, 0x1000, graph);
        missingEntry(graph, 0);
        dis.disassemble(ret(arch), 0, graph);
        decodedEntry(graph, 0);
        decodedEntry(graph, 0x1000);
        if (call) CHECK(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0});
        else CHECK(graph.get(0x1000)->branches() == std::set<std::uint64_t>{0});
      }
}

TEST_CASE("Graph explicit registration remains usable before machine instructions",
          "[decoded-entries][graph]") {
  Graph graph{Architecture::X86_64, {}};
  graph.addBlock(0x8000);
  graph.addFunction(0x8000);
  CHECK(graph.isValidBlock(0x8000));
  CHECK(graph.isValidFunction(0x8000));
  CHECK(graph.validBlockAddresses().contains(0x8000));
  CHECK(graph.validFunctionAddresses().contains(0x8000));
  CHECK(graph.get(0x8000) == nullptr);
  CHECK_FALSE(BasicBlock{&graph, 0x8000}.isValid());
  CHECK_FALSE(Function{&graph, 0x8000}.isValid());
  ignil::Block ir{0x8000};
  ir.append({ignil::Opcode::RET, {}, {}, 0x8000});
  graph.addIGNILBlock(std::move(ir));
  REQUIRE(graph.getIGNILBlock(0x8000));
  CHECK(graph.getIGNILBlock(0x8000)->ops().size() == 1);
  CHECK(graph.isValidBlock(0x8000));
}

TEST_CASE("Loader recovers hidden backward cross section callees and jump targets",
          "[decoded-entries][loader]") {
  for (auto arch : {Architecture::X86_64, Architecture::ARM64})
    for (auto strategy : strategies)
      for (bool call : {false, true}) {
        CAPTURE(arch, strategy, call);
        Bytes lower(0x10, arch == Architecture::ARM64 ? 0 : 0xcc);
        const auto returnBytes = ret(arch);
        std::copy(returnBytes.begin(), returnBytes.end(), lower.begin());
        append(lower, returnBytes); // Hidden 1010 is after the first RET.
        auto upper = transfer(arch, 0x2000, 0x1010, call);
        if (call) append(upper, returnBytes);
        std::string error;
        auto image = loader::LoadedImage::loadBytes(
            twoSectionElf(arch, 0x1000, lower, 0x2000, upper, 0x2000), &error);
        INFO(error);
        REQUIRE(image);
        REQUIRE(image->architecture() == arch);
        REQUIRE(image->executableSections().size() == 2);
        auto graph = loader::disassembleImage(*image, configuration(strategy));
        decodedEntry(graph, 0x2000);
        decodedBlock(graph, 0x1010);
        if (call || strategy == DisassemblyStrategy::RECURSIVE_TRAVERSAL)
          CHECK(graph.isValidFunction(0x1010));
        CHECK(graph.get(0x1010)->isReturn());
        if (call) CHECK(graph.get(0x2000)->callees() == std::set<std::uint64_t>{0x1010});
        else CHECK(graph.get(0x2000)->branches() == std::set<std::uint64_t>{0x1010});
      }
}

TEST_CASE("Loader recovers cross section conditional fallthrough despite a middle root",
          "[decoded-entries][loader]") {
  for (auto arch : {Architecture::X86_64, Architecture::ARM64})
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      const auto first = conditionalSelf(arch);
      const auto next = 0x1000 + first.size();
      const auto middle = next + 0x10;
      Bytes second(0x10, arch == Architecture::ARM64 ? 0 : 0xcc);
      const auto returnBytes = ret(arch);
      std::copy(returnBytes.begin(), returnBytes.end(), second.begin());
      append(second, returnBytes);
      std::string error;
      auto image = loader::LoadedImage::loadBytes(
          twoSectionElf(arch, 0x1000, first, next, second, 0x1000), &error);
      INFO(error);
      REQUIRE(image);
      REQUIRE(image->executableSections().size() == 2);
      Disassembler::AnalysisHints hints;
      hints.entryPoints = {middle}; // Initial recursive pass skips next.
      auto graph = loader::disassembleImage(*image, configuration(strategy), hints);
      decodedEntry(graph, 0x1000);
      decodedBlock(graph, next);
      if (strategy == DisassemblyStrategy::RECURSIVE_TRAVERSAL)
        CHECK(graph.isValidFunction(next));
      decodedEntry(graph, middle);
      REQUIRE(graph.get(0x1000)->isConditional());
      CHECK(graph.get(0x1000)->branches() == std::set<std::uint64_t>{0x1000});
      CHECK(BasicBlock{&graph, 0x1000}.successors() ==
            std::set<std::uint64_t>{0x1000, next});
      CHECK(graph.get(next)->isReturn());
    }
}

TEST_CASE("Decoded entries classify a late fragment interior from earlier real edges",
          "[decoded-entries][analysis][late-decoded-entry]") {
  for (auto arch : architectures)
    for (auto strategy : strategies)
      for (bool call : {false, true}) {
        CAPTURE(arch, strategy, call);
        const auto config = configuration(strategy);
        Disassembler dis{arch, config};
        Graph graph{arch, config};
        auto caller = transfer(arch, 0x1000, 0x2008, call);
        if (call) append(caller, ret(arch));
        dis.disassemble(caller, 0x1000, graph);
        REQUIRE(graph.get(0x1000));
        REQUIRE(graph.get(0x2008) == nullptr);
        if (call)
          REQUIRE(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0x2008});
        else
          REQUIRE(graph.get(0x1000)->branches() == std::set<std::uint64_t>{0x2008});

        // The late invocation decodes the target by falling through its NOP
        // prefix. It is neither this invocation's root nor a new local edge.
        Bytes callee;
        if (arch == Architecture::ARM64)
          for (unsigned i = 0; i < 4; ++i) appendWord(callee, 0xd503201f);
        else callee.assign(0x10, 0x90);
        append(callee, ret(arch));
        dis.disassemble(callee, 0x2000, graph);
        REQUIRE(graph.get(0x2008));
        REQUIRE_FALSE(graph.get(0x2008)->isReturn());
        REQUIRE(graph.get(0x2010));
        REQUIRE(graph.get(0x2010)->isReturn());
        CHECK(graph.isValidBlock(0x2008));
        const BasicBlock target{&graph, 0x2008};
        CHECK(target.isValid());
        if (target.isValid()) {
          CHECK(target.terminatorAddress() == 0x2010);
          CHECK(target.instructions().size() ==
                (arch == Architecture::ARM64 ? 3u : 9u));
        }
        CHECK(graph.getIGNILBlock(0x2008) != nullptr);
        if (const auto *ir = graph.getIGNILBlock(0x2008)) {
          CHECK(ir->unlifted().empty());
          REQUIRE_FALSE(ir->ops().empty());
          CHECK(ir->ops().back().opcode == ignil::Opcode::RET);
        }
        if (call) {
          CHECK(graph.isValidFunction(0x2008));
          CHECK(Function{&graph, 0x2008}.isValid());
        }
        const BasicBlock prefix{&graph, 0x2000};
        REQUIRE(prefix.isValid());
        CHECK(prefix.terminatorAddress() ==
              (arch == Architecture::ARM64 ? 0x2004u : 0x2007u));
        CHECK(prefix.successors() == std::set<std::uint64_t>{0x2008});
        // Older automatic registration kept these views/IR but did not set
        // the late instruction's entry flags; flags are not the premise here.
      }
}

TEST_CASE("Loader classifies a forward cross section callee already decoded in a prefix",
          "[decoded-entries][loader][late-decoded-entry]") {
  for (auto arch : {Architecture::X86_64, Architecture::ARM64})
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      auto lower = transfer(arch, 0x1000, 0x2010, true);
      append(lower, ret(arch));
      Bytes upper;
      if (arch == Architecture::ARM64)
        for (unsigned i = 0; i < 4; ++i) appendWord(upper, 0xd503201f);
      else upper.assign(0x10, 0x90);
      append(upper, ret(arch));
      std::string error;
      auto image = loader::LoadedImage::loadBytes(
          twoSectionElf(arch, 0x1000, lower, 0x2000, upper, 0x1000), &error);
      INFO(error);
      REQUIRE(image);
      REQUIRE(image->architecture() == arch);
      REQUIRE(image->executableSections().size() == 2);
      auto graph = loader::disassembleImage(*image, configuration(strategy));
      REQUIRE(graph.get(0x1000));
      CHECK(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0x2010});
      REQUIRE(graph.get(0x2000));
      REQUIRE(graph.get(0x2010));
      CHECK(graph.get(0x2010)->isReturn());
      CHECK(graph.isValidBlock(0x2010));
      CHECK(graph.isValidFunction(0x2010));
      CHECK(BasicBlock{&graph, 0x2010}.isValid());
      CHECK(Function{&graph, 0x2010}.isValid());
      CHECK(graph.getIGNILBlock(0x2010) != nullptr);
      if (const auto *ir = graph.getIGNILBlock(0x2010)) {
        CHECK(ir->unlifted().empty());
        REQUIRE_FALSE(ir->ops().empty());
        CHECK(ir->ops().back().opcode == ignil::Opcode::RET);
      }
      const BasicBlock prefix{&graph, 0x2000};
      REQUIRE(prefix.isValid());
      CHECK(prefix.terminatorAddress() ==
            (arch == Architecture::ARM64 ? 0x200cu : 0x200fu));
      CHECK(prefix.successors() == std::set<std::uint64_t>{0x2010});
    }
}

TEST_CASE("Decoded entries classify a historical callee recovered from overlapping bytes",
          "[decoded-entries][analysis][late-decoded-entry]") {
  for (auto arch : {Architecture::X86_32, Architecture::X86_64})
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      const auto config = configuration(strategy);
      Disassembler dis{arch, config};
      Graph graph{arch, config};
      auto caller = transfer(arch, 0x1000, 0x2008, true);
      append(caller, ret(arch));
      dis.disassemble(caller, 0x1000, graph);
      REQUIRE(graph.get(0x2008) == nullptr);

      // Linear sweep decodes the target C3 as MOV's immediate. The local
      // direct JMP recovers it only after that initial sweep has completed.
      const Bytes late = arch == Architecture::X86_64
          ? Bytes{0xeb, 0x06, 0x48, 0xb8, 0x00, 0x00,
                  0x00, 0x00, 0xc3, 0x00, 0x00, 0x00}
          : Bytes{0xeb, 0x06, 0x90, 0x90, 0xb8, 0x00,
                  0x00, 0x00, 0xc3, 0x00, 0x00, 0x00};
      dis.disassemble(late, 0x2000, graph);
      REQUIRE(graph.get(0x2000));
      CHECK(graph.get(0x2000)->branches() == std::set<std::uint64_t>{0x2008});
      REQUIRE(graph.get(0x2008));
      CHECK(graph.get(0x2008)->isReturn());
      decodedEntry(graph, 0x2008);
      CHECK(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0x2008});
      REQUIRE(graph.getIGNILBlock(0x2008));
      CHECK(graph.getIGNILBlock(0x2008)->unlifted().empty());
    }
}

TEST_CASE("Decoded entries classify a historical callee decoded by indirect expansion",
          "[decoded-entries][analysis][late-decoded-entry]") {
  for (auto arch : architectures)
    for (auto strategy : strategies) {
      CAPTURE(arch, strategy);
      const auto config = configuration(strategy);
      Disassembler dis{arch, config};
      Graph graph{arch, config};
      auto caller = transfer(arch, 0x1000, 0x2010, true);
      append(caller, ret(arch));
      dis.disassemble(caller, 0x1000, graph);
      REQUIRE(graph.get(0x2010) == nullptr);

      Bytes late;
      std::uint64_t indirect;
      if (arch == Architecture::ARM64) {
        appendWord(late, 0xd2840200); // MOVZ X0,#0x2010.
        appendWord(late, 0xd61f0000); // BR X0.
        appendWord(late, 0xd503201f);
        appendWord(late, 0xd503201f);
        indirect = 0x2004;
      } else {
        late = {0xb8, 0x10, 0x20, 0x00, 0x00, 0xff, 0xe0}; // MOV EAX; JMP [R/E]AX.
        late.resize(0x10, 0xcc);
        indirect = 0x2005;
      }
      append(late, ret(arch));
      dis.disassemble(late, 0x2000, graph);
      REQUIRE(graph.get(indirect));
      REQUIRE(graph.get(indirect)->hasIndirectTarget());
      // ARM linear recovery deliberately accepts table proofs only. Keep
      // that existing unresolved case as a negative control; recursive
      // recovery is the path that decodes this late target by expansion.
      if (arch == Architecture::ARM64 &&
          strategy == DisassemblyStrategy::LINEAR_SWEEP)
        CHECK(graph.get(indirect)->branches().empty());
      else
        CHECK(graph.get(indirect)->branches() == std::set<std::uint64_t>{0x2010});
      REQUIRE(graph.get(0x2010));
      CHECK(graph.get(0x2010)->isReturn());
      decodedEntry(graph, 0x2010);
      CHECK(graph.get(0x1000)->callees() == std::set<std::uint64_t>{0x2010});
      REQUIRE(graph.getIGNILBlock(0x2010));
      CHECK(graph.getIGNILBlock(0x2010)->unlifted().empty());
    }
}
