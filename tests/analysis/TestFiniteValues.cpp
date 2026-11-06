#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#ifdef DRAGON_WITH_Z3
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif
#include <array>
using namespace dragon;
using namespace dragon::analysis;
using O=ignil::Opcode;
namespace {
void add(Graph &g,ignil::Block b) {auto at=b.startAddress();g.addIGNILBlock(std::move(b));g.addBlock(at);}
void jump(ignil::Block &b,std::uint64_t to) {b.append({O::JMP,{}, {ignil::Const{to,64}},b.startAddress()});}
FiniteValues query(const Graph &g,std::uint64_t at,ignil::Value value,
                   const std::set<std::uint64_t> &entries={0x1000}) {
  FiniteValueQuery q{at,std::move(value)};
  auto r=finiteValues(g,entries,std::span{&q,1});
  REQUIRE(r.complete);REQUIRE(r.values.size()==1);return r.values[0];
}
void equals(const FiniteValues &v,std::vector<std::uint64_t> expected) {
  REQUIRE(v.kind==ValueSetKind::Finite);CHECK(v.values==expected);
}
}
TEST_CASE("Finite values join complete paths without changing constant slices","[finite-values]") {
  Graph g{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};
  entry.append({O::CJMP,{}, {x86::ZF,ignil::Const{0x1010,64},ignil::Const{0x1020,64}},0x1000});add(g,std::move(entry));
  for(auto [at,value]:{std::pair{0x1010,7},std::pair{0x1020,9}}) {
    ignil::Block arm{std::uint64_t(at)};arm.append({O::COPY,x86::EAX,{ignil::Const{std::uint64_t(value),32}},std::uint64_t(at)});
    jump(arm,0x1030);add(g,std::move(arm));
  }
  ignil::Block last{0x1030};last.append({O::JMP,{}, {x86::RAX},0x1030});add(g,std::move(last));
  auto r=finiteValues(g,{0x1000});REQUIRE(r.complete);equals(r.jumpTargets.at(0x1030),{7,9});
  CHECK(r.toJSON().starts_with("{"));
  auto old=BackwardSlicer{16}.slice(x86::RAX,0x1030,g);
  CHECK_FALSE(std::holds_alternative<slice::Const>(old.data));
  // A join which is also an independent entry has unknown input state.
  CHECK(query(g,0x1030,x86::RAX,{0x1000,0x1030}).kind==ValueSetKind::Unknown);
}
TEST_CASE("Finite values grow to a loop fixed point and unknown paths stay unknown","[finite-values]") {
  Graph g{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};entry.append({O::COPY,x86::EAX,{ignil::Const{1,32}},0x1000});jump(entry,0x1010);add(g,std::move(entry));
  ignil::Block test{0x1010};test.append({O::CJMP,{}, {x86::ZF,ignil::Const{0x1020,64},ignil::Const{0x1030,64}},0x1010});add(g,std::move(test));
  ignil::Block body{0x1020};
  bool unknown=false,growing=false;
  SECTION("constant redefinition") {body.append({O::COPY,x86::EAX,{ignil::Const{7,32}},0x1020});}
  SECTION("unknown path") {unknown=true;body.append({O::COPY,x86::EAX,{x86::EDI},0x1020});}
  SECTION("unbounded increment") {growing=true;body.append({O::ADD,x86::EAX,{x86::EAX,ignil::Const{1,32}},0x1020});}
  jump(body,0x1010);add(g,std::move(body));
  ignil::Block out{0x1030};out.append({O::JMP,{}, {x86::RAX},0x1030});add(g,std::move(out));
  auto r=finiteValues(g,{0x1000});REQUIRE(r.complete);
  if(unknown || growing)CHECK(r.jumpTargets.at(0x1030).kind==ValueSetKind::Unknown);
  else equals(r.jumpTargets.at(0x1030),{1,7});
}
TEST_CASE("Finite values preserve stack table initialization through loops","[finite-values][finite-memory]") {
  Graph g{Architecture::X86_64,{}};
  const ignil::Temp p{0,64},index{1,64},address{2,64},target{3,64};
  ignil::Block entry{0x1000};
  for(unsigned i=0;i<2;++i) {
    const ignil::Temp slot{i,64};
    entry.append({O::ADD,slot,{x86::RSP,ignil::Const{16+i*8,64}},0x1000});
    entry.append({O::STORE,{}, {slot,ignil::Const{0x2000+i*0x100,64}},0x1000});
  }
  entry.append({O::COPY,x86::ECX,{ignil::Const{0,32}},0x1000});jump(entry,0x1010);add(g,std::move(entry));
  ignil::Block test{0x1010};test.append({O::CJMP,{}, {x86::ZF,ignil::Const{0x1020,64},ignil::Const{0x1030,64}},0x1010});add(g,std::move(test));
  ignil::Block body{0x1020};body.append({O::COPY,x86::ECX,{ignil::Const{1,32}},0x1020});
  bool barrier=false;
  SECTION("unchanged table") {}
  SECTION("disjoint slot") {body.append({O::ADD,p,{x86::RSP,ignil::Const{40,64}},0x1020});body.append({O::STORE,{}, {p,x86::RDI},0x1020});}
  SECTION("call barrier") {barrier=true;body.append({O::CALL,{}, {ignil::Const{0x5000,64}},0x1020});}
  SECTION("possible alias") {barrier=true;body.append({O::STORE,{}, {x86::RDI,ignil::Const{0,64}},0x1020});}
  jump(body,0x1010);add(g,std::move(body));
  ignil::Block out{0x1030};
  out.append({O::SHL,index,{x86::RCX,ignil::Const{3,64}},0x1030});
  out.append({O::ADD,p,{x86::RSP,ignil::Const{16,64}},0x1030});
  out.append({O::ADD,address,{p,index},0x1030});
  out.append({O::LOAD,target,{address},0x1030});out.append({O::JMP,{}, {target},0x1030});add(g,std::move(out));
  auto r=finiteValues(g,{0x1000});REQUIRE(r.complete);
  if(barrier)CHECK(r.jumpTargets.at(0x1030).kind==ValueSetKind::Unknown);
  else equals(r.jumpTargets.at(0x1030),{0x2000,0x2100});
}
TEST_CASE("Finite values honor subregister snapshots and zero upper bits","[finite-values]") {
  for(auto arch:{Architecture::X86_64,Architecture::ARM64}) {
    const auto wide=arch==Architecture::X86_64?x86::RAX:arm::X0;
    const auto narrow=arch==Architecture::X86_64?x86::EAX:arm::W0;
    Graph g{arch,{}};ignil::Block entry{0x1000};
    const ignil::Temp snapshot{0,64};
    entry.append({O::COPY,wide,{ignil::Const{0x12345678abcdef01ULL,64}},0x1000});
    entry.append({O::COPY,snapshot,{wide},0x1000});
    entry.append({O::COPY,narrow,{ignil::Const{0x87654321,32}},0x1000});
    entry.append({O::RET,{}, {},0x1000});add(g,std::move(entry));
    equals(query(g,0x1000,wide),{0x87654321});
    equals(query(g,0x1000,snapshot),{0x12345678abcdef01ULL});
  }
  Graph g{Architecture::X86_64,{}};ignil::Block b{0x1000};
  b.append({O::COPY,x86::RAX,{ignil::Const{0x123456789abcdef0ULL,64}},0x1000});
  b.append({O::COPY,x86::AH,{ignil::Const{0x42,8}},0x1000});add(g,std::move(b));
  equals(query(g,0x1000,x86::RAX),{0x123456789abc42f0ULL});
}
TEST_CASE("Finite values distinguish unreachable and unknown and fail closed on budgets","[finite-values]") {
  Graph g{Architecture::X86_64,{}};ignil::Block entry{0x1000};
  entry.append({O::COPY,x86::EAX,{ignil::Const{7,32}},0x1000});add(g,std::move(entry));
  ignil::Block dead{0x2000};dead.append({O::JMP,{}, {x86::RAX},0x2000});add(g,std::move(dead));
  auto r=finiteValues(g,{0x1000});REQUIRE(r.complete);CHECK(r.jumpTargets.at(0x2000).kind==ValueSetKind::Unreachable);
  CHECK(query(g,0x1000,x86::RDI).kind==ValueSetKind::Unknown);
  const FiniteValueQuery q{0x1000,x86::RAX};
  for(unsigned kind=0;kind<4;++kind) {
    FiniteValueLimits limits;
    if(kind==0)limits.maxOperations=1;
    if(kind==1)limits.maxFacts=0;
    if(kind==2)limits.maxBlocks=0;
    if(kind==3)limits.maxValues=0;
    r=finiteValues(g,{0x1000},std::span{&q,1},limits);
    CHECK_FALSE(r.complete);CHECK(r.values[0].kind==ValueSetKind::Unknown);
    CHECK(r.jumpTargets.at(0x2000).kind==ValueSetKind::Unknown);
    CHECK(r.jumpTargets.at(0x2000).values.empty());
  }
  Graph cyclic{Architecture::X86_64,{}};ignil::Block loop{0x2000};
  jump(loop,0x2000);add(cyclic,std::move(loop));
  const FiniteValueQuery input{0x2000,x86::RAX};
  auto roots=finiteValues(cyclic,{},std::span{&input,1});
  REQUIRE(roots.complete);CHECK(roots.values[0].kind==ValueSetKind::Unknown);
}
TEST_CASE("Finite values handle overwrites and wrapping concrete memory","[finite-values][finite-memory]") {
  Graph g{Architecture::X86_64,{}};ignil::Block b{0x1000};
  const ignil::Temp out{0,64};
  b.append({O::STORE,{}, {ignil::Const{~std::uint64_t{0}-3,64},ignil::Const{0x1122334455667788ULL,64}},0x1000});
  b.append({O::STORE,{}, {ignil::Const{0,64},ignil::Const{0xabcd,16}},0x1000});
  b.append({O::LOAD,out,{ignil::Const{~std::uint64_t{0}-3,64}},0x1000});add(g,std::move(b));
  equals(query(g,0x1000,out),{0x1122abcd55667788ULL});
}

