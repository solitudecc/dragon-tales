#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <array>
#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
using O=ignil::Opcode;
const ignil::Reg ra{0x1000,64,{}},rb{0x2000,64,{}};
std::uint64_t mask(unsigned width) {return width==64?~std::uint64_t{0}:(std::uint64_t{1}<<width)-1;}
void add(Graph &graph,ignil::Block block) {
  const auto at=block.startAddress();graph.addIGNILBlock(std::move(block));graph.addBlock(at);
}
void jump(ignil::Block &block,std::uint64_t target) {
  block.append({O::JMP,{}, {ignil::Const{target,64}},block.startAddress()});
}
void input(ignil::Block &block,ignil::Reg reg,unsigned count,std::uint64_t factor,
           std::uint64_t fixed,std::uint32_t first) {
  const ignil::Temp raw{first,64},remainder{first+1,64},scaled{first+2,64};
  block.append({O::AND,raw,{reg,ignil::Const{63,64}},block.startAddress()});
  block.append({O::UMOD,remainder,{raw,ignil::Const{count,64}},block.startAddress()});
  block.append({O::MUL,scaled,{remainder,ignil::Const{factor,64}},block.startAddress()});
  block.append({O::OR,reg,{scaled,ignil::Const{fixed,64}},block.startAddress()});
}
FiniteValueResult run(unsigned a,unsigned b,std::uint64_t factorA,std::uint64_t factorB,
                      unsigned width,FiniteValueLimits limits={},std::uint64_t fixed=0) {
  Graph graph{Architecture::Unknown,{}};ignil::Block block{0x1000};
  input(block,ra,a,factorA,fixed,0);input(block,rb,b,factorB,0,3);
  const ignil::Temp result{6,ignil::Width(width)};
  block.append({O::XOR,result,{ra,rb},0x1000});block.append({O::JMP,{}, {result},0x1000});
  add(graph,std::move(block));const FiniteValueQuery query{0x1000,result};
  return finiteValues(graph,{0x1000},std::span{&query,1},limits);
}
void equal(const FiniteValueResult &actual,const std::set<std::uint64_t> &expected,unsigned maxValues) {
  REQUIRE(actual.complete);REQUIRE(actual.values.size()==1);
  if(expected.size()>maxValues) {
    CHECK(actual.values[0].kind==ValueSetKind::Unknown);CHECK(actual.values[0].values.empty());
  } else {
    REQUIRE(actual.values[0].kind==ValueSetKind::Finite);
    CHECK(actual.values[0].values==std::vector<std::uint64_t>{expected.begin(),expected.end()});
  }
}
std::set<std::uint64_t> product(unsigned a,unsigned b,std::uint64_t factorA,
                               std::uint64_t factorB,unsigned width,std::uint64_t fixed=0) {
  std::set<std::uint64_t> result;
  for(unsigned i=0;i<a;++i)for(unsigned j=0;j<b;++j)
    result.insert((((i*factorA)|fixed)^(j*factorB))&mask(width));
  return result;
}
FiniteValueResult hole(bool duplicateHighBits,FiniteValueLimits limits={}) {
  Graph graph{Architecture::Unknown,{}};
  ignil::Block entry{0x1000};
  entry.append({O::CJMP,{}, {ignil::Reg{0x3000,1,{}},ignil::Const{0x1010,64},ignil::Const{0x1020,64}},0x1000});
  add(graph,std::move(entry));
  for(auto [at,reg,to]:{std::tuple{0x1010,ra,0x1030},std::tuple{0x1040,rb,0x1060}}) {
    ignil::Block block{std::uint64_t(at)};
    const ignil::Temp raw{0,64};
    block.append({O::AND,raw,{reg,ignil::Const{31,64}},std::uint64_t(at)});
    block.append({O::UMOD,reg,{raw,ignil::Const{31,64}},std::uint64_t(at)});
    jump(block,to);add(graph,std::move(block));
  }
  for(auto [at,reg,to]:{std::tuple{0x1020,ra,0x1030},std::tuple{0x1050,rb,0x1060}}) {
    ignil::Block block{std::uint64_t(at)};
    block.append({O::COPY,reg,{ignil::Const{32,64}},std::uint64_t(at)});
    jump(block,to);add(graph,std::move(block));
  }
  ignil::Block joined{0x1030};
  joined.append({O::CJMP,{}, {ignil::Reg{0x3001,1,{}},ignil::Const{0x1040,64},ignil::Const{0x1050,64}},0x1030});
  add(graph,std::move(joined));
  ignil::Block out{0x1060};
  if(duplicateHighBits)for(auto [reg,id]:{std::pair{ra,0u},std::pair{rb,2u}}) {
    const ignil::Temp bit{id,64},shifted{id+1,64};
    out.append({O::AND,bit,{ignil::Reg{0x4000+id,64,{}},ignil::Const{1,64}},0x1060});
    out.append({O::SHL,shifted,{bit,ignil::Const{16,64}},0x1060});
    out.append({O::OR,reg,{reg,shifted},0x1060});
  }
  const ignil::Temp value{4,ignil::Width(duplicateHighBits?8:64)};
  out.append({O::XOR,value,{ra,rb},0x1060});out.append({O::JMP,{}, {value},0x1060});
  add(graph,std::move(out));const FiniteValueQuery query{0x1060,value};
  return finiteValues(graph,{0x1000},std::span{&query,1},limits);
}
}

