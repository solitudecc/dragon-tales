#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <array>
#include <limits>
#include <set>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
using O = ignil::Opcode;
constexpr std::uint64_t cell = 0x4000;
void add(Graph &graph, ignil::Block block) {
  const auto address = block.startAddress();
  graph.addIGNILBlock(std::move(block));
  graph.addBlock(address);
}
void store(ignil::Block &block, std::uint64_t at, std::uint64_t value,
           ignil::Width width = 32) {
  block.append({O::STORE, {}, {ignil::Const{at,64},ignil::Const{value,width}},block.startAddress()});
}
void load(ignil::Block &block, std::uint32_t id = 0,
          ignil::Width width = 32, std::uint64_t at = cell) {
  block.append({O::LOAD, ignil::Temp{id,width}, {ignil::Const{at,64}},block.startAddress()});
}
void jump(ignil::Block &block, std::uint64_t target) {
  block.append({O::JMP, {}, {ignil::Const{target,64}},block.startAddress()});
}
void equal(const FiniteValues &value, std::vector<std::uint64_t> expected) {
  REQUIRE(value.kind == ValueSetKind::Finite);
  CHECK(value.values == expected);
}
FiniteValues query(const Graph &graph, FiniteMemoryQuery item,
                   const std::set<std::uint64_t> &entries = {0x1000}) {
  const auto result = finiteMemoryValues(graph,entries,std::span{&item,1});
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == 1);
  return result.values.front();
}
} // namespace

TEST_CASE("Finite memory snapshots belong to the actual LOAD before later writes",
          "[finite-values][finite-memory-query]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    CAPTURE(arch);
    Graph graph{arch,{}};
    ignil::Block block{0x1000};
    store(block,cell,0x11223344);
    store(block,cell + 4,0xaabbccdd);
    load(block,0);                         // index 2: capture the old table value
    store(block,cell,0x55667788);
    load(block,1);                         // index 4: a later snapshot
    block.append({O::JMP, {}, {ignil::Temp{0,32}},0x1000});
    add(graph,std::move(block));
    const std::array queries{
      FiniteMemoryQuery{0x1000,2,cell,32},
      FiniteMemoryQuery{0x1000,2,cell + 4,32},
      FiniteMemoryQuery{0x1000,4,cell,32}};
    const FiniteValueQuery captured{0x1000,ignil::Temp{0,32}};
    const auto oldAPI = finiteValues(graph,{0x1000},std::span{&captured,1}).toJSON();
    const auto before = ignil::toString(*graph.getIGNILBlock(0x1000));
    const auto result = finiteMemoryValues(graph,{0x1000},queries);
    REQUIRE(result.complete);
    REQUIRE(result.values.size() == 3);
    equal(result.values[0],{0x11223344});
    equal(result.values[1],{0xaabbccdd});
    equal(result.values[2],{0x55667788});
    equal(result.jumpTargets.at(0x1000),{0x11223344});
    CHECK(ignil::toString(*graph.getIGNILBlock(0x1000)) == before);
    CHECK(finiteValues(graph,{0x1000},std::span{&captured,1}).toJSON() == oldAPI);
  }
}

TEST_CASE("Finite memory composes partial stores without inventing unknown bytes",
          "[finite-values][finite-memory-query]") {
  bool unknown = false;
  SECTION("known byte overwrite") {}
  SECTION("unknown byte overwrite") { unknown = true; }
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  store(block,cell,0x1122334455667788ULL,64);
  if (unknown)
    block.append({O::STORE, {}, {ignil::Const{cell + 2,64},x86::AL},0x1000});
  else store(block,cell + 2,0xcc,8);
  load(block,0,64);
  load(block,1,8);
  add(graph,std::move(block));
  const std::array queries{FiniteMemoryQuery{0x1000,2,cell,64},FiniteMemoryQuery{0x1000,3,cell,8}};
  const auto result = finiteMemoryValues(graph,{0x1000},queries);
  REQUIRE(result.complete);
  if (unknown) CHECK(result.values[0].kind == ValueSetKind::Unknown);
  else equal(result.values[0],{0x1122334455cc7788ULL});
  equal(result.values[1],{0x88});
}

