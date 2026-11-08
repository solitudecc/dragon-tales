#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
using O=ignil::Opcode;
std::uint64_t wordMask(ignil::Width width) {
  return width==64?~std::uint64_t{0}:(std::uint64_t{1}<<width)-1;
}
std::vector<std::uint64_t> inputs(std::uint64_t variable,std::uint64_t fixed,
                                ignil::Width width,bool hole=false) {
  std::set<std::uint64_t> values;
  for(auto subset=variable;;subset=(subset-1)&variable) {
    values.insert(((hole?subset*3:subset)|fixed)&wordMask(width));
    if(!subset)break;
  }
  return {values.begin(),values.end()};
}
FiniteValueResult evaluate(std::uint64_t leftMask,std::uint64_t leftFixed,ignil::Width leftWidth,
                           std::uint64_t rightMask,std::uint64_t rightFixed,ignil::Width rightWidth,
                           ignil::Width width,FiniteValueLimits limits={},bool hole=false) {
  Graph graph{Architecture::X86_64,{}};ignil::Block block{0x1000};
  const ignil::Temp a{0,leftWidth},af{1,leftWidth},b{2,rightWidth},bf{3,rightWidth},out{4,width},raw{5,leftWidth};
  block.append({O::AND,raw,{ignil::Reg{0x1000,leftWidth,{}},ignil::Const{leftMask,leftWidth}},0x1000});
  if(hole)block.append({O::MUL,a,{raw,ignil::Const{3,leftWidth}},0x1000});
  else block.append({O::COPY,a,{raw},0x1000});
  block.append({O::OR,af,{a,ignil::Const{leftFixed,leftWidth}},0x1000});
  block.append({O::AND,b,{ignil::Reg{0x2000,rightWidth,{}},ignil::Const{rightMask,rightWidth}},0x1000});
  block.append({O::OR,bf,{b,ignil::Const{rightFixed,rightWidth}},0x1000});
  block.append({O::XOR,out,{af,bf},0x1000});
  block.append({O::JMP,{}, {out},0x1000});
  graph.addIGNILBlock(std::move(block));graph.addBlock(0x1000);
  const FiniteValueQuery query{0x1000,out};
  return finiteValues(graph,{0x1000},std::span{&query,1},limits);
}
void check(const FiniteValueResult &result,const std::vector<std::uint64_t> &left,
           const std::vector<std::uint64_t> &right,ignil::Width width,unsigned maxValues) {
  std::set<std::uint64_t> expected;
  for(auto a:left)for(auto b:right)expected.insert((a^b)&wordMask(width));
  REQUIRE(result.complete);REQUIRE(result.values.size()==1);
  const auto &actual=result.values.front();
  if(expected.size()>maxValues) {
    CHECK(actual.kind==ValueSetKind::Unknown);CHECK(actual.values.empty());
  } else {
    CHECK(actual.kind==ValueSetKind::Finite);
    CHECK(actual.values==std::vector<std::uint64_t>{expected.begin(),expected.end()});
  }
  CHECK(result.jumpTargets.at(0x1000)==actual);
}
}

TEST_CASE("Finite XOR computes complete bit cubes within the original small budget",
          "[finite-values][finite-xor-cost]") {
  for(bool calls:{false,true})for(unsigned bits:{3u,4u,6u,8u}) {
    CAPTURE(calls,bits);
    const auto variable=(std::uint64_t{1}<<bits)-1;
    FiniteValueLimits limits;limits.proveLeafCalls=calls;
    limits.maxValues=unsigned(variable+1);limits.maxOperations=4096;
    const auto result=evaluate(variable,0x100,64,variable,0x200,64,64,limits);
    check(result,inputs(variable,0x100,64),inputs(variable,0x200,64),64,limits.maxValues);
    CHECK(result.operations<4096);
  }
}

TEST_CASE("Finite XOR bit cubes match complete concrete products at mixed widths",
          "[finite-values][finite-xor-cost]") {
  struct Shape {std::uint64_t variable,fixed;};
  const std::array shapes{Shape{0x3f,0},Shape{0x55,0x80},Shape{0xf,0xa0},
    Shape{0x8000000000000015ULL,0x4000000000000080ULL},Shape{0x3f00,3}};
  for(auto leftWidth:{8,16,32,64})for(auto rightWidth:{8,16,32,64})
    for(auto width:{1,8,16,32,64})for(const auto &left:shapes)for(const auto &right:shapes) {
      CAPTURE(leftWidth,rightWidth,width,left.variable,right.variable);
      const auto lm=left.variable&wordMask(leftWidth),lf=left.fixed&wordMask(leftWidth);
      const auto rm=right.variable&wordMask(rightWidth),rf=right.fixed&wordMask(rightWidth);
      const auto result=evaluate(lm,lf,leftWidth,rm,rf,rightWidth,width);
      check(result,inputs(lm,lf,leftWidth),inputs(rm,rf,rightWidth),width,64);
    }
}

TEST_CASE("Finite XOR rejects sets with holes and respects result and operation limits",
          "[finite-values][finite-xor-cost]") {
  for(bool calls:{false,true}) {
    FiniteValueLimits limits;limits.proveLeafCalls=calls;
    // Eight multiples of three have correlated bits and are not a bit cube.
    check(evaluate(7,0,64,0x15,0x80,64,64,limits,true),
          inputs(7,0,64,true),inputs(0x15,0x80,64),64,64);
    check(evaluate(0x3f,0,64,0x3f00,0,64,64,limits),
          inputs(0x3f,0,64),inputs(0x3f00,0,64),64,64);
    limits.maxOperations=1;
    const auto exhausted=evaluate(0x3f,0,64,0x3f,0,64,64,limits);
    CHECK_FALSE(exhausted.complete);CHECK(exhausted.operations==1);
    REQUIRE(exhausted.values.size()==1);
    CHECK(exhausted.values.front().kind==ValueSetKind::Unknown);
    CHECK(exhausted.values.front().values.empty());
    CHECK(exhausted.jumpTargets.at(0x1000).kind==ValueSetKind::Unknown);
  }
}
