#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include "../../lib/LLVMPreparedRegion.hpp"
#include <array>
#include <type_traits>

using namespace dragon;
using namespace dragon::llvmlifter;
using dragon::llvmlifter::detail::PreparedRegion;

namespace {
const std::array architectures{Architecture::X86_32, Architecture::X86_64, Architecture::ARM64};
const std::array strategies{DisassemblyStrategy::LINEAR_SWEEP, DisassemblyStrategy::RECURSIVE_TRAVERSAL};
Graph sample(Architecture arch, DisassemblyStrategy strategy) {
  Configuration config;config.disassemblyStrategy = strategy;
  Graph graph{arch,config};Disassembler dis{arch,config};
  const auto root = arch == Architecture::ARM64 ? std::vector<std::uint8_t>{0,0,0x1f,0xd6}
                                               : std::vector<std::uint8_t>{0xff,0xe0};
  const auto body = arch == Architecture::ARM64 ? std::vector<std::uint8_t>{0xe1,0,0x80,0xd2,0xc0,3,0x5f,0xd6}
                                               : std::vector<std::uint8_t>{0xb9,7,0,0,0,0xc3};
  dis.disassemble(root,0x1000,graph);dis.disassemble(body,0x2000,graph);
  return graph;
}
const std::array<std::uint64_t,1> extra{0x2000};
}

TEST_CASE("Prepared regions own immutable code after the source is destroyed", "[llvm][prepared-region]") {
  static_assert(std::is_same_v<decltype(std::declval<PreparedRegion>().graph()),const Graph &>);
  for (auto arch : architectures) for (auto strategy : strategies) {
    CAPTURE(arch,strategy);
    std::unique_ptr<PreparedRegion> prepared;
    const Instruction *sourceInstruction = nullptr;
    {
      auto source = sample(arch,strategy);
      sourceInstruction = source.get(0x1000);
      prepared = PreparedRegion::prepare(Function{&source,0x1000},source,extra,{});
      REQUIRE(prepared);
      CHECK(prepared->graph().get(0x1000) != sourceInstruction);
      auto *ir = source.getIGNILBlock(0x1000);REQUIRE(ir);
      *ir = ignil::Block{0x1000};
      CHECK_FALSE(prepared->graph().getIGNILBlock(0x1000)->empty());
    }
    REQUIRE(prepared->root().isValid());
    CHECK(prepared->blocks().size() == 2);
    CHECK(prepared->root().blocks().size() == 1);
    CHECK_FALSE(prepared->rebuilt());
    for (const auto &[address, block] : prepared->blocks()) {
      REQUIRE(block.isValid());
      REQUIRE_FALSE(block.instructions().empty());
      CHECK(block.instructions().front() == prepared->graph().get(address));
    }
    auto origin = prepared->origin(0x1000,0);REQUIRE(origin);
    PreparedRegion moved{std::move(*prepared)};
    prepared.reset();
    CHECK(moved.owns(*origin));
    CHECK(moved.blocks().at(0x2000).instructions().front() == moved.graph().get(0x2000));
    CHECK(moved.root().isValid());
  }
}

TEST_CASE("Prepared operation identities distinguish instances and same PC definitions", "[llvm][prepared-region]") {
  auto graph = sample(Architecture::X86_64,DisassemblyStrategy::LINEAR_SWEEP);
  ignil::Block custom{0x1000};
  custom.append({ignil::Opcode::COPY,ignil::Temp{1,64},{ignil::Const{7,64}},0x1000});
  custom.append({ignil::Opcode::COPY,ignil::Temp{2,64},{ignil::Const{9,64}},0x1000});
  custom.append({ignil::Opcode::JMP,std::nullopt,{ignil::Reg{0,64}},0x1000});
  custom.noteUnlifted(0x1000);
  *graph.getIGNILBlock(0x1000) = custom;
  auto first = PreparedRegion::prepare(Function{&graph,0x1000},graph,extra,{});
  auto second = PreparedRegion::prepare(Function{&graph,0x1000},graph,extra,{});
  REQUIRE(first);REQUIRE(second);
  auto a = first->origin(0x1000,0), b = first->origin(0x1000,1);
  REQUIRE(a);REQUIRE(b);
  CHECK(a->machinePC == b->machinePC);CHECK_FALSE(*a == *b);
  CHECK(first->owns(*a));CHECK_FALSE(second->owns(*a));
  auto corrupt = *a;corrupt.opIndex = 999999;CHECK_FALSE(first->owns(corrupt));
  CHECK_FALSE(first->origin(0x1001,0));CHECK_FALSE(first->origin(0x1000,3));
  CHECK(first->graph().getIGNILBlock(0x1000)->unlifted() == custom.unlifted());
  CHECK(ignil::toString(*first->graph().getIGNILBlock(0x1000)) == ignil::toString(custom));
  StaticRegionOptions rebuild;rebuild.rebuildDecodedBlocks = true;
  auto machine = PreparedRegion::prepare(Function{&graph,0x1000},graph,extra,rebuild);
  REQUIRE(machine);CHECK(machine->rebuilt());CHECK_FALSE(machine->owns(*a));
  CHECK(machine->graph().getIGNILBlock(0x1000)->unlifted().empty());
  CHECK_FALSE(ignil::toString(*machine->graph().getIGNILBlock(0x1000)) == ignil::toString(custom));
  CHECK(ignil::toString(*graph.getIGNILBlock(0x1000)) == ignil::toString(custom));
}