TEST_CASE("Finite memory has no image facts and keeps alias call and opaque barriers",
          "[finite-values][finite-memory-query]") {
  bool known = false;
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  SECTION("uninitialized bytes stay unknown") {}
  SECTION("proven disjoint write preserves the value") {
    known = true;
    store(block,cell,7);
    block.append({O::STORE, {}, {ignil::Const{cell + 4,64},x86::EDI},0x1000});
  }
  SECTION("unknown pointer store may overwrite the table") {
    store(block,cell,7);
    block.append({O::STORE, {}, {x86::RDI,ignil::Const{9,32}},0x1000});
  }
  SECTION("CALL may overwrite the table") {
    store(block,cell,7);
    block.append({O::CALL, {}, {ignil::Const{0x5000,64}},0x1000});
  }
  SECTION("zero op unlifted memory effect invalidates the facts") {
    store(block,cell,7);
    block.noteUnlifted(0x1001);
  }
  SECTION("local control is not flattened into a store proof") {
    store(block,cell,7);
    block.append({O::LABEL, {}, {},0x1000});
  }
  const auto index = block.ops().size();
  load(block);
  add(graph,std::move(block));
  const auto value = query(graph,{0x1000,index,cell,32});
  if (known) equal(value,{7});
  else CHECK(value.kind == ValueSetKind::Unknown);
}

TEST_CASE("Finite memory joins every incoming path rather than the first initializer",
          "[finite-values][finite-memory-query]") {
  bool missing = false;
  SECTION("both paths initialize distinct values") {}
  SECTION("one incoming path has no initializer") { missing = true; }
  Graph graph{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};
  entry.append({O::CJMP, {}, {x86::ZF,ignil::Const{0x1010,64},ignil::Const{0x1020,64}},0x1000});
  add(graph,std::move(entry));
  for (auto [address,value] : {std::pair{0x1010u,7u},std::pair{0x1020u,9u}}) {
    ignil::Block arm{address};
    if (!missing || address == 0x1010) store(arm,cell,value);
    jump(arm,0x1030);
    add(graph,std::move(arm));
  }
  ignil::Block out{0x1030};
  load(out);
  add(graph,std::move(out));
  const auto value = query(graph,{0x1030,0,cell,32});
  if (missing) CHECK(value.kind == ValueSetKind::Unknown);
  else equal(value,{7,9});
}

TEST_CASE("Finite memory reaches a loop fixed point before reporting load facts",
          "[finite-values][finite-memory-query]") {
  bool alias = false;
  SECTION("all loop writes are known") {}
  SECTION("a later iteration may alias the table") { alias = true; }
  Graph graph{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};
  store(entry,cell,1);
  jump(entry,0x1010);
  add(graph,std::move(entry));
  ignil::Block head{0x1010};
  load(head);
  head.append({O::CJMP, {}, {x86::ZF,ignil::Const{0x1020,64},ignil::Const{0x1030,64}},0x1010});
  add(graph,std::move(head));
  ignil::Block body{0x1020};
  if (alias) body.append({O::STORE, {}, {x86::RDI,ignil::Const{9,32}},0x1020});
  else store(body,cell,9);
  jump(body,0x1010);
  add(graph,std::move(body));
  ignil::Block out{0x1030};
  out.append({O::RET, {}, {},0x1030});
  add(graph,std::move(out));
  const auto value = query(graph,{0x1010,0,cell,32});
  if (alias) CHECK(value.kind == ValueSetKind::Unknown);
  else equal(value,{1,9});
}