TEST_CASE("Finite XOR proves dense correlated-bit sets within a small fixed budget",
          "[finite-values][finite-xor-span]") {
  for(bool calls:{false,true}) {
    FiniteValueLimits limits;limits.proveLeafCalls=calls;limits.maxOperations=4096;
    const auto result=run(54,54,257,257,64,limits);
    equal(result,product(54,54,257,257,64),64);CHECK(result.operations<4096);
  }
}

TEST_CASE("Finite XOR small spans match full products across bases and projections",
          "[finite-values][finite-xor-span]") {
  const std::array<std::uint64_t,4> factors{1,257,0x10001,(1ULL<<63)|1};
  for(auto [a,b]:{std::pair{32u,32u},std::pair{40u,40u},std::pair{54u,54u},std::pair{48u,12u}})
    for(auto factorA:factors)for(auto factorB:factors)for(unsigned width:{1,8,16,64}) {
      CAPTURE(a,b,factorA,factorB,width);
      equal(run(a,b,factorA,factorB,width,{},0x80000000),
            product(a,b,factorA,factorB,width,0x80000000),64);
    }
}

TEST_CASE("Finite XOR density requires strict inequality and deduplicated projections",
          "[finite-values][finite-xor-span]") {
  // A=B={0..30,32} has 32 members in a 64-element space. Its XOR image is
  // {0..62}; equality |A|+|B|=|H| cannot justify adding the missing value 63.
  std::set<std::uint64_t> expected;for(unsigned i=0;i<63;++i)expected.insert(i);
  for(bool duplicates:{false,true})for(bool calls:{false,true}) {
    CAPTURE(duplicates,calls);FiniteValueLimits limits;limits.proveLeafCalls=calls;
    // The duplicate variant first has 64 distinct 64-bit values on each side,
    // but truncation projects them back to the same 32-element set.
    const auto full=hole(duplicates,limits);equal(full,expected,64);
    limits.maxOperations=std::uint32_t(full.operations-1);
    const auto stopped=hole(duplicates,limits);
    CHECK_FALSE(stopped.complete);REQUIRE(stopped.values.size()==1);
    CHECK(stopped.values[0].kind==ValueSetKind::Unknown);CHECK(stopped.values[0].values.empty());
    for(const auto &[at,value]:stopped.jumpTargets)CHECK(value.kind==ValueSetKind::Unknown);
    limits.maxOperations=1000000;limits.maxValues=32;
    const auto oversized=hole(false,limits);REQUIRE(oversized.complete);
    REQUIRE(oversized.values.size()==1);CHECK(oversized.values[0].kind==ValueSetKind::Unknown);
  }
}