TEST_CASE("Finite values retain zero upper bytes but not unsupported effects","[finite-values]") {
  Graph g{Architecture::ARM64,{}};ignil::Block b{0x1000};
  b.append({O::UNDEF,arm::S0,{},0x1000});add(g,std::move(b));
  equals(query(g,0x1000,ignil::Reg{arm::V0.offset+8,64,"upper"}),{0});
  CHECK(query(g,0x1000,arm::S0).kind==ValueSetKind::Unknown);
  for(bool local:{false,true}) {
    Graph x{Architecture::X86_64,{}};ignil::Block block{0x1000};
    block.append({O::COPY,x86::RAX,{ignil::Const{7,64}},0x1000});
    if(local)block.append({O::LABEL,{}, {},0x1000});
    else block.noteUnlifted(0x1000);
    add(x,std::move(block));
    CHECK(query(x,0x1000,x86::RAX).kind==ValueSetKind::Unknown);
  }
}
TEST_CASE("Finite masks do not invent initialized jump table entries","[finite-values][finite-memory]") {
  Graph g{Architecture::X86_64,{}};ignil::Block b{0x1000};
  const ignil::Temp masked{0,64},scaled{1,64},base{2,64},pointer{3,64},loaded{4,64};
  b.append({O::AND,masked,{x86::RDI,ignil::Const{3,64}},0x1000});
  b.append({O::SHL,scaled,{masked,ignil::Const{3,64}},0x1000});
  b.append({O::ADD,base,{x86::RSP,ignil::Const{16,64}},0x1000});
  b.append({O::STORE,{}, {base,ignil::Const{0x2000,64}},0x1000});
  const ignil::Temp second{5,64};
  b.append({O::ADD,second,{base,ignil::Const{8,64}},0x1000});
  b.append({O::STORE,{}, {second,ignil::Const{0x2100,64}},0x1000});
  b.append({O::ADD,pointer,{base,scaled},0x1000});
  b.append({O::LOAD,loaded,{pointer},0x1000});add(g,std::move(b));
  equals(query(g,0x1000,masked),{0,1,2,3});
  CHECK(query(g,0x1000,loaded).kind==ValueSetKind::Unknown);
}
TEST_CASE("Finite stack pointer differences do not cross a widening wrap","[finite-values]") {
  Graph g{Architecture::X86_32,{}};ignil::Block b{0x1000};
  const ignil::Temp prior{0,32},narrow{1,32},wide{2,64};
  b.append({O::SUB,prior,{x86::ESP,ignil::Const{1,32}},0x1000});
  b.append({O::SUB,narrow,{prior,x86::ESP},0x1000});
  b.append({O::SUB,wide,{prior,x86::ESP},0x1000});
  add(g,std::move(b));
  equals(query(g,0x1000,narrow),{0xffffffff});
  CHECK(query(g,0x1000,wide).kind==ValueSetKind::Unknown);
}
#ifdef DRAGON_WITH_Z3
TEST_CASE("Finite integer transfers agree with symbolic IGNIL at mixed widths","[finite-values][finite-arithmetic]") {
  const std::array<ignil::Width,5> widths{1,8,16,32,64};
  const std::array<std::uint64_t,10> edges{0,1,2,7,63,64,65,1ULL<<63,0x7fffffffffffffffULL,~0ULL};
  const std::array binary{O::ADD,O::SUB,O::MUL,O::AND,O::OR,O::XOR,O::SHL,O::SHR,O::SAR,
    O::EQ,O::NEQ,O::ULT,O::ULE,O::UGT,O::UGE,O::SLT,O::SLE,O::SGT,O::SGE};
  ignil::Block block{0x1000};std::vector<ignil::Temp> outputs;
  std::vector<FiniteValueQuery> queries;
  auto emit=[&](O op,ignil::Width dst,std::vector<ignil::Value> sources) {
    const ignil::Temp t{std::uint32_t(outputs.size()),dst};
    block.append({op,t,std::move(sources),0x1000});
    outputs.push_back(t);queries.push_back({0x1000,t});
  };
  for(auto op:binary)for(unsigned i=0;i<widths.size();++i)
    for(unsigned j=0;j<widths.size();++j)for(unsigned n=0;n<edges.size();++n)
      emit(op,widths[(i+j+n)%widths.size()],
        {ignil::Const{edges[n],widths[i]},ignil::Const{edges[(n+3)%edges.size()],widths[j]}});
  for(auto op:{O::COPY,O::NEG,O::NOT,O::ZEXT,O::SEXT,O::TRUNC})
    for(auto src:widths)for(auto dst:widths)for(auto value:edges)
      emit(op,dst,{ignil::Const{value,src}});
  ignil::SymbolicExecutor reference;
  REQUIRE(reference.run(block).ok());
  Graph g{Architecture::X86_64,{}};add(g,std::move(block));
  auto actual=finiteValues(g,{0x1000},queries);
  REQUIRE(actual.complete);REQUIRE(actual.values.size()==outputs.size());
  for(std::size_t i=0;i<outputs.size();++i) {
    INFO("operation "<<i<<" "<<ignil::toString(g.getIGNILBlock(0x1000)->ops()[i]));
    const auto expected=reference.readTemp(outputs[i]);REQUIRE(expected);
    const auto value=reference.evaluate(*expected);REQUIRE(value);
    REQUIRE(actual.values[i].kind==ValueSetKind::Finite);
    CHECK(actual.values[i].values==std::vector<std::uint64_t>{*value});
  }
}
#endif
