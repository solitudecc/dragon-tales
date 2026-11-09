#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;

namespace {
using O = ignil::Opcode;
using W = ignil::Width;
constexpr std::uint64_t entry = 0x1000;
constexpr std::uint64_t cell = 0x4000;

void add(Graph &graph, ignil::Block block) {
  const auto address = block.startAddress();
  graph.addIGNILBlock(std::move(block));
  graph.addBlock(address);
}

void store(ignil::Block &block, ignil::Value address, ignil::Value value) {
  block.append({O::STORE, {}, {std::move(address), std::move(value)}, entry});
}

void byte(ignil::Block &block, std::uint64_t address, std::uint64_t value) {
  store(block, ignil::Const{address, 64}, ignil::Const{value, 8});
}

FiniteValueQuery load(ignil::Block &block, std::uint32_t id,
                      ignil::Value address, W width = 8) {
  const ignil::Temp value{id, width};
  block.append({O::LOAD, value, {std::move(address)}, entry});
  return {entry, value};
}

void equal(const FiniteValues &value, std::uint64_t expected) {
  CHECK(value.kind == ValueSetKind::Finite);
  CHECK(value.values == std::vector<std::uint64_t>{expected});
}

void unknown(const FiniteValues &value) {
  CHECK(value.kind == ValueSetKind::Unknown);
  CHECK(value.values.empty());
}

std::uint64_t mask(W width) {
  return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << width) - 1;
}

ignil::Temp stackAddress(ignil::Block &block, std::uint32_t id,
                         const ignil::Reg &sp, std::uint64_t offset) {
  const ignil::Temp address{id, sp.width};
  block.append({O::ADD, address,
                {sp, ignil::Const{offset & mask(sp.width), sp.width}}, entry});
  return address;
}
} // namespace