TEST_CASE("Finite memory admits independent entries and discovered CALL inputs",
          "[finite-values][finite-memory-query]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};
  store(entry,cell,7);
  jump(entry,0x1010);
  add(graph,std::move(entry));
  ignil::Block out{0x1010};
  load(out);
  add(graph,std::move(out));
  const FiniteMemoryQuery item{0x1010,0,cell,32};
  equal(query(graph,item),{7});
  CHECK(query(graph,item,{0x1000,0x1010}).kind == ValueSetKind::Unknown);
  graph.addFunction(0x1000);
  graph.addFunction(0x1010);
  CHECK(query(graph,item,{}).kind == ValueSetKind::Unknown);
  // Even with explicit roots, a decoded direct CALL independently admits
  // arbitrary input state at its callee; a tail path's store is insufficient.
  Instruction call{0x2000,5,"call","0x1010",{0xe8,0x0b,0xf0,0xff,0xff},false,true,false,false,false,false};
  call.addCallee(0x1010);
  graph.addInstruction(std::move(call));
  CHECK(query(graph,item).kind == ValueSetKind::Unknown);

  Graph cyclic{Architecture::X86_64,{}};
  ignil::Block loop{0x3000};
  load(loop);
  store(loop,cell,7);
  jump(loop,0x3000);
  add(cyclic,std::move(loop));
  CHECK(query(cyclic,{0x3000,0,cell,32},{}).kind == ValueSetKind::Unknown);
}

TEST_CASE("Finite memory validates the LOAD program point before reachability",
          "[finite-values][finite-memory-query]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  store(block,cell,7);
  load(block);
  add(graph,std::move(block));
  ignil::Block dead{0x2000};
  load(dead);
  dead.append({O::RET, {}, {},0x2000});
  add(graph,std::move(dead));
  const std::array queries{
    FiniteMemoryQuery{0x1000,1,cell,32},
    FiniteMemoryQuery{0x1000,0,cell,32}, // STORE is not a LOAD point
    FiniteMemoryQuery{0x1000,2,cell,32},
    FiniteMemoryQuery{0x1000,std::numeric_limits<std::size_t>::max(),cell,32},
    FiniteMemoryQuery{0x1000,1,cell,8},  // width must agree with the LOAD
    FiniteMemoryQuery{0x1000,1,cell,0},
    FiniteMemoryQuery{0x1000,1,cell,128},
    FiniteMemoryQuery{0x9999,0,cell,32},
    FiniteMemoryQuery{0x2000,0,cell,32}, // valid but unreachable
    FiniteMemoryQuery{0x2000,1,cell,32}}; // invalid even though unreachable
  const auto result = finiteMemoryValues(graph,{0x1000},queries);
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == queries.size());
  equal(result.values[0],{7});
  for (std::size_t i=1;i<queries.size();++i) {
    CAPTURE(i);
    CHECK(result.values[i].kind == (i == 8 ? ValueSetKind::Unreachable : ValueSetKind::Unknown));
    CHECK(result.values[i].values.empty());
    CHECK(result.values[i].width == queries[i].width);
  }
}

TEST_CASE("Finite memory budget failure invalidates every reported snapshot",
          "[finite-values][finite-memory-query]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  store(block,cell,7);
  load(block,0);
  load(block,1);
  block.append({O::JMP, {}, {ignil::Temp{0,32}},0x1000});
  add(graph,std::move(block));
  ignil::Block dead{0x2000};
  load(dead);
  add(graph,std::move(dead));
  const std::array queries{FiniteMemoryQuery{0x1000,1,cell,32},
                           FiniteMemoryQuery{0x1000,2,cell,32},FiniteMemoryQuery{0x2000,0,cell,32}};
  const auto full = finiteMemoryValues(graph,{0x1000},queries);
  REQUIRE(full.complete);
  REQUIRE(full.operations > 1);
  equal(full.values[0],{7});
  equal(full.values[1],{7});
  CHECK(full.values[2].kind == ValueSetKind::Unreachable);
  for (unsigned kind=0;kind<4;++kind) {
    CAPTURE(kind);
    FiniteValueLimits limits;
    if (kind == 0) limits.maxOperations = full.operations - 1;
    if (kind == 1) limits.maxFacts = 0;
    if (kind == 2) limits.maxBlocks = 0;
    if (kind == 3) limits.maxValues = 0;
    const auto result = finiteMemoryValues(graph,{0x1000},queries,limits);
    CHECK_FALSE(result.complete);
    REQUIRE(result.values.size() == queries.size());
    for (const auto &value : result.values) {
      CHECK(value.kind == ValueSetKind::Unknown);
      CHECK(value.values.empty());
    }
    CHECK(result.jumpTargets.at(0x1000).kind == ValueSetKind::Unknown);
  }
}