TEST_CASE("Prepared machine regions split and rebuild old block prefixes", "[llvm][prepared-region]") {
  for (auto arch : architectures) for (auto strategy : strategies) {
    CAPTURE(arch,strategy);
    Configuration config;config.disassemblyStrategy = strategy;
    Disassembler dis{arch,config};Graph graph{arch,config};
    const auto code = arch == Architecture::ARM64
        ? std::vector<std::uint8_t>{0xe0,0,0x80,0xd2,0x21,1,0x80,0xd2,0xc0,3,0x5f,0xd6}
        : std::vector<std::uint8_t>{0xb8,7,0,0,0,0xb9,9,0,0,0,0xc3};
    dis.disassemble(code,0x1000,graph);
    const std::uint64_t split = arch == Architecture::ARM64 ? 0x1004 : 0x1005;
    const auto text = ignil::toString(*graph.getIGNILBlock(0x1000));
    StaticRegionOptions options;options.rebuildDecodedBlocks = true;
    auto prepared = PreparedRegion::prepare(Function{&graph,0x1000},graph,std::span{&split,1},options);
    REQUIRE(prepared);REQUIRE(prepared->blocks().size() == 2);
    const auto *prefix = prepared->graph().getIGNILBlock(0x1000);REQUIRE(prefix);REQUIRE_FALSE(prefix->empty());
    CHECK(prefix->ops().back().opcode == ignil::Opcode::JMP);
    CHECK(std::get<ignil::Const>(prefix->ops().back().srcs.at(0)).value == split);
    CHECK(prepared->blocks().at(0x1000).instructions().size() == 1);
    CHECK(prepared->blocks().at(split).instructions().size() == 2);
    CHECK(ignil::toString(*graph.getIGNILBlock(0x1000)) == text);
    CHECK_FALSE(graph.isValidBlock(split));
    CHECK(prepared->origin(split,0)->block == split);
  }
}

TEST_CASE("Prepared region rejection is local and does not poison a later request", "[llvm][prepared-region]") {
  for (auto arch : architectures) {
    auto graph = sample(arch,DisassemblyStrategy::LINEAR_SWEEP);
    const Function root{&graph,0x1000};
    const auto text = ignil::toString(*graph.getIGNILBlock(0x1000));
    for (const auto options : {StaticRegionOptions{0,65536,false},StaticRegionOptions{4097,65536,false},
                              StaticRegionOptions{4096,0,false},StaticRegionOptions{4096,65537,false},
                              StaticRegionOptions{1,65536,false},StaticRegionOptions{4096,1,false}})
      CHECK_FALSE(PreparedRegion::prepare(root,graph,extra,options));
    const std::uint64_t missing = 0x9999;
    CHECK_FALSE(PreparedRegion::prepare(root,graph,std::span{&missing,1},{}));
    auto foreign = graph.clone();
    CHECK_FALSE(PreparedRegion::prepare(root,foreign,extra,{}));
    CHECK(PreparedRegion::prepare(root,graph,extra,{}));
    CHECK(ignil::toString(*graph.getIGNILBlock(0x1000)) == text);
  }
}