TEST_CASE("Finite wide writes invalidate their byte footprint without erasing disjoint cells",
          "[finite-values][finite-memory][finite-write-footprints]") {
  // Width is uint8_t: 256 is not representable. In particular 255 must keep
  // its 32-byte footprint rather than round through Width(256) to zero.
  for (W width : {W{1}, W{7}, W{9}, W{63}, W{64}, W{65}, W{127},
                  W{128}, W{129}, W{248}, W{255}}) {
    CAPTURE(unsigned(width));
    const auto count = (unsigned(width) + 7) / 8;
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    byte(block, cell - 1, 0x11);
    byte(block, cell, 0x22);
    byte(block, cell + count - 1, 0x33);
    byte(block, cell + count, 0x44);
    const ignil::Temp value{0, width};
    block.append({O::UNDEF, value, {}, entry});
    store(block, ignil::Const{cell, 64}, value);
    const std::array queries{
        load(block, 10, ignil::Const{cell - 1, 64}),
        load(block, 11, ignil::Const{cell, 64}),
        load(block, 12, ignil::Const{cell + count - 1, 64}),
        load(block, 13, ignil::Const{cell + count, 64})};
    add(graph, std::move(block));
    const auto result = finiteValues(graph, {entry}, queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    equal(result.values[0], 0x11);
    unknown(result.values[1]);
    unknown(result.values[2]);
    equal(result.values[3], 0x44);
  }
}

TEST_CASE("Finite candidate writes invalidate the union and never initialize every candidate",
          "[finite-values][finite-memory][finite-write-footprints]") {
  for (W width : {W{8}, W{16}, W{128}, W{255}}) {
    CAPTURE(unsigned(width));
    const auto count = (unsigned(width) + 7) / 8;
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    for (unsigned i = 0; i != 4; ++i) {
      const auto address = cell + 64 * i;
      byte(block, address - 1, 0x11);
      byte(block, address, 0x22);
      byte(block, address + count - 1, 0x33);
      byte(block, address + count, 0x44);
    }
    const ignil::Temp index{0, 64}, scaled{1, 64}, address{2, 64};
    block.append({O::AND, index, {x86::RDI, ignil::Const{3, 64}}, entry});
    block.append({O::SHL, scaled, {index, ignil::Const{6, 64}}, entry});
    block.append({O::ADD, address, {scaled, ignil::Const{cell, 64}}, entry});
    if (width <= 64)
      store(block, address, ignil::Const{9, width});
    else {
      const ignil::Temp value{3, width};
      block.append({O::UNDEF, value, {}, entry});
      store(block, address, value);
    }
    std::vector<FiniteValueQuery> queries;
    for (unsigned i = 0; i != 4; ++i) {
      const auto at = cell + 64 * i;
      queries.push_back(load(block, 10 + 4 * i, ignil::Const{at - 1, 64}));
      queries.push_back(load(block, 11 + 4 * i, ignil::Const{at, 64}));
      queries.push_back(load(block, 12 + 4 * i, ignil::Const{at + count - 1, 64}));
      queries.push_back(load(block, 13 + 4 * i, ignil::Const{at + count, 64}));
    }
    add(graph, std::move(block));
    const auto result = finiteValues(graph, {entry}, queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    for (unsigned i = 0; i != 4; ++i) {
      equal(result.values[4 * i], 0x11);
      // Even a constant stored value is written at only one actual address.
      unknown(result.values[4 * i + 1]);
      unknown(result.values[4 * i + 2]);
      equal(result.values[4 * i + 3], 0x44);
    }
  }
}

TEST_CASE("Finite wide writes retain unwritten fragments of overlapping scalar facts",
          "[finite-values][finite-memory][finite-write-footprints]") {
  for (W width : {W{65}, W{128}, W{255}}) {
    CAPTURE(unsigned(width));
    const auto count = (unsigned(width) + 7) / 8;
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    store(block, ignil::Const{cell - 4, 64},
          ignil::Const{0x8877665544332211ULL, 64});
    store(block, ignil::Const{cell + count - 4, 64},
          ignil::Const{0x1122334455667788ULL, 64});
    const ignil::Temp value{0, width};
    block.append({O::UNDEF, value, {}, entry});
    store(block, ignil::Const{cell, 64}, value);
    const std::array queries{
        load(block, 10, ignil::Const{cell - 4, 64}, 32),
        load(block, 11, ignil::Const{cell, 64}, 32),
        load(block, 12, ignil::Const{cell + count - 4, 64}, 32),
        load(block, 13, ignil::Const{cell + count, 64}, 32)};
    add(graph, std::move(block));
    const auto result = finiteValues(graph, {entry}, queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    equal(result.values[0], 0x44332211);
    unknown(result.values[1]);
    unknown(result.values[2]);
    equal(result.values[3], 0x11223344);
  }
}

TEST_CASE("Finite candidate footprints preserve complete bytes of partially overlapped facts",
          "[finite-values][finite-memory][finite-write-footprints]") {
  Graph graph{Architecture::X86_64, {}};
  ignil::Block block{entry};
  store(block, ignil::Const{cell, 64}, ignil::Const{0x1122334455667788ULL, 64});
  const ignil::Temp index{0, 64}, scaled{1, 64}, address{2, 64};
  block.append({O::AND, index, {x86::RDI, ignil::Const{1, 64}}, entry});
  block.append({O::SHL, scaled, {index, ignil::Const{4, 64}}, entry});
  block.append({O::ADD, address, {scaled, ignil::Const{cell + 2, 64}}, entry});
  store(block, address, ignil::Const{0x1ff, 9});
  const std::array queries{
      load(block, 10, ignil::Const{cell, 64}, 16),
      load(block, 11, ignil::Const{cell + 2, 64}),
      load(block, 12, ignil::Const{cell + 3, 64}),
      load(block, 13, ignil::Const{cell + 4, 64}, 32),
      load(block, 14, ignil::Const{cell, 64}, 64)};
  add(graph, std::move(block));
  const auto result = finiteValues(graph, {entry}, queries);
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == queries.size());
  equal(result.values[0], 0x7788);
  unknown(result.values[1]);
  unknown(result.values[2]);
  equal(result.values[3], 0x11223344);
  unknown(result.values[4]);
}

TEST_CASE("Finite concrete candidate footprints wrap in the 64 bit address space",
          "[finite-values][finite-memory][finite-write-footprints]") {
  constexpr auto top = std::numeric_limits<std::uint64_t>::max();
  Graph graph{Architecture::X86_64, {}};
  ignil::Block block{entry};
  for (auto address : {top - 4, top - 3, std::uint64_t{0},
                       std::uint64_t{4}, std::uint64_t{11}, std::uint64_t{12}})
    byte(block, address, 0x77);
  const ignil::Temp index{0, 64}, scaled{1, 64}, address{2, 64};
  block.append({O::AND, index, {x86::RDI, ignil::Const{1, 64}}, entry});
  block.append({O::SHL, scaled, {index, ignil::Const{3, 64}}, entry});
  block.append({O::ADD, address, {scaled, ignil::Const{top - 3, 64}}, entry});
  store(block, address, ignil::Const{9, 64});
  const std::array queries{
      load(block, 10, ignil::Const{top - 4, 64}),
      load(block, 11, ignil::Const{top - 3, 64}),
      load(block, 12, ignil::Const{0, 64}),
      load(block, 13, ignil::Const{4, 64}),
      load(block, 14, ignil::Const{11, 64}),
      load(block, 15, ignil::Const{12, 64})};
  add(graph, std::move(block));
  const auto result = finiteValues(graph, {entry}, queries);
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == queries.size());
  equal(result.values[0], 0x77);
  for (unsigned i = 1; i != 5; ++i) unknown(result.values[i]);
  equal(result.values[5], 0x77);
}

TEST_CASE("Finite stack candidate footprints wrap at the architectural pointer width",
          "[finite-values][finite-memory][finite-write-footprints]") {
  for (bool wide : {false, true}) {
    CAPTURE(wide);
    const auto sp = wide ? x86::RSP : x86::ESP;
    const auto input = wide ? x86::RDI : x86::EDI;
    Graph graph{wide ? Architecture::X86_64 : Architecture::X86_32, {}};
    ignil::Block block{entry};
    const std::array<std::uint64_t, 6> offsets{
        mask(sp.width) - 4, mask(sp.width) - 3, 0, 4, 11, 12};
    for (unsigned i = 0; i != offsets.size(); ++i) {
      const auto address = stackAddress(block, 20 + i, sp, offsets[i]);
      store(block, address, ignil::Const{0x77, 8});
    }
    const ignil::Temp index{0, sp.width}, scaled{1, sp.width},
        start{2, sp.width}, address{3, sp.width};
    block.append({O::AND, index, {input, ignil::Const{1, sp.width}}, entry});
    block.append({O::SHL, scaled, {index, ignil::Const{3, sp.width}}, entry});
    block.append({O::ADD, start, {sp, ignil::Const{mask(sp.width) - 3, sp.width}}, entry});
    block.append({O::ADD, address, {start, scaled}, entry});
    store(block, address, ignil::Const{9, 64});
    std::vector<FiniteValueQuery> queries;
    for (unsigned i = 0; i != offsets.size(); ++i) {
      const auto at = stackAddress(block, 40 + i, sp, offsets[i]);
      queries.push_back(load(block, 60 + i, at));
    }
    add(graph, std::move(block));
    const auto result = finiteValues(graph, {entry}, queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    equal(result.values[0], 0x77);
    for (unsigned i = 1; i != 5; ++i) unknown(result.values[i]);
    equal(result.values[5], 0x77);
  }
}

TEST_CASE("Finite footprints do not assume distinct unknown bases are disjoint",
          "[finite-values][finite-memory][finite-write-footprints]") {
  enum Scenario { UnknownPointer, StackMayAliasConcrete, ConcreteMayAliasStack };
  for (auto scenario : {UnknownPointer, StackMayAliasConcrete, ConcreteMayAliasStack}) {
    CAPTURE(scenario);
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    const auto stack = stackAddress(block, 0, x86::RSP, 16);
    const ignil::Value saved = scenario == ConcreteMayAliasStack
                                  ? ignil::Value{stack}
                                  : ignil::Value{ignil::Const{cell, 64}};
    store(block, saved, ignil::Const{7, 64});
    const ignil::Value written = scenario == UnknownPointer ? ignil::Value{x86::RDI}
        : scenario == StackMayAliasConcrete ? ignil::Value{stack}
                                           : ignil::Value{ignil::Const{cell, 64}};
    store(block, written, ignil::Const{9, 128});
    const std::array queries{load(block, 1, saved, 64)};
    add(graph, std::move(block));
    const auto result = finiteValues(graph, {entry}, queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == 1);
    unknown(result.values[0]);
  }
}

TEST_CASE("Finite zero width writes remain an unknown memory barrier",
          "[finite-values][finite-memory][finite-write-footprints]") {
  Graph graph{Architecture::X86_64, {}};
  ignil::Block block{entry};
  byte(block, cell, 7);
  byte(block, cell + 0x100, 8);
  store(block, ignil::Const{cell, 64}, ignil::Const{0, 0});
  const std::array queries{load(block, 0, ignil::Const{cell, 64}),
                           load(block, 1, ignil::Const{cell + 0x100, 64})};
  add(graph, std::move(block));
  const auto result = finiteValues(graph, {entry}, queries);
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == queries.size());
  for (const auto &value : result.values) unknown(value);
}

TEST_CASE("Finite write footprint budget failures discard every query and target",
          "[finite-values][finite-memory][finite-write-footprints]") {
  Graph graph{Architecture::X86_64, {}};
  ignil::Block block{entry};
  byte(block, cell, 7);
  byte(block, cell + 0x100, 8);
  const ignil::Temp index{0, 64}, address{1, 64};
  block.append({O::AND, index, {x86::RDI, ignil::Const{1, 64}}, entry});
  block.append({O::ADD, address, {index, ignil::Const{cell, 64}}, entry});
  store(block, address, ignil::Const{0, 128});
  const std::array queries{load(block, 2, ignil::Const{cell + 0x100, 64})};
  block.append({O::JMP, {}, {ignil::Const{0x9000, 64}}, entry});
  add(graph, std::move(block));
  const auto full = finiteValues(graph, {entry}, queries);
  REQUIRE(full.complete);
  REQUIRE(full.operations > 0);
  for (bool factLimit : {false, true}) {
    CAPTURE(factLimit);
    FiniteValueLimits limits;
    if (factLimit) limits.maxFacts = 0;
    else limits.maxOperations = static_cast<std::uint32_t>(full.operations - 1);
    const auto result = finiteValues(graph, {entry}, queries, limits);
    CHECK_FALSE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    for (const auto &value : result.values) unknown(value);
    REQUIRE(result.jumpTargets.contains(entry));
    unknown(result.jumpTargets.at(entry));
  }
}
